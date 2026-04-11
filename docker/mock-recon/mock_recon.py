#!/usr/bin/env python3
"""
Mock Reconstruction Server — MRD TCP protocol

TCP server on port 9002 that speaks the same wire protocol as
python-ismrmrd-server (2-byte message ID framing).

Accepts: CONFIG_FILE(1), CONFIG_TEXT(2), METADATA_XML(3),
         ACQUISITION(1008), WAVEFORM(1026), IMAGE(1022), CLOSE(4)

Does simple FFT reconstruction on acquisitions and sends images
back to the client (marshal) over the same TCP connection using
MRD_MESSAGE_ISMRMRD_IMAGE (1022).

Waveforms and unknown messages are logged and skipped.
"""

import argparse
import ctypes
import logging
import os
import socket
import struct
import sys
import threading

import numpy as np

try:
    import ismrmrd
except ImportError:
    print("ERROR: ismrmrd package required. pip install ismrmrd", file=sys.stderr)
    sys.exit(1)

logging.basicConfig(level=logging.INFO, format='%(asctime)s [mock_recon] %(message)s')
log = logging.getLogger('mock_recon')

# MRD message IDs (from python-ismrmrd-server constants.py)
MRD_MESSAGE_CONFIG_FILE        = 1
MRD_MESSAGE_CONFIG_TEXT        = 2
MRD_MESSAGE_METADATA_XML_TEXT  = 3
MRD_MESSAGE_CLOSE              = 4
MRD_MESSAGE_TEXT               = 5
MRD_MESSAGE_ISMRMRD_ACQUISITION = 1008
MRD_MESSAGE_ISMRMRD_IMAGE      = 1022
MRD_MESSAGE_ISMRMRD_WAVEFORM   = 1026

SIZEOF_MRD_MESSAGE_IDENTIFIER = 2  # uint16
SIZEOF_MRD_MESSAGE_LENGTH     = 4  # uint32
SIZEOF_CONFIG_FILE             = 1024


def read_exact(sock, n):
    """Read exactly n bytes from socket."""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("Connection closed")
        buf.extend(chunk)
    return bytes(buf)


def reconstruct_slice(kspace_lines, nx, ny):
    """Simple FFT reconstruction."""
    kspace = np.zeros((ny, nx), dtype=np.complex64)
    for line_idx, data in kspace_lines:
        if 0 <= line_idx < ny:
            nsamples = min(len(data), nx)
            kspace[line_idx, :nsamples] = data[:nsamples]
    image = np.fft.fftshift(np.fft.ifft2(np.fft.ifftshift(kspace)))
    return np.abs(image).astype(np.float32)


def send_image(sock, image_data, image_series, slice_idx=0):
    """Send an image back to the client using ismrmrd package (no hand-rolled offsets)."""
    ny, nx = image_data.shape

    # Use the ismrmrd package to build a proper Image object
    img = ismrmrd.Image.from_array(image_data.reshape(1, 1, ny, nx), transpose=False)
    img.image_series_index = image_series
    img.slice = slice_idx  # Spatial slice index (per mrdhelper.update_img_header_from_raw)
    # data_type is set automatically by from_array based on dtype (float32 → FLOAT)

    # Send MRD_MESSAGE_ISMRMRD_IMAGE tag + serialize via ismrmrd
    sock.sendall(struct.pack('<H', MRD_MESSAGE_ISMRMRD_IMAGE))
    img.serialize_into(sock.sendall)

    log.info(f"Sent image ({nx}x{ny}) series={image_series} slice={slice_idx}")


