#!/usr/bin/env python3
"""Continuous ISMRMRD image generator that posts wobbling slices to the marshal."""

import argparse
import http.client
import json
import sys
import time
import urllib.parse
from typing import Iterator

import ismrmrd
import numpy as np


def build_frame(frame_index: int, nx: int, ny: int, nz: int) -> ismrmrd.Image:
    phase = frame_index * 0.25
    grid_x = np.linspace(0, 1, nx, dtype=np.float32)
    grid_y = np.linspace(0, 1, ny, dtype=np.float32)
    base = grid_x[None, :] + grid_y[:, None]
    stack = []
    for z in range(nz):
        wobble = 0.25 * np.sin(phase + z * 0.35 + grid_x[None, :] * 2.0 + grid_y[:, None] * 1.5)
        stack.append(base + wobble)
    data = np.stack(stack, axis=0).astype(np.float32)
    img = ismrmrd.Image.from_array(data, transpose=False)
    head = img.getHead()
    head.channels = 1
    head.data_type = ismrmrd.DATATYPE_FLOAT
    head.matrix_size[0] = nx
    head.matrix_size[1] = ny
    head.matrix_size[2] = nz
    head.image_index = (frame_index % 65535) + 1
    head.image_series_index = 1
    head.slice = frame_index % max(1, nz)
    img.setHead(head)
    return img


def iter_frames(limit: int) -> Iterator[int]:
    count = 0
    while limit == 0 or count < limit:
        yield count
        count += 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--http", default="http://localhost:8080", help="marshal base URL")
    parser.add_argument("--stream", default="demo_stream", help="logical stream name")
    parser.add_argument("--frames", type=int, default=0, help="number of frames (0=infinite)")
    parser.add_argument("--interval", type=float, default=0.5, help="seconds between frames")
    parser.add_argument("--dt-ms", type=float, default=None, help="milliseconds between frames (overrides --interval)")
    parser.add_argument("--size", type=int, default=32, help="width/height when --nx/--ny omitted")
    parser.add_argument("--nx", type=int, default=None, help="number of samples along X")
    parser.add_argument("--ny", type=int, default=None, help="number of samples along Y")
    parser.add_argument("--slices", type=int, default=4, help="number of slices per frame")
    parser.add_argument("--nslices", type=int, default=None, help="alias for --slices")
    args = parser.parse_args()

    url = urllib.parse.urlparse(args.http)
    if url.scheme != "http":
        print("Only http:// URLs are supported", file=sys.stderr)
        return 1
    host = url.hostname or "localhost"
    port = url.port or 80

    conn = http.client.HTTPConnection(host, port)
    path = "/v1/ismrmrd/frame"

    nx = args.nx if args.nx is not None else args.size
    ny = args.ny if args.ny is not None else args.size
    nz = args.nslices if args.nslices is not None else args.slices
    interval = args.interval if args.dt_ms is None else args.dt_ms / 1000.0

    for frame_index in iter_frames(args.frames):
        img = build_frame(frame_index, nx, ny, nz)
        body = bytes(img.getHead()) + img.data.tobytes()
        headers = {
            "Content-Type": "application/octet-stream",
            "X-MRD-Stream": args.stream,
        }
        conn.request("POST", path, body=body, headers=headers)
        resp = conn.getresponse()
        payload = resp.read()
        ok = resp.status == 200
        msg = payload.decode("utf-8", errors="ignore") if payload else ""
        if ok:
            try:
                parsed = json.loads(msg)
                print(f"frame {frame_index} -> {json.dumps(parsed)}")
            except json.JSONDecodeError:
                print(f"frame {frame_index} -> status {resp.status} body={msg}")
        else:
            print(f"frame {frame_index} failed: {resp.status} body={msg}", file=sys.stderr)
            break
        if interval > 0:
            time.sleep(interval)

    conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
