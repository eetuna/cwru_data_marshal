#!/usr/bin/env python3
"""
Pose Client - Sends simulated robot poses via POST /pose

JSON body: {"position": [x, y, z], "orientation": [R00..R22]}
"""

import argparse
import json
import math
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser(description='Pose client')
    parser.add_argument('--http', default='http://localhost:8080')
    parser.add_argument('--interval', type=float, default=0.5)
    parser.add_argument('--count', type=int, default=0, help='0=infinite')
    args = parser.parse_args()

    print(f"pose_client: sending poses to {args.http}/pose")

    counter = 0
    while args.count == 0 or counter < args.count:
        t = counter * 0.1
        pose = {
            "position": [
                10.0 * math.cos(t),
                10.0 * math.sin(t),
                5.0 + math.sin(t * 0.3)
            ],
            "orientation": [
                math.cos(t), -math.sin(t), 0,
                math.sin(t),  math.cos(t), 0,
                0,            0,           1
            ]
        }

        body = json.dumps(pose).encode()
        req = urllib.request.Request(
            f"{args.http}/pose",
            data=body,
            headers={'Content-Type': 'application/json'},
            method='POST'
        )
        try:
            with urllib.request.urlopen(req, timeout=5) as resp:
                if counter % 10 == 0:
                    print(f"pose {counter}: HTTP {resp.status}")
        except Exception as e:
            print(f"pose {counter}: error: {e}")

        counter += 1
        time.sleep(args.interval)


if __name__ == '__main__':
    main()
