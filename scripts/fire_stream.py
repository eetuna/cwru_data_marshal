#!/usr/bin/env python3
"""
fire_stream.py — single-connection, paced MRD streamer using the real fire
(python-ismrmrd-server) protocol.

Opens ONE MRD TCP connection to the marshal, sends CONFIG + METADATA once, then
streams paced frames of a *moving* Shepp-Logan phantom over that one connection,
reading return messages back on the same socket (exactly like a real scanner):

  --mode kspace : send ACQUISITION lines per frame -> marshal -> recon -> image back
  --mode image  : send a reconstructed IMAGE per frame -> published directly (bypass recon)

Multislice: --slices N streams an N-slice volume per frame. In kspace mode each
slice is sent with idx.slice + LAST_IN_SLICE flags (recon reconstructs per
slice); in image mode one IMAGE message carries the whole 3D stack.

Works in live or dump mode (dump is a marshal setting — same stream; the marshal
archives instead of publishing). Run inside the fire-python image, e.g.:

  docker run --rm --network cwru-demo-net -v "$PWD/scripts:/scripts" fire-python:latest \
    python3 /scripts/fire_stream.py --address mri-marshal --port 9100 \
    --mode kspace --fps 10 --frames 0 --matrix 128 --slices 5
"""
import argparse, io, socket, sys, time, threading
import numpy as np

# fire modules live here in the fire-python image
sys.path.insert(0, "/opt/code/python-ismrmrd-server")
import ismrmrd, ismrmrd.xsd                      # noqa: E402
from ismrmrdtools import simulation, transform   # noqa: E402
from connection import Connection                # noqa: E402
import constants                                 # noqa: E402

# Precomputed 2-byte message tags (same wire bytes as connection.py sends).
ACQ_TAG = constants.MrdMessageIdentifier.pack(constants.MRD_MESSAGE_ISMRMRD_ACQUISITION)
IMG_TAG = constants.MrdMessageIdentifier.pack(constants.MRD_MESSAGE_ISMRMRD_IMAGE)


def build_header(matrix, coils, slices, fov=300.0):
    h = ismrmrd.xsd.ismrmrdHeader()
    exp = ismrmrd.xsd.experimentalConditionsType(); exp.H1resonanceFrequency_Hz = 128000000
    h.experimentalConditions = exp
    sysinfo = ismrmrd.xsd.acquisitionSystemInformationType(); sysinfo.receiverChannels = coils
    h.acquisitionSystemInformation = sysinfo

    enc = ismrmrd.xsd.encodingType()
    enc.trajectory = ismrmrd.xsd.trajectoryType.CARTESIAN
    for space in ("encodedSpace", "reconSpace"):
        fovmm = ismrmrd.xsd.fieldOfViewMm(); fovmm.x = fov; fovmm.y = fov; fovmm.z = 6.0
        ms = ismrmrd.xsd.matrixSizeType(); ms.x = matrix; ms.y = matrix; ms.z = 1
        sp = ismrmrd.xsd.encodingSpaceType(); sp.matrixSize = ms; sp.fieldOfView_mm = fovmm
        setattr(enc, space, sp)
    limits = ismrmrd.xsd.encodingLimitsType()
    l1 = ismrmrd.xsd.limitType(); l1.minimum = 0; l1.maximum = matrix - 1; l1.center = matrix // 2
    limits.kspace_encoding_step_1 = l1
    # Slice limits: the marshal parses <slice><maximum> to size multislice volumes.
    ls = ismrmrd.xsd.limitType(); ls.minimum = 0; ls.maximum = slices - 1; ls.center = slices // 2
    limits.slice = ls
    enc.encodingLimits = limits
    h.encoding.append(enc)
    return h.toXML('utf-8')


# Orbit angular speed (rad/sec). Motion is driven by WALL-CLOCK phase, not frame
# index, so the dot orbits at this fixed visible speed regardless of --fps
# (higher fps just = more points on the same circle = smoother, not faster).
ORBIT_RAD_PER_SEC = 1.4          # ~4.5 s per revolution


def moving_phantom(matrix, phase, slice_idx=0, base=None):
    """Shepp-Logan + a bright disk orbiting the center at `phase` radians,
    offset per slice so slices differ."""
    phan = (base if base is not None else simulation.phantom(matrix)).astype(np.float32).copy()
    t = phase + slice_idx * 0.8
    cx = matrix // 2 + int(matrix * 0.32 * np.cos(t))
    cy = matrix // 2 + int(matrix * 0.32 * np.sin(t))
    yy, xx = np.ogrid[:matrix, :matrix]
    disk = ((xx - cx) ** 2 + (yy - cy) ** 2) < (matrix * 0.14) ** 2
    phan[disk] += 3.0
    return phan


