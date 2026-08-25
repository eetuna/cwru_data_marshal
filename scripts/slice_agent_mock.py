#!/usr/bin/env python3
"""Mock of Andrew's `slice_agent --listen` for marshal e2e tests.

Speaks the real protocol (dynamic-slice-position-main/agent/PROTOCOL.md):
TCP server, one client at a time, no framing, fixed 56-byte little-endian
SliceCommand packets:
    double tx, ty, tz   (mm, PCS)      double rx, ry, rz   (degrees)
    uint32 flags        (0 update, 0xDEAD quit)   uint32 pad
Prints one JSON line per packet (CMD {...}) and connection events, so a
test can grep what the marshal actually sent. Writes nothing to shared
memory. Exits after --max-clients clients have disconnected (default: run
until killed) or after --timeout seconds.

    python3 slice_agent_mock.py [--port 9270] [--max-clients 1] [--timeout 60]
"""
import argparse, json, socket, struct, sys, time

FMT = "<ddddddII"
SIZE = struct.calcsize(FMT)
assert SIZE == 56

def log(kind, **kw):
    print(kind + (" " + json.dumps(kw) if kw else ""), flush=True)

def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9270)
    ap.add_argument("--max-clients", type=int, default=0, help="exit after N clients (0 = forever)")
    ap.add_argument("--timeout", type=float, default=0, help="exit after N seconds (0 = never)")
    a = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", a.port))
    srv.listen(1)
    srv.settimeout(0.5)
    log("LISTENING", port=a.port)
    t0 = time.time()
    clients = 0
    while True:
        if a.timeout and time.time() - t0 > a.timeout:
            log("TIMEOUT")
            return 2
        try:
            conn, addr = srv.accept()
        except socket.timeout:
            continue
        clients += 1
        log("CONNECTED", peer=addr[0], n=clients)
        frame = 0
        with conn:
            while True:
                raw = recv_exact(conn, SIZE)
                if raw is None:
                    log("DISCONNECTED", frames=frame)
                    break
                tx, ty, tz, rx, ry, rz, flags, _pad = struct.unpack(FMT, raw)
                log("CMD", frame=frame, tx=tx, ty=ty, tz=tz, rx=rx, ry=ry, rz=rz, flags=flags)
                frame += 1
                if flags == 0xDEAD:
                    log("QUIT", frames=frame)
                    break
        if a.max_clients and clients >= a.max_clients:
            return 0

if __name__ == "__main__":
    sys.exit(main())
