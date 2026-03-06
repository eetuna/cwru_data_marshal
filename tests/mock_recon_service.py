#!/usr/bin/env python3
"""
Mock Reconstruction Service for Testing (Async Version)

This Flask server simulates an external reconstruction service with async callbacks.
It receives raw k-space data, processes asynchronously, and POSTs back to marshal.

Usage:
    python3 tests/mock_recon_service.py [port]

Then start marshal with:
    ./build/marshal --data ./data --recon-endpoint http://localhost:9002
"""

from flask import Flask, request, Response, jsonify
import struct
import sys
import time
import threading
import requests

app = Flask(__name__)

# ISMRMRD ImageHeader size (must match C++ struct - 198 bytes, NOT 340)
# See ismrmrd.h: static_assert(sizeof(ISMRMRD_ImageHeader) == 198, ...)
IMAGEHEADER_SIZE = 198

# ISMRMRD AcquisitionHeader size (340 bytes)
# See ismrmrd.h for full structure definition
ACQUISITION_HEADER_SIZE = 340

# Simulated reconstruction delay (seconds)
RECON_DELAY = 0.5  # Adjust for testing: 0.5s is realistic for simple FFT


def create_image_header(matrix_x=64, matrix_y=64, matrix_z=1, channels=1, data_type=5):
    """
    Create a minimal ISMRMRD ImageHeader.

    ImageHeader layout (from ismrmrd.h):
        offset 0:  version (uint16_t)
        offset 2:  data_type (uint16_t)
        offset 4:  flags (uint64_t)
        offset 12: measurement_uid (uint32_t)
        offset 16: matrix_size[3] (3x uint16_t)
        offset 22: field_of_view[3] (3x float)
        offset 34: channels (uint16_t)
        ... other fields ...

    data_type values:
        1 = USHORT
        2 = SHORT
        3 = UINT
        4 = INT
        5 = FLOAT
        6 = DOUBLE
        7 = CXFLOAT
        8 = CXDOUBLE
    """
    header = bytearray(IMAGEHEADER_SIZE)

    # version (uint16_t at offset 0)
    struct.pack_into('<H', header, 0, 1)

    # data_type (uint16_t at offset 2)
    struct.pack_into('<H', header, 2, data_type)

    # flags (uint64_t at offset 4) - skip (leave as 0)

    # measurement_uid (uint32_t at offset 12) - skip

    # matrix_size (3x uint16_t at offset 16, 18, 20)
    struct.pack_into('<H', header, 16, matrix_x)
    struct.pack_into('<H', header, 18, matrix_y)
    struct.pack_into('<H', header, 20, matrix_z)

    # field_of_view (3x float at offset 22, 26, 30) - optional, set reasonable defaults
    struct.pack_into('<f', header, 22, float(matrix_x))  # FOV x in mm
    struct.pack_into('<f', header, 26, float(matrix_y))  # FOV y in mm
    struct.pack_into('<f', header, 30, float(matrix_z))  # FOV z in mm

    # channels (uint16_t at offset 34)
    struct.pack_into('<H', header, 34, channels)

    return bytes(header)


def create_gradient_image(matrix_x=64, matrix_y=64):
    """Create a 2D gradient pattern for visual verification."""
    num_pixels = matrix_x * matrix_y
    pixels = bytearray(num_pixels * 4)  # 4 bytes per float

    for y in range(matrix_y):
        for x in range(matrix_x):
            idx = (y * matrix_x + x) * 4
            value = (x + y) / (matrix_x + matrix_y - 2)
            struct.pack_into('<f', pixels, idx, value)

    return bytes(pixels)


def create_3d_gradient(matrix_x=64, matrix_y=64, matrix_z=1):
    """Create a 3D gradient pattern for visual verification."""
    num_pixels = matrix_x * matrix_y * matrix_z
    pixels = bytearray(num_pixels * 4)  # 4 bytes per float

    for z in range(matrix_z):
        for y in range(matrix_y):
            for x in range(matrix_x):
                idx = ((z * matrix_y + y) * matrix_x + x) * 4
                # Gradient varies by slice (z dimension adds variation)
                value = (x + y + z * 20) / (matrix_x + matrix_y + matrix_z * 20 - 2)
                struct.pack_into('<f', pixels, idx, value)

    return bytes(pixels)


def parse_acquisitions(raw_data):
    """
    Parse multiple ISMRMRD acquisitions from binary data.

    Returns list of acquisition info dicts with keys:
        - slice: slice index
        - samples: number of samples
        - channels: number of channels
        - data_size: size of k-space data in bytes
    """
    acquisitions = []
    offset = 0

    while offset < len(raw_data):
        if offset + ACQUISITION_HEADER_SIZE > len(raw_data):
            break

        # Parse header bytes
        header_bytes = raw_data[offset:offset + ACQUISITION_HEADER_SIZE]

        # Extract key fields from ISMRMRD AcquisitionHeader
        # Offsets based on ismrmrd.h structure definition
        version = struct.unpack_from('<H', header_bytes, 0)[0]
        number_of_samples = struct.unpack_from('<H', header_bytes, 34)[0]
        active_channels = struct.unpack_from('<H', header_bytes, 38)[0]

        # idx structure starts at offset 242
        # Within idx (EncodingCounters), slice is at offset 6
        # So absolute offset for slice = 242 + 6 = 248
        slice_idx = struct.unpack_from('<H', header_bytes, 248)[0]  # idx.slice

        # Calculate sample data size (complex<float> = 8 bytes per sample per channel)
        samples_size = number_of_samples * active_channels * 8

        acquisitions.append({
            'slice': slice_idx,
            'samples': number_of_samples,
            'channels': active_channels,
            'data_size': samples_size
        })

        offset += ACQUISITION_HEADER_SIZE + samples_size

    return acquisitions


