#!/usr/bin/env python3
"""
Scanner simulator: generate Shepp-Logan k-space and POST it to marshal.

This plays the role of a real MRI scanner for end-to-end testing of the
reconstruction path. It:

  1. Generates a Shepp-Logan phantom dataset using the tooling vendored in
     third_party/python-ismrmrd-server (generate_cartesian_shepp_logan_dataset.py).
  2. Reads the acquisitions from the resulting .h5 as serialized bytes.
  3. Concatenates them (AcquisitionHeader + traj + data, back-to-back - the
     same on-the-wire layout marshal's /v1/mrd/frame handler expects).
  4. POSTs the blob to marshal with X-MRD-Stream set.

Marshal detects the body as ACQUISITION (raw k-space), forwards it async to
recon-sim (the HTTP shim + python-ismrmrd-server), which runs inverse FFT and
POSTs the reconstructed image back to marshal's /v1/mrd/callback. The stored
image can then be fetched with GET /v1/mrd/latest.

Usage (from the worktree root):
    python3 clients/mocks/scanner_kspace_client.py \
        --marshal http://localhost:8080 \
        --stream shepp_logan \
        --matrix 128 \
        --coils 1

Dependencies: ismrmrd, numpy, ismrmrd-python-tools, requests. These are all
available in the recon-sim container; if you run the client on the host, pip
install ismrmrd ismrmrd-python-tools requests.
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile

import ismrmrd
import requests

# The generator lives alongside the vendored python-ismrmrd-server code.
_VENDORED = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..",
    "..",
    "third_party",
    "python-ismrmrd-server",
)
sys.path.insert(0, _VENDORED)

import generate_cartesian_shepp_logan_dataset as gen  # noqa: E402


def build_kspace_blob(h5_path: str) -> bytes:
    """Serialize every acquisition in the dataset back to on-the-wire bytes.

    ismrmrd.Acquisition.serialize_into() takes a callable; we capture output
    into a bytearray and return it concatenated. Layout per acquisition is
    AcquisitionHeader (340) + trajectory floats + sample complex floats.
    """
    dset = ismrmrd.Dataset(h5_path, "dataset", create_if_needed=False)
    try:
        n = dset.number_of_acquisitions()
        buf = bytearray()

        def sink(chunk):
            buf.extend(chunk)

        for i in range(n):
            acq = dset.read_acquisition(i)
            acq.serialize_into(sink)
        return bytes(buf)
    finally:
        dset.close()


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--marshal", default="http://localhost:8080",
                   help="Marshal base URL")
    p.add_argument("--stream", default="shepp_logan",
                   help="X-MRD-Stream header value")
    p.add_argument("--session", default="scanner_sim",
                   help="X-MRD-Session header value")
    p.add_argument("--matrix", type=int, default=128,
                   help="Matrix size for the generated phantom")
    p.add_argument("--coils", type=int, default=1,
                   help="Number of coils")
    p.add_argument("--oversampling", type=int, default=2,
                   help="Readout oversampling factor")
    p.add_argument("--noise", type=float, default=0.05,
                   help="Noise level")
    p.add_argument("--keep-h5", action="store_true",
                   help="Keep the generated .h5 for inspection")
    args = p.parse_args()

    with tempfile.TemporaryDirectory() as td:
        h5_path = os.path.join(td, "shepp_logan.h5")
        print(f"[scanner-sim] generating phantom k-space -> {h5_path}")
        gen.create(
            filename=h5_path,
            matrix_size=args.matrix,
            coils=args.coils,
            oversampling=args.oversampling,
            repetitions=1,
            acceleration=1,
            noise_level=args.noise,
        )

        print(f"[scanner-sim] reading acquisitions and serializing")
        blob = build_kspace_blob(h5_path)
        print(f"[scanner-sim] k-space blob: {len(blob)} bytes")

        if args.keep_h5:
            keep = os.path.abspath("scanner_sim_kspace.h5")
            os.replace(h5_path, keep)
            print(f"[scanner-sim] kept .h5 at {keep}")

        url = args.marshal.rstrip("/") + "/v1/mrd/frame"
        print(f"[scanner-sim] POST {url} (stream={args.stream})")
        r = requests.post(
            url,
            data=blob,
            headers={
                "Content-Type": "application/octet-stream",
                "X-MRD-Stream": args.stream,
                "X-MRD-Session": args.session,
            },
            timeout=60,
        )
        print(f"[scanner-sim] marshal response: {r.status_code}")
        try:
            print(f"[scanner-sim] body: {r.json()}")
        except Exception:
            print(f"[scanner-sim] body: {r.text[:500]}")

        if r.status_code not in (200, 201, 202):
            return 1

    print("[scanner-sim] done. Poll GET /v1/mrd/latest on marshal to see the "
          "reconstructed image once recon-sim callbacks fire.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
