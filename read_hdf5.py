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
  One-shot:  python3 read_hdf5.py <file_path> <frame_index> <mode>
  Server:    python3 read_hdf5.py --serve
             (persistent worker: one JSON request per stdin line
              {"path": ..., "frame": N, "mode": "2d"|"3d"} -> one JSON
              response per stdout line. Avoids a Python interpreter
              start per poll, which caps the viewer at a few fps.)
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


def read_frame(fpath, frame_index, mode):
    """Read one frame from the file; returns a JSON-serializable dict."""
    if not os.path.exists(fpath):
        return {"error": f"File not found: {fpath}"}

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
            return {"error": f"unexpected image dataset shape {shape}"}

        # frame_index <= 0 means "newest". The marshal appends images to the
        # snapshot during a scan, so entry [0] is the OLDEST frame; the live
        # viewer must read the newest or it freezes on the scan's first frame.
        fi = (n_frames - 1) if frame_index <= 0 else min(frame_index, n_frames - 1)

        if mode == "2d":
            mid_z = nz // 2
            values = data[fi, 0, mid_z, :, :].flatten().tolist()
            return {"width": int(nx), "height": int(ny), "values": values}
        elif mode == "3d":
            values = data[fi, 0, :, :, :].flatten().tolist()
            return {"width": int(nx), "height": int(ny), "depth": int(nz), "values": values}
        else:
            return {"error": f"Unknown mode: {mode}"}
    finally:
        f.close()


def serve():
    """Persistent worker: JSON-lines request/response over stdin/stdout."""
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
            result = read_frame(req["path"], int(req.get("frame", 0)), req.get("mode", "2d"))
        except Exception as e:
            result = {"error": str(e)}
        sys.stdout.write(json.dumps(result) + "\n")
        sys.stdout.flush()


def main():
    if "--serve" in sys.argv:
        serve()
        return

    if len(sys.argv) != 4:
        print(json.dumps({"error": "Usage: read_hdf5.py <path> <frame_index> <2d|3d>  (or --serve)"}))
        sys.exit(1)

    result = read_frame(sys.argv[1], int(sys.argv[2]), sys.argv[3])
    print(json.dumps(result))


if __name__ == "__main__":
    main()
