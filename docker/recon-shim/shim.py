#!/usr/bin/env python3
"""
HTTP -> TCP shim between marshal and python-ismrmrd-server.

Marshal POSTs raw k-space (AcquisitionHeader + samples, concatenated) to
/reconstruct. The shim:

  1. Parses the body into individual ISMRMRD acquisitions.
  2. Fabricates a minimal XML dataset header from the first acquisition.
  3. Opens a TCP connection to python-ismrmrd-server on localhost:9002.
  4. Sends: CONFIG_TEXT("simplefft") -> METADATA_XML_TEXT(header) ->
     ISMRMRD_ACQUISITION * N -> CLOSE.
  5. Reads back ISMRMRD_IMAGE messages and strips python-ismrmrd-server's
     attribute envelope.
  6. POSTs each image (198-byte ImageHeader + raw pixels) to the X-MRD-Callback
     URL marshal provided, preserving X-MRD-Stream / X-MRD-Session /
     X-MRD-Job-Id headers.
  7. Returns HTTP 202 to marshal immediately; the actual work happens in a
     background thread.
"""

import ctypes
import logging
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import ismrmrd
import numpy as np
import requests

SHIM_PORT = 9003
RECON_HOST = "127.0.0.1"
RECON_PORT = 9002
RECON_CONFIG = "simplefft"  # Inverse FFT - realistic recon for k-space

# ISMRMRD wire protocol constants (match constants.py in python-ismrmrd-server)
MRD_MESSAGE_CONFIG_TEXT = 2
MRD_MESSAGE_METADATA_XML_TEXT = 3
MRD_MESSAGE_CLOSE = 4
MRD_MESSAGE_ISMRMRD_ACQUISITION = 1008
MRD_MESSAGE_ISMRMRD_IMAGE = 1022

# Marshal's callback expects a flat 198-byte ImageHeader + pixel bytes.
IMAGEHEADER_SIZE = 198
ACQUISITION_HEADER_SIZE = 340  # sizeof(ISMRMRD::AcquisitionHeader)

logging.basicConfig(
    level=logging.INFO, format="[shim] %(asctime)s %(levelname)s %(message)s"
)
log = logging.getLogger("shim")


# ---------------------------------------------------------------------------
# k-space body parsing (borrowed from tests/mock_recon_service.py)
# ---------------------------------------------------------------------------


def parse_acquisitions(raw: bytes):
    """Return a list of ismrmrd.Acquisition objects.

    Uses ismrmrd.Acquisition.deserialize_from, which knows the real field
    offsets (e.g. trajectory_dimensions at 176, not 36). We feed it a read()
    callable that pulls from an in-memory buffer.
    """
    import io

    stream = io.BytesIO(raw)

    def reader(nbytes: int) -> bytes:
        return stream.read(nbytes)

    acqs = []
    while stream.tell() < len(raw):
        try:
            acq = ismrmrd.Acquisition.deserialize_from(reader)
        except Exception as e:
            log.warning("stopped parsing at byte %d: %s", stream.tell(), e)
            break
        acqs.append(acq)
    return acqs


def fabricate_xml_header(number_of_samples: int, active_channels: int, num_lines: int) -> str:
    """Minimal ISMRMRD XML header. simplefft needs the encoded matrix size
    and reconSpace matrix (recon crops oversampled readout 2x to that).
    """
    nx = number_of_samples
    ny = num_lines
    rx = nx // 2 if nx % 2 == 0 else nx  # simple 2x readout deoversampling
    ry = ny
    return f"""<?xml version="1.0"?>
<ismrmrdHeader xmlns="http://www.ismrm.org/ISMRMRD" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <acquisitionSystemInformation>
    <systemVendor>marshal-shim</systemVendor>
    <systemModel>simulator</systemModel>
    <systemFieldStrength_T>1.5</systemFieldStrength_T>
    <receiverChannels>{active_channels}</receiverChannels>
  </acquisitionSystemInformation>
  <experimentalConditions>
    <H1resonanceFrequency_Hz>63870000</H1resonanceFrequency_Hz>
  </experimentalConditions>
  <encoding>
    <trajectory>cartesian</trajectory>
    <encodedSpace>
      <matrixSize><x>{nx}</x><y>{ny}</y><z>1</z></matrixSize>
      <fieldOfView_mm><x>256</x><y>256</y><z>5</z></fieldOfView_mm>
    </encodedSpace>
    <reconSpace>
      <matrixSize><x>{rx}</x><y>{ry}</y><z>1</z></matrixSize>
      <fieldOfView_mm><x>256</x><y>256</y><z>5</z></fieldOfView_mm>
    </reconSpace>
    <encodingLimits>
      <kspace_encoding_step_1><minimum>0</minimum><maximum>{ny - 1}</maximum><center>{ny // 2}</center></kspace_encoding_step_1>
    </encodingLimits>
  </encoding>
</ismrmrdHeader>"""


