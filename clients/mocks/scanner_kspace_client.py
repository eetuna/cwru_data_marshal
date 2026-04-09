#!/usr/bin/env python3
"""
Scanner simulator: streams animated multi-slice Shepp-Logan k-space to marshal.

Each iteration:
  1. Generates a volumetric Shepp-Logan phantom (N slices) with a rotation and
     intensity modulation that varies over time, so the reconstructed image
     visibly changes frame to frame.
  2. Inverse-FFTs each slice to k-space.
  3. Emits noise calibration scans, then per-slice phase-encode lines with
     idx.slice set and ACQ_FIRST_IN_SLICE / ACQ_LAST_IN_SLICE flags so the
     recon server groups them correctly.
  4. Serializes the stack to on-the-wire ISMRMRD bytes and POSTs to
     marshal's /v1/mrd/frame.
  5. Sleeps --dt-ms, repeats.

Marshal forwards each POST to recon-sim (python-ismrmrd-server simplefft),
which returns one image per slice. The shim then packs all N slices back into
a single 3D frame and POSTs it to marshal's callback, so marshal stores one
multi-slice volume per iteration and GET /v1/mrd/latest advances its
frame_index on every tick.

Usage (from the worktree root):
    python3 clients/mocks/scanner_kspace_client.py \
        --marshal http://localhost:8080 \
        --stream phantom \
        --matrix 128 --slices 5 --frames 0 --dt-ms 500
"""

from __future__ import annotations

import argparse
import math
import sys
import time

import ismrmrd
import numpy as np
import requests
from ismrmrdtools import simulation, transform

NOISE_SCANS = 32
OVERSAMPLING = 2


def make_rotated_phantom(matrix_size: int, angle_rad: float, intensity: float) -> np.ndarray:
    """Shepp-Logan phantom with a soft rotation and intensity modulation.

    Rotation is done with a coordinate warp (bilinear sampling) so we stay
    within numpy / scipy-free territory. intensity scales the whole image.
    """
    base = simulation.phantom(matrix_size).astype(np.float32)
    ny, nx = base.shape
    yy, xx = np.mgrid[0:ny, 0:nx].astype(np.float32)
    cx = (nx - 1) / 2.0
    cy = (ny - 1) / 2.0
    cos_a = math.cos(angle_rad)
    sin_a = math.sin(angle_rad)
    xr = cos_a * (xx - cx) + sin_a * (yy - cy) + cx
    yr = -sin_a * (xx - cx) + cos_a * (yy - cy) + cy
    x0 = np.floor(xr).astype(np.int32)
    y0 = np.floor(yr).astype(np.int32)
    x1 = x0 + 1
    y1 = y0 + 1
    wx = xr - x0
    wy = yr - y0
    in_bounds = (x0 >= 0) & (y0 >= 0) & (x1 < nx) & (y1 < ny)
    x0c = np.clip(x0, 0, nx - 1)
    x1c = np.clip(x1, 0, nx - 1)
    y0c = np.clip(y0, 0, ny - 1)
    y1c = np.clip(y1, 0, ny - 1)
    i00 = base[y0c, x0c]
    i01 = base[y0c, x1c]
    i10 = base[y1c, x0c]
    i11 = base[y1c, x1c]
    rotated = (
        i00 * (1 - wx) * (1 - wy)
        + i01 * wx * (1 - wy)
        + i10 * (1 - wx) * wy
        + i11 * wx * wy
    )
    rotated = np.where(in_bounds, rotated, 0.0)
    return (rotated * intensity).astype(np.float32)


def build_volume(matrix_size: int, num_slices: int, tick: int) -> np.ndarray:
    """Return a (num_slices, matrix_size, matrix_size) float32 volume.

    Each slice is a Shepp-Logan phantom with a slice-dependent z-modulation
    (shrinks away from the central slice) and a time-dependent rotation and
    brightness so successive ticks visibly animate.
    """
    vol = np.zeros((num_slices, matrix_size, matrix_size), dtype=np.float32)
    angle = 0.05 * tick  # radians per tick
    brightness = 0.75 + 0.25 * math.sin(0.1 * tick)
    for s in range(num_slices):
        # slice_weight: peaks at the middle slice and tapers away
        rel = (s - (num_slices - 1) / 2.0) / max(1.0, (num_slices - 1) / 2.0)
        slice_weight = math.cos(0.5 * math.pi * rel) ** 2  # 0..1
        img = make_rotated_phantom(matrix_size, angle + 0.1 * s, brightness * slice_weight)
        vol[s] = img
    return vol


