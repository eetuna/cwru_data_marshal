#!/usr/bin/env python3
"""
Mock Reconstruction Server

HTTP server on port 9002 that accepts:
  POST /header  — stores XML header
  POST /config  — stores config name
  POST /frame   — deserializes ISMRMRD acquisition, does simplefft, posts image back
  POST /close   — clears state

Uses ismrmrd Python package for deserialization/serialization.
Posts reconstructed images back to MARSHAL_URL/image.

When /frame body is a WAVEFORM (40B header) or UNKNOWN, logs and skips.
"""

import argparse
import logging
import math
import os
import struct
import sys
import urllib.request
from http.server import HTTPServer, BaseHTTPRequestHandler

import numpy as np

try:
    import ismrmrd
except ImportError:
    print("ERROR: ismrmrd package required. pip install ismrmrd", file=sys.stderr)
    sys.exit(1)

logging.basicConfig(level=logging.INFO, format='%(asctime)s [mock_recon] %(message)s')
log = logging.getLogger('mock_recon')

ACQUISITION_HEADER_BYTES = 340
IMAGE_HEADER_BYTES = 198
WAVEFORM_HEADER_BYTES = 40

# Global state
state = {
    'xml_header': None,
    'config': None,
    'marshal_url': None,
    'acq_count': 0,
    'kspace_buffer': {},  # slice -> list of (line_idx, complex64 array)
    'enc_nx': 128,
    'enc_ny': 128,
    'image_series': 0,
}


def detect_type(body: bytes) -> str:
    """Detect ISMRMRD message type from raw body."""
    if len(body) < WAVEFORM_HEADER_BYTES:
        return 'UNKNOWN'

    # Try acquisition
    if len(body) >= ACQUISITION_HEADER_BYTES:
        version = struct.unpack_from('<H', body, 0)[0]
        nsamples = struct.unpack_from('<H', body, 22)[0]  # number_of_samples offset
        nchannels = struct.unpack_from('<H', body, 24)[0]  # active_channels offset
        if 0 < version <= 10 and 0 < nsamples <= 65535 and 0 < nchannels <= 256:
            return 'ACQUISITION'

    # Try image
    if len(body) >= IMAGE_HEADER_BYTES:
        version = struct.unpack_from('<H', body, 0)[0]
        data_type = struct.unpack_from('<H', body, 2)[0]
        if 0 < version <= 10 and 0 < data_type <= 10:
            return 'IMAGE'

    # Try waveform
    if len(body) >= WAVEFORM_HEADER_BYTES:
        version = struct.unpack_from('<H', body, 0)[0]
        nsamples = struct.unpack_from('<H', body, 28)[0]
        channels = struct.unpack_from('<H', body, 30)[0]
        if 0 < version <= 10 and 0 < nsamples <= 65535 and 0 < channels <= 256:
            return 'WAVEFORM'

    return 'UNKNOWN'


def reconstruct_slice(kspace_lines, nx, ny):
    """Simple FFT reconstruction from collected k-space lines."""
    kspace = np.zeros((ny, nx), dtype=np.complex64)
    for line_idx, data in kspace_lines:
        if 0 <= line_idx < ny:
            # data may have multiple channels; take first channel
            nsamples = min(len(data), nx)
            kspace[line_idx, :nsamples] = data[:nsamples]

    image = np.fft.fftshift(np.fft.ifft2(np.fft.ifftshift(kspace)))
    return np.abs(image).astype(np.float32)


