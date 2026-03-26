#!/usr/bin/env python3
"""
HDF5 frame reader for the WebGL client.
Mirrors viz_client_main.cpp logic:
  1. Open .mrd file in SWMR read mode
  2. Read /images/data dataset [frames, channels, z, y, x] float32
  3. Output JSON with pixel values for the requested frame

Usage:
  python3 read_hdf5.py <file_path> <frame_index> <mode>
  mode: "2d" (middle slice) or "3d" (full volume)
"""
import sys, os, json
os.environ["HDF5_USE_FILE_LOCKING"] = "FALSE"
import h5py

def main():
    if len(sys.argv) != 4:
        print(json.dumps({"error": "Usage: read_hdf5.py <path> <frame_index> <2d|3d>"}))
        sys.exit(1)

    fpath = sys.argv[1]
    frame_index = int(sys.argv[2])
    mode = sys.argv[3]

    if not os.path.exists(fpath):
        print(json.dumps({"error": f"File not found: {fpath}"}))
        sys.exit(1)

    f = h5py.File(fpath, "r", swmr=True)
    try:
        dset = f["/images/data"]
        dset.refresh()
        # shape: [frames, channels, z, y, x]
        shape = dset.shape
        n_frames, n_ch, nz, ny, nx = shape
        fi = min(frame_index, n_frames - 1)

        if mode == "2d":
            mid_z = nz // 2
            values = dset[fi, 0, mid_z, :, :].flatten().tolist()
            result = {"width": nx, "height": ny, "values": values}
        elif mode == "3d":
            values = dset[fi, 0, :, :, :].flatten().tolist()
            result = {"width": nx, "height": ny, "depth": nz, "values": values}
        else:
            result = {"error": f"Unknown mode: {mode}"}

        print(json.dumps(result))
    finally:
        f.close()

if __name__ == "__main__":
    main()