# ---------------------------------------------------------------------------
# TCP send/recv helpers for python-ismrmrd-server wire format
# ---------------------------------------------------------------------------


def send_all(sock: socket.socket, data) -> None:
    # Accept bytes, bytearray, memoryview, or numpy arrays. Coerce to bytes
    # so we always have something memoryview can slice cleanly.
    if not isinstance(data, (bytes, bytearray, memoryview)):
        data = bytes(data)
    view = memoryview(data).cast("B")  # flatten to 1D byte view
    while len(view):
        n = sock.send(view)
        view = view[n:]


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError(f"socket closed after {len(buf)}/{n} bytes")
        buf.extend(chunk)
    return bytes(buf)


def send_config_text(sock: socket.socket, text: str) -> None:
    payload = (text + "\0").encode()
    send_all(sock, struct.pack("<H", MRD_MESSAGE_CONFIG_TEXT))
    send_all(sock, struct.pack("<I", len(payload)))
    send_all(sock, payload)


def send_metadata_xml(sock: socket.socket, xml: str) -> None:
    payload = (xml + "\0").encode()
    send_all(sock, struct.pack("<H", MRD_MESSAGE_METADATA_XML_TEXT))
    send_all(sock, struct.pack("<I", len(payload)))
    send_all(sock, payload)


def send_acquisition_record(sock: socket.socket, acq) -> None:
    """Send one acquisition using ismrmrd's own serializer.

    Writes the 2-byte message id, then delegates to acq.serialize_into which
    emits header (340B) + trajectory + samples in the exact format the server
    expects.
    """
    send_all(sock, struct.pack("<H", MRD_MESSAGE_ISMRMRD_ACQUISITION))
    acq.serialize_into(lambda chunk: send_all(sock, chunk))


def send_close(sock: socket.socket) -> None:
    send_all(sock, struct.pack("<H", MRD_MESSAGE_CLOSE))


# ---------------------------------------------------------------------------
# Reading images back from python-ismrmrd-server
# ---------------------------------------------------------------------------


def read_message_id(sock: socket.socket):
    b = recv_exact(sock, 2)
    return struct.unpack("<H", b)[0]


def read_image_message(sock: socket.socket):
    """Read one MRD_MESSAGE_ISMRMRD_IMAGE. Returns (header_bytes, pixel_bytes).

    Wire layout (see connection.py:326 comments):
        2 bytes  id (already consumed by caller)
      198 bytes  ImageHeader
        8 bytes  attribute length (uint64)
      var bytes  attribute string
      var bytes  pixel data (size derived from header)
    """
    header_bytes = recv_exact(sock, IMAGEHEADER_SIZE)

    # Use ismrmrd to introspect the header so we know how many pixel bytes to read.
    image_header = ismrmrd.ImageHeader.from_buffer_copy(header_bytes)
    attr_len = struct.unpack("<Q", recv_exact(sock, 8))[0]
    _attr = recv_exact(sock, attr_len)  # discarded - marshal doesn't want attributes

    mx = image_header.matrix_size[0]
    my = image_header.matrix_size[1]
    mz = image_header.matrix_size[2] if image_header.matrix_size[2] else 1
    channels = image_header.channels if image_header.channels else 1
    nentries = mx * my * mz * channels

    # Map ISMRMRD data_type -> bytes per element
    dt_bytes = {
        1: 2,  # USHORT
        2: 2,  # SHORT
        3: 4,  # UINT
        4: 4,  # INT
        5: 4,  # FLOAT
        6: 8,  # DOUBLE
        7: 8,  # CXFLOAT  (2x float)
        8: 16,  # CXDOUBLE (2x double)
    }.get(image_header.data_type, 4)

    pixel_bytes = recv_exact(sock, nentries * dt_bytes)
    return header_bytes, pixel_bytes


# ---------------------------------------------------------------------------
# Worker: run one reconstruction round-trip
# ---------------------------------------------------------------------------