def send_image_to_marshal(image_data: np.ndarray, image_series: int):
    """Send reconstructed image back to marshal as POST /image."""
    if state['marshal_url'] is None:
        return

    ny, nx = image_data.shape
    hdr = bytearray(IMAGE_HEADER_BYTES)
    struct.pack_into('<H', hdr, 0, 1)          # version
    struct.pack_into('<H', hdr, 2, 5)          # data_type = FLOAT
    struct.pack_into('<H', hdr, 4, nx)         # matrix_size[0]
    struct.pack_into('<H', hdr, 6, ny)         # matrix_size[1]
    struct.pack_into('<H', hdr, 8, 1)          # matrix_size[2]
    struct.pack_into('<H', hdr, 10, 1)         # channels
    struct.pack_into('<H', hdr, 128, image_series)  # image_series_index

    attr = b''
    attr_len = struct.pack('<Q', len(attr))
    pixel_bytes = image_data.tobytes()

    body = bytes(hdr) + attr_len + attr + pixel_bytes

    req = urllib.request.Request(
        f"{state['marshal_url']}/image",
        data=body,
        headers={'Content-Type': 'application/octet-stream'},
        method='POST'
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            log.info(f"Sent image ({nx}x{ny}) series={image_series} -> HTTP {resp.status}")
    except Exception as e:
        log.warning(f"Failed to send image to marshal: {e}")


class ReconHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length) if content_length > 0 else b''

        if self.path == '/header':
            state['xml_header'] = body.decode('utf-8', errors='replace')
            state['acq_count'] = 0
            state['kspace_buffer'] = {}

            # Try to parse encoding size from XML
            xml = state['xml_header']
            try:
                import re
                mx = re.search(r'<x>(\d+)</x>', xml)
                my = re.search(r'<y>(\d+)</y>', xml)
                if mx: state['enc_nx'] = int(mx.group(1))
                if my: state['enc_ny'] = int(my.group(1))
            except:
                pass

            log.info(f"POST /header: {len(body)}B, enc={state['enc_nx']}x{state['enc_ny']}")
            self._ok()

        elif self.path == '/config':
            state['config'] = body.decode('utf-8', errors='replace')
            log.info(f"POST /config: {state['config']}")
            self._ok()

        elif self.path == '/frame':
            msg_type = detect_type(body)

            if msg_type == 'ACQUISITION':
                self._handle_acquisition(body)
            elif msg_type == 'WAVEFORM':
                log.info(f"POST /frame: WAVEFORM ({len(body)}B) — skipped")
            elif msg_type == 'IMAGE':
                log.info(f"POST /frame: IMAGE ({len(body)}B) — skipped")
            else:
                log.info(f"POST /frame: UNKNOWN ({len(body)}B) — skipped")

            self._ok()

        elif self.path == '/close':
            # Reconstruct any remaining buffered slices
            self._flush_reconstruction()
            state['xml_header'] = None
            state['config'] = None
            state['acq_count'] = 0
            state['kspace_buffer'] = {}
            log.info("POST /close: scan finalized")
            self._ok()

        else:
            self.send_response(404)
            self.end_headers()

    def _handle_acquisition(self, body: bytes):
        state['acq_count'] += 1
        nx = state['enc_nx']
        ny = state['enc_ny']

        # Parse header fields we need
        nsamples = struct.unpack_from('<H', body, 22)[0]
        nchannels = struct.unpack_from('<H', body, 24)[0]
        traj_dim = struct.unpack_from('<H', body, 26)[0]
        slice_idx = struct.unpack_from('<H', body, 68)[0]  # idx.slice offset
        line_idx = struct.unpack_from('<H', body, 66)[0]   # idx.kspace_encode_step_1 offset
        flags = struct.unpack_from('<Q', body, 28)[0]

        # Data offset
        traj_bytes = traj_dim * nsamples * 4
        data_offset = ACQUISITION_HEADER_BYTES + traj_bytes
        sample_bytes = nsamples * nchannels * 8  # complex64

        if data_offset + sample_bytes > len(body):
            return

        # Read first channel's data
        samples = np.frombuffer(body[data_offset:data_offset + nsamples * 8],
                                dtype=np.complex64)

        if slice_idx not in state['kspace_buffer']:
            state['kspace_buffer'][slice_idx] = []
        state['kspace_buffer'][slice_idx].append((line_idx, samples.copy()))

        # Check LAST_IN_SLICE flag
        LAST_IN_SLICE = (1 << 0)  # bit 1 in ISMRMRD
        # Actually ISMRMRD_ACQ_LAST_IN_SLICE = 4, so bit 3 (1-indexed)
        if flags & (1 << 3):  # ISMRMRD_ACQ_LAST_IN_SLICE - 1 = 3
            if slice_idx in state['kspace_buffer']:
                lines = state['kspace_buffer'].pop(slice_idx)
                if len(lines) > 0:
                    image = reconstruct_slice(lines, nx, ny)
                    send_image_to_marshal(image, state['image_series'])
                    state['image_series'] += 1

        if state['acq_count'] % 100 == 0:
            log.info(f"Received {state['acq_count']} acquisitions")

    def _flush_reconstruction(self):
        """Reconstruct any remaining buffered slices."""
        nx = state['enc_nx']
        ny = state['enc_ny']
        for slice_idx, lines in state['kspace_buffer'].items():
            if len(lines) > 0:
                image = reconstruct_slice(lines, nx, ny)
                send_image_to_marshal(image, state['image_series'])
                state['image_series'] += 1

    def _ok(self):
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(b'{"status":"ok"}')

    def log_message(self, format, *args):
        pass  # suppress default logging


def main():
    parser = argparse.ArgumentParser(description='Mock ISMRMRD reconstruction server')
    parser.add_argument('--port', type=int, default=9002)
    parser.add_argument('--marshal-url', default=os.environ.get('MARSHAL_URL', 'http://localhost:8080'))
    args = parser.parse_args()

    state['marshal_url'] = args.marshal_url
    log.info(f"Starting mock_recon on port {args.port}, marshal_url={args.marshal_url}")

    server = HTTPServer(('0.0.0.0', args.port), ReconHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info("Shutting down")
        server.server_close()


if __name__ == '__main__':
    main()
