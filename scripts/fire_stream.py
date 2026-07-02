#!/usr/bin/env python3
"""
fire_stream.py — single-connection, paced MRD streamer using the real fire
(python-ismrmrd-server) protocol.

Opens ONE MRD TCP connection to the marshal, sends CONFIG + METADATA once, then
streams paced frames of a *moving* Shepp-Logan phantom over that one connection,
reading return messages back on the same socket (exactly like a real scanner):

  --mode kspace : send ACQUISITION lines per frame -> marshal -> recon -> image back
  --mode image  : send a reconstructed IMAGE per frame -> published directly (bypass recon)

Works in live or dump mode (dump is a marshal setting — same stream; the marshal
archives instead of publishing). Run inside the fire-python image, e.g.:

  docker run --rm --network cwru-demo-net fire-python:latest \
    python3 /scripts/fire_stream.py --address mri-marshal --port 9100 \
    --mode kspace --fps 5 --frames 0 --matrix 128

Mount this file in:  -v "$PWD/scripts:/scripts"
"""
import argparse, socket, sys, time, threading
import numpy as np

# fire modules live here in the fire-python image
sys.path.insert(0, "/opt/code/python-ismrmrd-server")
import ismrmrd, ismrmrd.xsd                      # noqa: E402
from ismrmrdtools import simulation, transform   # noqa: E402
from connection import Connection                # noqa: E402


def build_header(matrix, coils, fov=300.0):
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
    enc.encodingLimits = limits
    h.encoding.append(enc)
    return h.toXML('utf-8')


def moving_phantom(matrix, frame):
    """Base Shepp-Logan + a bright disk orbiting the center -> visibly changes per frame."""
    phan = simulation.phantom(matrix).astype(np.float32)
    t = frame * 0.3
    cx = matrix // 2 + int(matrix * 0.30 * np.cos(t))
    cy = matrix // 2 + int(matrix * 0.30 * np.sin(t))
    yy, xx = np.ogrid[:matrix, :matrix]
    disk = ((xx - cx) ** 2 + (yy - cy) ** 2) < (matrix * 0.07) ** 2
    phan[disk] += 1.0
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
    p.add_argument("--coils", type=int, default=8)
    args = p.parse_args()

    matrix, coils = args.matrix, args.coils
    csm = simulation.generate_birdcage_sensitivities(matrix, coils)
    xml = build_header(matrix, coils)

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
    print(f"streaming mode={args.mode} fps={args.fps} matrix={matrix} -> {args.address}:{args.port}", flush=True)
    try:
        while args.frames == 0 or frame < args.frames:
            t0 = time.time()
            img = moving_phantom(matrix, frame)

            if args.mode == "kspace":
                coil_images = np.tile(img, (coils, 1, 1)) * csm
                K = transform.transform_image_to_kspace(coil_images, (1, 2))
                for line in range(matrix):
                    acq.scan_counter = counter
                    acq.idx.repetition = frame
                    acq.idx.kspace_encode_step_1 = line
                    acq.clearAllFlags()
                    if line == 0:
                        acq.setFlag(ismrmrd.ACQ_FIRST_IN_ENCODE_STEP1)
                        acq.setFlag(ismrmrd.ACQ_FIRST_IN_SLICE)
                        acq.setFlag(ismrmrd.ACQ_FIRST_IN_REPETITION)
                    elif line == matrix - 1:
                        acq.setFlag(ismrmrd.ACQ_LAST_IN_ENCODE_STEP1)
                        acq.setFlag(ismrmrd.ACQ_LAST_IN_SLICE)
                        acq.setFlag(ismrmrd.ACQ_LAST_IN_REPETITION)
                    acq.data[:] = K[:, line, :]
                    conn.send_acquisition(acq)
                    counter += 1
            else:  # image
                im = ismrmrd.Image.from_array(img, transpose=False)
                im.image_index = frame
                im.field_of_view = (300.0, 300.0, 6.0)
                im.read_dir = (1.0, 0.0, 0.0)
                im.phase_dir = (0.0, 1.0, 0.0)
                im.slice_dir = (0.0, 0.0, 1.0)
                conn.send_image(im)

            frame += 1
            now = time.time()
            if now - last_log >= 1.0:
                print(f"  streamed {frame} frames ({args.mode})", flush=True)
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