def process_and_callback(raw_kspace, callback_url, stream_name, session_id, job_id):
    """
    Background thread: simulate reconstruction and POST result to callback.

    This is where real reconstruction (FFT, Gadgetron, etc.) would happen.
    Supports both single-slice (2D) and multi-slice (3D) volumes.
    """
    try:
        print(f"[mock-recon] [{job_id}] Starting reconstruction...")

        # Parse acquisitions to determine number of slices
        acquisitions = parse_acquisitions(raw_kspace)
        if not acquisitions:
            print(f"[mock-recon] [{job_id}] ERROR: No valid acquisitions found in k-space data")
            return

        # Determine number of slices (unique slice indices)
        slice_indices = set(acq['slice'] for acq in acquisitions)
        num_slices = max(slice_indices) + 1 if slice_indices else 1

        print(f"[mock-recon] [{job_id}] Parsed {len(acquisitions)} acquisitions, {num_slices} slices")
        print(f"[mock-recon] [{job_id}] Slice indices: {sorted(slice_indices)}")

        # Simulate processing time (real recon could be seconds)
        time.sleep(RECON_DELAY)

        # Create 3D reconstructed volume
        matrix_x, matrix_y, matrix_z = 64, 64, num_slices
        channels = 1

        header = create_image_header(matrix_x, matrix_y, matrix_z, channels)
        pixels = create_3d_gradient(matrix_x, matrix_y, matrix_z)
        reconstructed = header + pixels

        print(f"[mock-recon] [{job_id}] Reconstruction complete: {matrix_x}x{matrix_y}x{matrix_z}")
        print(f"[mock-recon] [{job_id}] Sending callback to {callback_url}")

        # POST reconstructed image to marshal callback endpoint
        response = requests.post(
            callback_url,
            data=reconstructed,
            headers={
                'Content-Type': 'application/octet-stream',
                'X-MRD-Stream': stream_name,
                'X-MRD-Session': session_id or '',
                'X-MRD-Job-Id': job_id
            },
            timeout=30
        )

        if response.status_code == 200:
            print(f"[mock-recon] [{job_id}] Callback successful: {response.status_code}")
            try:
                resp_data = response.json()
                print(f"[mock-recon] [{job_id}] Marshal response: frame={resp_data.get('frame_index')}, "
                      f"path={resp_data.get('path')}")
            except Exception:
                print(f"[mock-recon] [{job_id}] Marshal response: {response.text[:200]}")
        else:
            print(f"[mock-recon] [{job_id}] Callback failed: {response.status_code} - {response.text}")

    except requests.exceptions.RequestException as e:
        print(f"[mock-recon] [{job_id}] ERROR: Callback POST failed: {e}")
    except Exception as e:
        print(f"[mock-recon] [{job_id}] ERROR: Reconstruction failed: {e}")


@app.route('/reconstruct', methods=['POST'])
def reconstruct():
    """
    Receive raw k-space data and queue async reconstruction with callback.

    Headers expected:
        X-MRD-Stream: stream identifier
        X-MRD-Callback: URL to POST reconstructed images to
        X-MRD-Job-Id: unique job identifier (for tracking)
        X-MRD-Session: optional session identifier

    Returns:
        HTTP 202 Accepted immediately (processing happens async)
        Falls back to synchronous if no callback URL provided
    """
    raw_kspace = request.data
    stream_name = request.headers.get('X-MRD-Stream', 'unknown')
    callback_url = request.headers.get('X-MRD-Callback')
    job_id = request.headers.get('X-MRD-Job-Id', f'job_{int(time.time()*1000)}')
    session_id = request.headers.get('X-MRD-Session', '')

    print(f"[mock-recon] Received {len(raw_kspace)} bytes of k-space data")
    print(f"[mock-recon] Stream: {stream_name}, Job: {job_id}")
    print(f"[mock-recon] Callback URL: {callback_url}")

    if not callback_url:
        # Fallback to synchronous mode if no callback provided (backwards compatibility)
        print(f"[mock-recon] WARNING: No callback URL, falling back to synchronous mode")

        # Create a 64x64 float image (matching the handoff doc example)
        matrix_x, matrix_y, matrix_z = 64, 64, 1
        channels = 1

        header = create_image_header(matrix_x, matrix_y, matrix_z, channels)
        pixels = create_gradient_image(matrix_x, matrix_y)
        reconstructed = header + pixels

        print(f"[mock-recon] Returning {len(reconstructed)} bytes synchronously")
        return Response(reconstructed, mimetype='application/octet-stream')

    # Spawn background thread for async processing
    thread = threading.Thread(
        target=process_and_callback,
        args=(raw_kspace, callback_url, stream_name, session_id, job_id),
        daemon=True  # Don't block shutdown
    )
    thread.start()

    # Return 202 Accepted immediately
    return jsonify({
        "status": "processing",
        "job_id": job_id,
        "stream": stream_name,
        "message": "Reconstruction queued. Results will be POSTed to callback URL."
    }), 202


@app.route('/health', methods=['GET'])
def health():
    """Health check endpoint."""
    return jsonify({
        'status': 'ok',
        'service': 'mock-reconstruction',
        'mode': 'async',
        'recon_delay_s': RECON_DELAY
    })


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9002
    print(f"[mock-recon] Starting ASYNC mock reconstruction service on port {port}")
    print(f"[mock-recon] Simulated reconstruction delay: {RECON_DELAY}s")
    print(f"[mock-recon] Endpoints:")
    print(f"[mock-recon]   POST /reconstruct - Receive k-space, process async, callback with image")
    print(f"[mock-recon]   GET /health - Health check")

    # Use threaded=True to allow concurrent requests during callbacks
    app.run(host='0.0.0.0', port=port, debug=False, threaded=True)