def run_reconstruction(
    raw_kspace: bytes,
    callback_url: str,
    stream: str,
    session: str,
    job_id: str,
) -> None:
    try:
        log.info("[%s] parsing %d bytes of k-space", job_id, len(raw_kspace))
        acqs = parse_acquisitions(raw_kspace)
        if not acqs:
            log.error("[%s] no valid acquisitions in body", job_id)
            return
        log.info("[%s] %d acquisitions", job_id, len(acqs))

        first_ns = int(acqs[0].number_of_samples)
        first_chan = int(acqs[0].active_channels) or 1
        # Count distinct phase encode lines (kspace_encoding_step_1).
        ny = max(int(a.idx.kspace_encode_step_1) for a in acqs) + 1
        xml = fabricate_xml_header(first_ns, first_chan, ny)

        log.info("[%s] connecting to %s:%d", job_id, RECON_HOST, RECON_PORT)
        sock = socket.create_connection((RECON_HOST, RECON_PORT), timeout=30)
        try:
            send_config_text(sock, RECON_CONFIG)
            send_metadata_xml(sock, xml)
            sent = 0
            for acq in acqs:
                send_acquisition_record(sock, acq)
                sent += 1
            send_close(sock)
            # Half-close the write side so the server sees EOF on any further
            # reads after CLOSE rather than misinterpreting following bytes.
            try:
                sock.shutdown(socket.SHUT_WR)
            except Exception:
                pass
            log.info("[%s] sent %d acquisitions + CLOSE, reading images back", job_id, sent)

            images = []
            while True:
                try:
                    mid = read_message_id(sock)
                except ConnectionError:
                    break
                if mid == MRD_MESSAGE_ISMRMRD_IMAGE:
                    images.append(read_image_message(sock))
                elif mid == MRD_MESSAGE_CLOSE:
                    log.info("[%s] server sent CLOSE", job_id)
                    break
                else:
                    log.warning("[%s] unexpected message id %d, stopping read", job_id, mid)
                    break
        finally:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except Exception:
                pass
            sock.close()

        if not images:
            log.error("[%s] no images returned", job_id)
            return
        log.info("[%s] got %d images, combining into one 3D frame", job_id, len(images))

        # Combine all returned 2D images into a single 3D ImageHeader+pixels
        # so marshal stores it as one multi-slice frame. Rewrite matrix_size[2]
        # in the first header to the slice count, concatenate pixel payloads.
        first_hdr = bytearray(images[0][0])
        struct.pack_into("<H", first_hdr, 20, len(images))  # matrix_size[2]
        combined_pixels = b"".join(px for _, px in images)
        body = bytes(first_hdr) + combined_pixels

        try:
            r = requests.post(
                callback_url,
                data=body,
                headers={
                    "Content-Type": "application/octet-stream",
                    "X-MRD-Stream": stream,
                    "X-MRD-Session": session,
                    "X-MRD-Job-Id": job_id,
                },
                timeout=30,
            )
            log.info(
                "[%s] %d-slice volume -> marshal: %d",
                job_id,
                len(images),
                r.status_code,
            )
        except Exception as e:
            log.error("[%s] callback POST failed: %s", job_id, e)
    except Exception as e:
        log.exception("[%s] reconstruction failed: %s", job_id, e)


# ---------------------------------------------------------------------------
# HTTP front end
# ---------------------------------------------------------------------------


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        log.info("HTTP %s", fmt % args)

    def do_GET(self):
        if self.path == "/health":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"status":"ok","service":"recon-shim"}')
            return
        self.send_response(404)
        self.end_headers()

    def do_POST(self):
        if self.path != "/reconstruct":
            self.send_response(404)
            self.end_headers()
            return

        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length else b""
        stream = self.headers.get("X-MRD-Stream", "unknown")
        callback = self.headers.get("X-MRD-Callback", "")
        session = self.headers.get("X-MRD-Session", "")
        job_id = self.headers.get("X-MRD-Job-Id", f"job_{int(time.time() * 1000)}")

        log.info(
            "received /reconstruct: %d bytes, stream=%s, job=%s, callback=%s",
            len(body),
            stream,
            job_id,
            callback,
        )

        if not callback:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(b'{"error":"X-MRD-Callback header is required"}')
            return

        threading.Thread(
            target=run_reconstruction,
            args=(body, callback, stream, session, job_id),
            daemon=True,
        ).start()

        self.send_response(202)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(
            f'{{"status":"processing","job_id":"{job_id}","stream":"{stream}"}}'.encode()
        )


def main():
    server = ThreadingHTTPServer(("0.0.0.0", SHIM_PORT), Handler)
    log.info(
        "shim listening on :%d, forwards to python-ismrmrd-server %s:%d",
        SHIM_PORT,
        RECON_HOST,
        RECON_PORT,
    )
    server.serve_forever()


if __name__ == "__main__":
    sys.exit(main() or 0)
