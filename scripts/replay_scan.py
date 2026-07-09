#!/usr/bin/env python3
"""replay_scan.py — replay a recorded scan archive at the marshal as if a
scanner were sending it.

Input: a scan_*.h5 recorded by the marshal (dump mode: session-data/dump/
from_scanner/, or a live-mode archive). Sends CONFIG + METADATA + the
recorded acquisitions/images over one MRD TCP connection, then CLOSE —
indistinguishable from a live scan to the marshal/recon/viewer.

Pacing (choose one):
  default        original timing, reconstructed from the records'
                 acquisition_time_stamp fields (--tick-ms per tick, Siemens
                 default 2.5 ms). If the recording has no timestamps (test
                 tools write zeros), falls back to --fps with a warning.
  --fps N        fixed rate: N frames/s (k-space frame = one repetition;
                 image frame = one image).
  --full-speed   no pacing — push as fast as TCP accepts (fastest check
                 that the pipeline handles the data; not realistic timing).

Run inside the fire-python image:
  docker run --rm --network cwru-demo-net \
    -v "$PWD/session-data:/data" -v "$PWD/scripts:/scripts" fire-python:latest \
    python3 /scripts/replay_scan.py /data/dump/from_scanner/scan_<ts>.h5 \
      --address mri-marshal --port 9100 [--fps 10 | --full-speed]
"""
import argparse, io, socket, sys, threading, time
sys.path.insert(0, "/opt/code/python-ismrmrd-server")
import ismrmrd
import constants
from connection import Connection

ACQ_TAG = constants.MrdMessageIdentifier.pack(
    constants.MRD_MESSAGE_ISMRMRD_ACQUISITION)


def reader_loop(sock):
    inc = Connection(sock, False)
    try:
        for _ in inc:   # drain images/text/close pushed back by the marshal
            pass
    except Exception:
        pass


def main():
    p = argparse.ArgumentParser()
    p.add_argument("file")
    p.add_argument("--address", default="mri-marshal")
    p.add_argument("--port", type=int, default=9100)
    p.add_argument("--config", default="invertcontrast")
    p.add_argument("--group", default="dataset")
    p.add_argument("--fps", type=float, default=0.0,
                   help="fixed frames/s pacing (overrides original timing)")
    p.add_argument("--full-speed", action="store_true",
                   help="no pacing at all")
    p.add_argument("--tick-ms", type=float, default=2.5,
                   help="ms per acquisition_time_stamp tick (default 2.5)")
    p.add_argument("--fallback-fps", type=float, default=10.0,
                   help="rate used when the recording has no timestamps")
    p.add_argument("--preload", action="store_true",
                   help="read+serialize the whole file into RAM before "
                        "sending — use for timing-accurate k-space replay "
                        "(HDF5 per-record reads cap streaming at ~60 acqs/s)")
    args = p.parse_args()

    ds = ismrmrd.Dataset(args.file, args.group, False)
    xml = ds.read_xml_header()
    # h5py hands back bytes; send_metadata needs str, and any stray NUL
    # breaks the recon's XML parser.
    if isinstance(xml, bytes):
        xml = xml.decode("utf-8")
    xml = xml.strip("\x00").strip()

    nacq = nimg = 0
    try:
        nacq = ds.number_of_acquisitions()
    except LookupError:
        pass
    try:
        nimg = ds.number_of_images("image_0")
    except LookupError:
        pass
    if nacq == 0 and nimg == 0:
        sys.exit("no acquisitions or images in the file")

    mode = "full-speed" if args.full_speed else (
        f"fixed {args.fps} fps" if args.fps > 0 else "original timing")
    print(f"replaying {args.file}: {nacq} acqs, {nimg} imgs -> "
          f"{args.address}:{args.port} ({mode})", flush=True)

    sock = None
    conn = None

    def connect():
        # Deferred until after any --preload so the scan session doesn't sit
        # idle on the wire during the slow HDF5 read.
        nonlocal sock, conn
        sock = socket.create_connection((args.address, args.port))
        threading.Thread(target=reader_loop, args=(sock,), daemon=True).start()
        conn = Connection(sock, False)
        conn.send_config_file(args.config)
        conn.send_metadata(xml)

    t_start = time.time()
    sent = 0

    if nacq:
        first = ds.read_acquisition(0)
        has_ts = False
        # original timing needs timestamps; probe a few records
        for i in range(0, nacq, max(1, nacq // 16)):
            if ds.read_acquisition(i).acquisition_time_stamp:
                has_ts = True
                break
        fps = args.fps
        if not args.full_speed and fps <= 0 and not has_ts:
            fps = args.fallback_fps
            print(f"  recording has no timestamps -> pacing at "
                  f"{fps} frames/s (--fallback-fps)", flush=True)

        ts0 = first.acquisition_time_stamp
        rep0 = first.idx.repetition
        frame = 0

        # --preload: pay the slow HDF5 read up front so the send loop's
        # timing is exact. Without it, frames stream straight from the file
        # (fine for image archives and slow k-space; the per-record HDF5
        # read caps throughput at roughly 60 acqs/s).
        def frames_from_file():
            nonlocal sent, rep0
            buf, meta = io.BytesIO(), None
            for i in range(nacq):
                acq = ds.read_acquisition(i)
                if meta is None:
                    meta = (acq.idx.repetition, acq.acquisition_time_stamp)
                if acq.idx.repetition != rep0:
                    yield meta, buf.getvalue()
                    buf, meta = io.BytesIO(), (acq.idx.repetition,
                                               acq.acquisition_time_stamp)
                    rep0 = acq.idx.repetition
                buf.write(ACQ_TAG)
                acq.serialize_into(buf.write)
                sent += 1
            yield meta, buf.getvalue()

        frames = frames_from_file()
        if args.preload:
            t_read = time.time()
            frames = list(frames)
            print(f"  preloaded {sent} acqs / {len(frames)} frames "
                  f"in {time.time() - t_read:.1f}s", flush=True)
        connect()
        t_start = time.time()

        for (rep, ts), payload in frames:
            if not args.full_speed and frame > 0:
                if fps > 0:
                    target = t_start + frame / fps
                elif has_ts and ts:
                    target = t_start + (ts - ts0) * args.tick_ms / 1000.0
                else:
                    target = 0
                now = time.time()
                if target > now:
                    time.sleep(target - now)
            sock.sendall(payload)
            frame += 1

    if conn is None:
        connect()
        t_start = time.time()
    for i in range(nimg):
        img = ds.read_image("image_0", i)
        if not args.full_speed:
            fps = args.fps if args.fps > 0 else args.fallback_fps
            target = t_start + i / fps
            now = time.time()
            if target > now:
                time.sleep(target - now)
        conn.send_image(img)
        sent += 1

    conn.send_close()
    time.sleep(0.5)
    try:
        sock.close()
    except Exception:
        pass
    print(f"replayed {sent} records in {time.time() - t_start:.1f}s", flush=True)


if __name__ == "__main__":
    main()
