#!/usr/bin/env python3
"""
mrd_bench.py — marshal-only performance harness (byte-pump endpoints).

  gen        build preamble.bin / acq_frame.bin / image.bin (valid ISMRMRD
             wire bytes) into --dir
  sink       accept one connection; exact-read tagged messages, discard;
             reply CLOSE on CLOSE. (recon stand-in for forward tests)
  echo       like sink, but after each complete FRAME (nacq acqs) send
             image.bin back. (recon stand-in for latency tests)
  blast      connect; send preamble, then N frames (paced by --fps or
             unpaced); drain replies on a thread; report rates and, with
             --rtt, per-frame round-trip stats.
  retblast   sink that, after the first frame, fires --count copies of
             image.bin back-to-back, then CLOSE on CLOSE.

Run inside the fire-python image (gen needs ismrmrd + /scripts/fire_stream.py).
"""
import argparse, io, os, socket, struct, sys, time, threading

def build_files(dirpath, matrix, coils, slices):
    sys.path.insert(0, "/opt/code/python-ismrmrd-server")
    import ismrmrd
    import numpy as np
    sys.path.insert(0, "/scripts")
    from fire_stream import build_header

    xml = build_header(matrix, coils, slices)
    pre = io.BytesIO()
    pre.write(struct.pack("<H", 1) + b"invertcontrast".ljust(1024, b"\0"))
    payload = xml + b"\0" if isinstance(xml, bytes) else (xml + "\0").encode()
    pre.write(struct.pack("<H", 3) + struct.pack("<I", len(payload)) + payload)

    acq = ismrmrd.Acquisition()
    acq.resize(matrix, coils)
    acq.available_channels = coils
    acq.center_sample = matrix // 2
    acq.read_dir[0] = 1.0; acq.phase_dir[1] = 1.0; acq.slice_dir[2] = 1.0
    data = (np.random.standard_normal((coils, matrix))
            + 1j * np.random.standard_normal((coils, matrix))).astype(np.complex64)
    frame = io.BytesIO()
    for s in range(slices):
        for line in range(matrix):
            acq.idx.slice = s
            acq.idx.kspace_encode_step_1 = line
            acq.clearAllFlags()
            if line == matrix - 1:
                acq.setFlag(ismrmrd.ACQ_LAST_IN_ENCODE_STEP1)
                acq.setFlag(ismrmrd.ACQ_LAST_IN_SLICE)
            acq.data[:] = data
            frame.write(struct.pack("<H", 1008))
            acq.serialize_into(frame.write)

    vol = np.abs(np.random.standard_normal((slices, matrix, matrix))).astype(np.float32)
    im = ismrmrd.Image.from_array(vol, transpose=False)
    im.image_index = 1
    img = io.BytesIO()
    img.write(struct.pack("<H", 1022))
    im.serialize_into(img.write)

    os.makedirs(dirpath, exist_ok=True)
    for name, b in (("preamble.bin", pre.getvalue()),
                    ("acq_frame.bin", frame.getvalue()),
                    ("image.bin", img.getvalue())):
        with open(os.path.join(dirpath, name), "wb") as f:
            f.write(b)
        print(f"{name}: {len(b)} bytes", flush=True)
    nacq = slices * matrix
    print(f"acqs/frame={nacq} bytes/acq_msg={len(frame.getvalue())//nacq}", flush=True)


def recv_exact(sock, n):
    b = sock.recv(n, socket.MSG_WAITALL)
    if len(b) != n:
        raise ConnectionError("short read")
    return b


def serve(args, echo=False, retblast=False):
    img = open(os.path.join(args.dir, "image.bin"), "rb").read() if (echo or retblast) else b""
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    print(f"listening :{args.port}", flush=True)
    while True:
        s, peer = srv.accept()
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f"accepted {peer}", flush=True)
        nacq_seen = 0
        frames_seen = 0
        fired = False
        t0 = time.time()
        nbytes = 0
        try:
            while True:
                tag = struct.unpack("<H", recv_exact(s, 2))[0]
                if tag == 1:
                    nbytes += 1026; recv_exact(s, 1024)
                elif tag in (2, 3, 5):
                    ln = struct.unpack("<I", recv_exact(s, 4))[0]
                    nbytes += 6 + ln; recv_exact(s, ln)
                elif tag == 1008:
                    recv_exact(s, args.acq_body)
                    nbytes += 2 + args.acq_body
                    nacq_seen += 1
                    if nacq_seen % args.nacq == 0:
                        frames_seen += 1
                        if echo:
                            s.sendall(img)
                        if retblast and not fired:
                            fired = True
                            t = time.time()
                            for _ in range(args.count):
                                s.sendall(img)
                            dt = time.time() - t
                            print(f"retblast: {args.count} images in {dt:.2f}s "
                                  f"= {args.count/dt:.0f} img/s", flush=True)
                elif tag == 4:
                    dt = time.time() - t0
                    print(f"session: {frames_seen} frames {nacq_seen} acqs "
                          f"{nbytes/1e6:.1f}MB in {dt:.2f}s = "
                          f"{nacq_seen/dt:.0f} acq/s {nbytes/dt/1e6:.0f} MB/s",
                          flush=True)
                    s.sendall(struct.pack("<H", 4))
                    break
                else:
                    print(f"unexpected tag {tag}", flush=True)
                    break
        except (ConnectionError, OSError) as e:
            print(f"conn ended: {e}", flush=True)
        s.close()
        if args.once:
            break