def reader_loop(sock):
    inc = Connection(sock, False)
    try:
        for _ in inc:      # drain images / text / close from the marshal
            pass
    except Exception:
        pass


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--address", default="mri-marshal")
    p.add_argument("--port", type=int, default=9100)
    p.add_argument("--mode", choices=["kspace", "image"], default="kspace")
    p.add_argument("--config", default="invertcontrast")
    p.add_argument("--fps", type=float, default=5.0)
    p.add_argument("--frames", type=int, default=0, help="0 = until Ctrl-C")
    p.add_argument("--matrix", type=int, default=128)
    p.add_argument("--slices", type=int, default=1)
    p.add_argument("--coils", type=int, default=8)
    args = p.parse_args()

    matrix, coils, slices = args.matrix, args.coils, args.slices
    base = simulation.phantom(matrix)
    csm = simulation.generate_birdcage_sensitivities(matrix, coils)
    xml = build_header(matrix, coils, slices)

    sock = socket.create_connection((args.address, args.port))
    reader = threading.Thread(target=reader_loop, args=(sock,), daemon=True)
    reader.start()

    conn = Connection(sock, False)
    conn.send_config_file(args.config)
    conn.send_metadata(xml)

    # reusable acquisition template
    acq = ismrmrd.Acquisition()
    acq.resize(matrix, coils)
    acq.available_channels = coils
    acq.center_sample = matrix // 2
    acq.read_dir[0] = 1.0; acq.phase_dir[1] = 1.0; acq.slice_dir[2] = 1.0

    period = 1.0 / args.fps if args.fps > 0 else 0.0
    counter = 0
    frame = 0
    last_log = 0.0
    t_start = time.time()
    print(f"streaming mode={args.mode} fps={args.fps} matrix={matrix} slices={slices} "
          f"-> {args.address}:{args.port}", flush=True)
    try:
        while args.frames == 0 or frame < args.frames:
            t0 = time.time()
            # wall-clock phase => constant on-screen orbit speed at any fps
            phase = (t0 - t_start) * ORBIT_RAD_PER_SEC

            # Serialize the whole frame into one buffer and sendall() ONCE
            # (Option A): identical wire bytes to connection.py's per-item sends,
            # but one syscall instead of ~4 per k-space line — the sender was
            # spending most of its time in socket.send() overhead.
            buf = io.BytesIO()
            if args.mode == "kspace":
                for s in range(slices):
                    img = moving_phantom(matrix, phase, s, base)
                    coil_images = np.tile(img, (coils, 1, 1)) * csm
                    K = transform.transform_image_to_kspace(coil_images, (1, 2))
                    for line in range(matrix):
                        acq.scan_counter = counter
                        acq.idx.repetition = frame
                        acq.idx.slice = s
                        acq.idx.kspace_encode_step_1 = line
                        acq.clearAllFlags()
                        if line == 0:
                            acq.setFlag(ismrmrd.ACQ_FIRST_IN_ENCODE_STEP1)
                            acq.setFlag(ismrmrd.ACQ_FIRST_IN_SLICE)
                            if s == 0:
                                acq.setFlag(ismrmrd.ACQ_FIRST_IN_REPETITION)
                        elif line == matrix - 1:
                            acq.setFlag(ismrmrd.ACQ_LAST_IN_ENCODE_STEP1)
                            acq.setFlag(ismrmrd.ACQ_LAST_IN_SLICE)
                            if s == slices - 1:
                                acq.setFlag(ismrmrd.ACQ_LAST_IN_REPETITION)
                        acq.data[:] = K[:, line, :]
                        buf.write(ACQ_TAG)
                        acq.serialize_into(buf.write)
                        counter += 1
            else:  # image: one IMAGE message carrying the whole slice stack
                vol = np.stack([moving_phantom(matrix, phase, s, base)
                                for s in range(slices)])         # (nz, ny, nx)
                im = ismrmrd.Image.from_array(vol, transpose=False)
                im.image_index = frame
                im.field_of_view = (300.0, 300.0, 6.0 * slices)
                im.read_dir = (1.0, 0.0, 0.0)
                im.phase_dir = (0.0, 1.0, 0.0)
                im.slice_dir = (0.0, 0.0, 1.0)
                if frame == 0:
                    print(f"  image payload shape={im.data.shape} "
                          f"(matrix_size={tuple(im.matrix_size)}, channels={im.channels})", flush=True)
                buf.write(IMG_TAG)
                im.serialize_into(buf.write)
            sock.sendall(buf.getvalue())

            frame += 1
            now = time.time()
            if now - last_log >= 1.0:
                print(f"  streamed {frame} frames ({args.mode}, {slices} slice(s))", flush=True)
                last_log = now
            dt = now - t0
            if period > dt:
                time.sleep(period - dt)
    except KeyboardInterrupt:
        print("\nstopping...", flush=True)
    finally:
        conn.send_close()
        time.sleep(0.5)
        try:
            sock.close()
        except Exception:
            pass
    print(f"sent {frame} frames", flush=True)


if __name__ == "__main__":
    main()
