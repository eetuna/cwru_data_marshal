#!/usr/bin/env python3
"""
HTTP Tracker - Polls GET /image/latest and GET /pose from the marshal.

Prints the latest image path and pose to stdout.
"""

import argparse
import json
import time
import urllib.request


def get_json(url: str):
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=2) as resp:
            return json.loads(resp.read())
    except Exception:
        return None


def main():
    parser = argparse.ArgumentParser(description='HTTP tracker')
    parser.add_argument('--http', default='http://localhost:8080')
    parser.add_argument('--interval', type=float, default=1.0)
    parser.add_argument('--count', type=int, default=0, help='0=infinite')
    args = parser.parse_args()

    print(f"http_tracker: polling {args.http}")

    counter = 0
    while args.count == 0 or counter < args.count:
        image = get_json(f"{args.http}/image/latest")
        pose = get_json(f"{args.http}/pose")

        parts = []
        if image:
            path = image.get("path", "")
            err = image.get("error", False)
            parts.append(f"image={path}" + (" [ERROR]" if err else ""))
        if pose:
            p = pose.get("p", [0, 0, 0])
            parts.append(f"pose=({p[0]:.1f},{p[1]:.1f},{p[2]:.1f})")

        if parts:
            print(f"[{counter}] {' | '.join(parts)}")

        counter += 1
        time.sleep(args.interval)


if __name__ == '__main__':
    main()
