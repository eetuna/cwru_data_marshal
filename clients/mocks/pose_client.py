#!/usr/bin/env python3
"""
Mock Pose Client - Sends simulated robot/tracker poses to MRI Marshal

Pure Python implementation (no external dependencies required).
Sends POST requests to /v1/pose/update endpoint with synthetic pose data.
"""

import json
import time
import math
import argparse
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError


def generate_circular_trajectory(t, radius=50.0, height=100.0, frequency=0.1):
    """Generate a smooth circular trajectory in 3D space."""
    angle = 2 * math.pi * frequency * t
    x = radius * math.cos(angle)
    y = radius * math.sin(angle)
    z = height
    cos_a, sin_a = math.cos(angle), math.sin(angle)
    R = [cos_a, -sin_a, 0.0, sin_a, cos_a, 0.0, 0.0, 0.0, 1.0]
    return [x, y, z], R


def generate_linear_trajectory(t, velocity=10.0):
    """Generate a simple linear back-and-forth trajectory."""
    amplitude = 100.0
    x = amplitude * math.sin(2 * math.pi * velocity * t / (2 * amplitude))
    R = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    return [x, 0.0, 100.0], R


def send_pose_update(endpoint, position, rotation):
    """Send pose update to the MRI Marshal endpoint."""
    url = f"{endpoint}/v1/pose/update"
    payload = {"p": [round(v, 3) for v in position], "R": [round(v, 6) for v in rotation]}

    try:
        req = Request(
            url,
            data=json.dumps(payload).encode('utf-8'),
            headers={'Content-Type': 'application/json', 'User-Agent': 'mock-pose-client/1.0'},
            method='POST'
        )
        with urlopen(req, timeout=5.0) as response:
            return response.status == 200
    except (HTTPError, URLError) as e:
        print(f"[ERROR] {e}")
        return False


def main():
    parser = argparse.ArgumentParser(description='Mock Pose client', formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument('--endpoint', default='http://localhost:8080', help='MRI Marshal endpoint')
    parser.add_argument('--interval', type=float, default=0.1, help='Interval between updates (seconds)')
    parser.add_argument('--count', type=int, default=0, help='Number of poses to send (0 = infinite)')
    parser.add_argument('--trajectory', choices=['circular', 'linear'], default='circular', help='Motion type')
    parser.add_argument('--radius', type=float, default=50.0, help='Circular trajectory radius (mm)')
    parser.add_argument('--height', type=float, default=100.0, help='Z height (mm)')
    parser.add_argument('--frequency', type=float, default=0.1, help='Rotation frequency (Hz)')
    parser.add_argument('--velocity', type=float, default=10.0, help='Linear velocity (mm/s)')
    args = parser.parse_args()

    print(f"[*] Mock Pose Client: {args.endpoint}, Trajectory={args.trajectory}")

    t_start = time.time()
    sent_count = 0
    success_count = 0

    try:
        while True:
            t = time.time() - t_start
            if args.trajectory == 'circular':
                position, rotation = generate_circular_trajectory(t, args.radius, args.height, args.frequency)
            else:
                position, rotation = generate_linear_trajectory(t, args.velocity)

            if send_pose_update(args.endpoint, position, rotation):
                success_count += 1
            sent_count += 1

            if sent_count % 10 == 0:
                print(f"[{sent_count:04d}] p=[{position[0]:7.2f}, {position[1]:7.2f}, {position[2]:7.2f}] | Success: {100.0 * success_count / sent_count:.1f}%")

            if args.count > 0 and sent_count >= args.count:
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n[*] Interrupted")

    print(f"\n[*] Total: {sent_count}, Success: {success_count}")


if __name__ == "__main__":
    main()