def build_kspace_blob(volume: np.ndarray, coils: int, noise_level: float) -> bytes:
    """Turn a (nz, ny, nx) image volume into serialized ISMRMRD acquisitions.

    For each slice we:
      - Apply coil sensitivities (birdcage).
      - Inverse FFT image -> k-space (oversampled readout).
      - Emit ACQ_IS_NOISE_MEASUREMENT scans for the very first slice only.
      - Emit per-line acquisitions with idx.slice, idx.kspace_encode_step_1,
        and ACQ_FIRST/LAST_IN_SLICE flags set so simplefft groups correctly.
    """
    nz, ny, nx = volume.shape
    nkx = OVERSAMPLING * nx
    nky = ny

    # Pad each slice for oversampled readout.
    pad = (nkx - nx) // 2
    padded = np.pad(volume, ((0, 0), (0, 0), (pad, pad)), mode="constant")

    csm = simulation.generate_birdcage_sensitivities(nx, coils)  # (coils, nx, nx)
    csm_padded = np.pad(csm, ((0, 0), (0, 0), (pad, pad)), mode="constant")

    buf = bytearray()

    def sink(chunk):
        buf.extend(chunk)

    acq = ismrmrd.Acquisition()
    acq.resize(nkx, coils)
    acq.version = 1
    acq.available_channels = coils
    acq.center_sample = round(nkx / 2)
    acq.read_dir[0] = 1.0
    acq.phase_dir[1] = 1.0
    acq.slice_dir[2] = 1.0

    counter = 0

    # Noise calibration scans (once, up front)
    for _ in range(NOISE_SCANS):
        noise = noise_level * (
            np.random.randn(coils, nkx) + 1j * np.random.randn(coils, nkx)
        )
        acq.scan_counter = counter
        acq.clearAllFlags()
        acq.setFlag(ismrmrd.ACQ_IS_NOISE_MEASUREMENT)
        acq.data[:] = noise
        acq.serialize_into(sink)
        counter += 1

    # Imaging lines per slice
    for s in range(nz):
        coil_images = np.tile(padded[s], (coils, 1, 1)) * csm_padded
        K = transform.transform_image_to_kspace(coil_images, (1, 2))
        noise = noise_level * (
            np.random.randn(coils, nky, nkx) + 1j * np.random.randn(coils, nky, nkx)
        )
        K = K + noise
        for line in range(nky):
            acq.scan_counter = counter
            acq.idx.slice = s
            acq.idx.kspace_encode_step_1 = line
            acq.clearAllFlags()
            if line == 0:
                acq.setFlag(ismrmrd.ACQ_FIRST_IN_ENCODE_STEP1)
                acq.setFlag(ismrmrd.ACQ_FIRST_IN_SLICE)
                if s == 0:
                    acq.setFlag(ismrmrd.ACQ_FIRST_IN_REPETITION)
            if line == nky - 1:
                acq.setFlag(ismrmrd.ACQ_LAST_IN_ENCODE_STEP1)
                acq.setFlag(ismrmrd.ACQ_LAST_IN_SLICE)
                if s == nz - 1:
                    acq.setFlag(ismrmrd.ACQ_LAST_IN_REPETITION)
            acq.data[:] = K[:, line, :]
            acq.serialize_into(sink)
            counter += 1

    return bytes(buf)


def post_frame(url: str, blob: bytes, stream: str, session: str) -> int:
    r = requests.post(
        url,
        data=blob,
        headers={
            "Content-Type": "application/octet-stream",
            "X-MRD-Stream": stream,
            "X-MRD-Session": session,
        },
        timeout=60,
    )
    return r.status_code


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--marshal", default="http://localhost:8080")
    p.add_argument("--stream", default="phantom")
    p.add_argument("--session", default="scanner_sim")
    p.add_argument("--matrix", type=int, default=128)
    p.add_argument("--slices", type=int, default=5)
    p.add_argument("--coils", type=int, default=1)
    p.add_argument("--noise", type=float, default=0.05)
    p.add_argument("--dt-ms", type=int, default=500, help="delay between frames in ms")
    p.add_argument(
        "--frames",
        type=int,
        default=0,
        help="Number of frames to send (0 = forever until Ctrl-C)",
    )
    args = p.parse_args()

    url = args.marshal.rstrip("/") + "/v1/mrd/frame"
    print(
        f"[scanner-sim] streaming to {url} stream={args.stream} "
        f"matrix={args.matrix} slices={args.slices} coils={args.coils} "
        f"dt={args.dt_ms}ms frames={'inf' if args.frames == 0 else args.frames}"
    )

    tick = 0
    try:
        while True:
            if args.frames and tick >= args.frames:
                break
            t0 = time.time()
            vol = build_volume(args.matrix, args.slices, tick)
            blob = build_kspace_blob(vol, args.coils, args.noise)
            gen_ms = int((time.time() - t0) * 1000)
            try:
                status = post_frame(url, blob, args.stream, args.session)
            except requests.exceptions.RequestException as e:
                print(f"[scanner-sim] tick={tick} POST error: {e}")
                status = -1
            print(
                f"[scanner-sim] tick={tick} slices={args.slices} "
                f"blob={len(blob)}B gen={gen_ms}ms -> marshal={status}"
            )
            tick += 1
            # Sleep the remainder of the frame interval
            elapsed_ms = int((time.time() - t0) * 1000)
            remaining = args.dt_ms - elapsed_ms
            if remaining > 0:
                time.sleep(remaining / 1000.0)
    except KeyboardInterrupt:
        print("\n[scanner-sim] interrupted, exiting")

    return 0


if __name__ == "__main__":
    sys.exit(main())