def blast(args):
    pre = open(os.path.join(args.dir, "preamble.bin"), "rb").read()
    frame = open(os.path.join(args.dir, "acq_frame.bin"), "rb").read()
    imglen = len(open(os.path.join(args.dir, "image.bin"), "rb").read())

    s = socket.create_connection((args.address, args.port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    rtts = []
    got_close = threading.Event()
    sent_ts = {}
    lock = threading.Lock()

    def reader():
        n_img = 0
        try:
            while True:
                tag = struct.unpack("<H", recv_exact(s, 2))[0]
                if tag == 1022:
                    recv_exact(s, imglen - 2)
                    n_img += 1
                    if args.rtt:
                        with lock:
                            ts = sent_ts.pop(n_img, None)
                        if ts is not None:
                            rtts.append(time.time() - ts)
                elif tag in (2, 3, 5):
                    ln = struct.unpack("<I", recv_exact(s, 4))[0]
                    recv_exact(s, ln)
                elif tag == 4:
                    print(f"reader: CLOSE after {n_img} images", flush=True)
                    got_close.set()
                    return
                else:
                    print(f"reader: unexpected tag {tag}", flush=True)
                    got_close.set()
                    return
        except (ConnectionError, OSError) as e:
            print(f"reader ended: {e}", flush=True)
            got_close.set()

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    s.sendall(pre)
    period = 1.0 / args.fps if args.fps > 0 else 0.0
    t0 = time.time()
    for i in range(1, args.frames + 1):
        t = time.time()
        if args.rtt:
            with lock:
                sent_ts[i] = t
        s.sendall(frame)
        if period:
            dt = time.time() - t
            if period > dt:
                time.sleep(period - dt)
    t1 = time.time()
    nb = len(frame) * args.frames
    print(f"blast: {args.frames} frames {nb/1e6:.1f}MB in {t1-t0:.2f}s = "
          f"{args.frames/(t1-t0):.1f} fps {nb/(t1-t0)/1e6:.0f} MB/s", flush=True)
    s.sendall(struct.pack("<H", 4))
    got_close.wait(timeout=30)
    if args.rtt and rtts:
        rtts.sort()
        n = len(rtts)
        print(f"rtt n={n} p50={rtts[n//2]*1000:.1f}ms "
              f"p90={rtts[int(n*0.9)]*1000:.1f}ms "
              f"max={rtts[-1]*1000:.1f}ms", flush=True)
    s.close()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("mode", choices=["gen", "sink", "echo", "blast", "retblast"])
    p.add_argument("--dir", default="/bench")
    p.add_argument("--matrix", type=int, default=96)
    p.add_argument("--coils", type=int, default=8)
    p.add_argument("--slices", type=int, default=5)
    p.add_argument("--address", default="mri-marshal")
    p.add_argument("--port", type=int, default=9100)
    p.add_argument("--fps", type=float, default=0)
    p.add_argument("--frames", type=int, default=200)
    p.add_argument("--count", type=int, default=2000)
    p.add_argument("--rtt", action="store_true")
    p.add_argument("--once", action="store_true")
    p.add_argument("--acq-body", type=int, default=0)
    p.add_argument("--nacq", type=int, default=0)
    args = p.parse_args()

    if not args.nacq:
        args.nacq = args.matrix * args.slices
    if args.mode == "gen":
        build_files(args.dir, args.matrix, args.coils, args.slices)
    elif args.mode == "sink":
        serve(args, echo=False)
    elif args.mode == "echo":
        serve(args, echo=True)
    elif args.mode == "retblast":
        serve(args, retblast=True)
    elif args.mode == "blast":
        blast(args)


if __name__ == "__main__":
    main()
