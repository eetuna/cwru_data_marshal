#!/usr/bin/env python3
"""
HDF5 frame reader for the WebGL client.

Reads the reconstructed image the marshal publishes to latest_image.h5.
Marshal v2 writes canonical ISMRMRD via Dataset::appendImage, producing
    <group>/image_<n>/data   with shape [n_images, channels, z, y, x]
(e.g. dataset/image_0/data (1,1,1,256,256) int16).

Older marshals wrote a single /images/data 5D dataset; that layout is
still supported as a fallback.

Usage:
  python3 read_hdf5.py <file_path> <frame_index> <mode>
  mode: "2d" (middle slice) or "3d" (full volume)
"""
import sys, os, json
os.environ["HDF5_USE_FILE_LOCKING"] = "FALSE"
import h5py


def find_image_dataset(f):
    """Return the image-data dataset, canonical ISMRMRD or legacy layout."""
    # Legacy layout: single /images/data
    if "images" in f and isinstance(f["images"], h5py.Group) and "data" in f["images"]:
        return f["images"]["data"]
    # Canonical ISMRMRD: <group>/image_<n>/data
    found = []

    def visit(name, obj):
        parts = name.split("/")
        if (isinstance(obj, h5py.Dataset) and parts[-1] == "data"
                and len(parts) >= 2 and parts[-2].startswith("image_")):
            found.append(name)

    f.visititems(visit)
    if not found:
        raise KeyError("no image data dataset found "
                       "(looked for /images/data and <group>/image_*/data)")
    # Pick the latest appended image (image_0, image_1, ...)
    found.sort(key=lambda n: (len(n), n))
    return f[found[-1]]


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

    # Marshal publishes atomically-renamed complete files. Try SWMR (works for
    # libismrmrd's libver=latest files); fall back to a plain read otherwise.
    try:
        f = h5py.File(fpath, "r", swmr=True)
    except Exception:
        f = h5py.File(fpath, "r")
    try:
        dset = find_image_dataset(f)
        try:
            dset.refresh()
        except Exception:
            pass

        shape = dset.shape
        if len(shape) == 5:            # [n_images, channels, z, y, x]
            data = dset
            n_frames, n_ch, nz, ny, nx = shape
        elif len(shape) == 4:          # [channels, z, y, x] single image
            data = dset[()].reshape((1,) + shape)
            n_frames, n_ch, nz, ny, nx = data.shape
        else:
            print(json.dumps({"error": f"unexpected image dataset shape {shape}"}))
            return

        fi = min(frame_index, n_frames - 1)

        if mode == "2d":
            mid_z = nz // 2
            values = data[fi, 0, mid_z, :, :].flatten().tolist()
            result = {"width": nx, "height": ny, "values": values}
        elif mode == "3d":
            values = data[fi, 0, :, :, :].flatten().tolist()
            result = {"width": nx, "height": ny, "depth": nz, "values": values}
        else:
            result = {"error": f"Unknown mode: {mode}"}

        print(json.dumps(result))
    finally:
        f.close()


if __name__ == "__main__":
    main()