def handle_connection(conn, addr):
    """Handle one scanner/marshal connection."""
    log.info(f"Connection from {addr}")

    xml_header = None
    config = None
    enc_nx = 128
    enc_ny = 128
    acq_count = 0
    image_series = 0
    kspace_buffer = {}  # slice_idx -> [(line_idx, complex64 array)]

    try:
        while True:
            # Read 2-byte message ID
            id_bytes = read_exact(conn, SIZEOF_MRD_MESSAGE_IDENTIFIER)
            msg_id = struct.unpack('<H', id_bytes)[0]

            if msg_id == MRD_MESSAGE_CONFIG_FILE:
                data = read_exact(conn, SIZEOF_CONFIG_FILE)
                config = data.split(b'\x00', 1)[0].decode('utf-8')
                log.info(f"CONFIG_FILE: {config}")

            elif msg_id == MRD_MESSAGE_CONFIG_TEXT:
                length = struct.unpack('<I', read_exact(conn, 4))[0]
                data = read_exact(conn, length)
                config = data.split(b'\x00', 1)[0].decode('utf-8')
                log.info(f"CONFIG_TEXT: {config}")

            elif msg_id == MRD_MESSAGE_METADATA_XML_TEXT:
                length = struct.unpack('<I', read_exact(conn, 4))[0]
                data = read_exact(conn, length)
                xml_header = data.split(b'\x00', 1)[0].decode('utf-8')
                # Parse encoding size
                import re
                mx = re.search(r'<x>(\d+)</x>', xml_header)
                my = re.search(r'<y>(\d+)</y>', xml_header)
                if mx: enc_nx = int(mx.group(1))
                if my: enc_ny = int(my.group(1))
                acq_count = 0
                kspace_buffer = {}
                log.info(f"METADATA_XML: {len(xml_header)}B, enc={enc_nx}x{enc_ny}")

            elif msg_id == MRD_MESSAGE_CLOSE:
                log.info(f"CLOSE (received {acq_count} acquisitions)")
                # Flush remaining slices
                for flush_slice_idx, lines in kspace_buffer.items():
                    if lines:
                        img = reconstruct_slice(lines, enc_nx, enc_ny)
                        send_image(conn, img, image_series, flush_slice_idx)
                        image_series += 1
                kspace_buffer = {}
                acq_count = 0
                # Send CLOSE back but keep connection open for next volume
                conn.sendall(struct.pack('<H', MRD_MESSAGE_CLOSE))
                # Don't break — loop back and wait for next CONFIG/data on same connection

            elif msg_id == MRD_MESSAGE_TEXT:
                length = struct.unpack('<I', read_exact(conn, 4))[0]
                data = read_exact(conn, length)
                text = data.split(b'\x00', 1)[0].decode('utf-8')
                log.info(f"TEXT: {text}")

            elif msg_id == MRD_MESSAGE_ISMRMRD_ACQUISITION:
                acq = ismrmrd.Acquisition.deserialize_from(
                    lambda n: read_exact(conn, n))
                acq_count += 1

                h = acq.getHead()
                slice_idx = h.idx.slice
                line_idx = h.idx.kspace_encode_step_1
                nsamples = h.number_of_samples

                # First channel data
                samples = acq.data[0, :nsamples].astype(np.complex64)

                if slice_idx not in kspace_buffer:
                    kspace_buffer[slice_idx] = []
                kspace_buffer[slice_idx].append((line_idx, samples))

                # Check LAST_IN_SLICE flag (ISMRMRD_ACQ_LAST_IN_SLICE=8, bit index 7)
                if h.flags & (1 << 7):
                    lines = kspace_buffer.pop(slice_idx, [])
                    if lines:
                        img = reconstruct_slice(lines, enc_nx, enc_ny)
                        send_image(conn, img, image_series, slice_idx)
                        image_series += 1

                if acq_count % 100 == 0:
                    log.info(f"Received {acq_count} acquisitions")

            elif msg_id == MRD_MESSAGE_ISMRMRD_IMAGE:
                # Read and skip image from scanner
                img = ismrmrd.Image.deserialize_from(
                    lambda n: read_exact(conn, n))
                log.info(f"IMAGE from client ({img.getHead().matrix_size[0]}x"
                         f"{img.getHead().matrix_size[1]}) — skipped")

            elif msg_id == MRD_MESSAGE_ISMRMRD_WAVEFORM:
                wf = ismrmrd.Waveform.deserialize_from(
                    lambda n: read_exact(conn, n))
                log.info(f"WAVEFORM ({wf.getHead().number_of_samples} samples) — skipped")

            else:
                log.warning(f"Unknown message ID: {msg_id}")
                break

    except ConnectionError:
        log.info(f"Connection closed from {addr}")
    except Exception as e:
        log.error(f"Error handling connection: {e}")
    finally:
        conn.close()
        log.info(f"Session ended for {addr}")


def main():
    parser = argparse.ArgumentParser(description='Mock MRD TCP reconstruction server')
    parser.add_argument('--port', type=int, default=9002)
    args = parser.parse_args()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('0.0.0.0', args.port))
    server.listen(5)
    log.info(f"MRD TCP mock_recon listening on port {args.port}")

    try:
        while True:
            conn, addr = server.accept()
            t = threading.Thread(target=handle_connection, args=(conn, addr))
            t.daemon = True
            t.start()
    except KeyboardInterrupt:
        log.info("Shutting down")
    finally:
        server.close()


if __name__ == '__main__':
    main()
