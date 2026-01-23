#!/usr/bin/env python3
"""
Mock ECG Client - Sends simulated ECG signals to MRI Marshal

Pure Python implementation (no external dependencies required).
Sends POST requests to /v1/bio/signal endpoint with synthetic ECG data.
"""

import json
import time
import math
import random
import argparse
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError


def generate_ecg_sample(t, baseline_hz=1.2):
    """
    Generate a synthetic ECG-like waveform using sinusoidal components.
    """
    p_wave = 0.15 * math.sin(2 * math.pi * baseline_hz * t)
    qrs_offset = (t * baseline_hz) % 1.0
    if 0.15 < qrs_offset < 0.25:
        qrs_wave = 2.0 * math.sin(2 * math.pi * 10 * (qrs_offset - 0.15))
    else:
        qrs_wave = 0.0
    t_wave = 0.3 * math.sin(2 * math.pi * baseline_hz * t - math.pi / 3)
    noise = random.gauss(0, 0.05)
    return p_wave + qrs_wave + t_wave + noise


def send_ecg_signal(endpoint, source, data, rate_hz):
    """Send ECG signal data to the MRI Marshal endpoint."""
    url = f"{endpoint}/v1/bio/signal"
    payload = {"source": source, "data": data, "rate_hz": rate_hz}

    try:
        req = Request(
            url,
            data=json.dumps(payload).encode('utf-8'),
            headers={'Content-Type': 'application/json', 'User-Agent': 'mock-ecg-client/1.0'},
            method='POST'
        )
        with urlopen(req, timeout=5.0) as response:
            return response.status == 200
    except (HTTPError, URLError) as e:
        print(f"[ERROR] {e}")
        return False


def main():
    parser = argparse.ArgumentParser(description='Mock ECG client', formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument('--endpoint', default='http://localhost:8080', help='MRI Marshal endpoint')
    parser.add_argument('--source', default='ecg_monitor', help='ECG source identifier')
    parser.add_argument('--interval', type=float, default=1.0, help='Interval between transmissions (seconds)')
    parser.add_argument('--count', type=int, default=0, help='Number of signals to send (0 = infinite)')
    parser.add_argument('--rate-hz', type=float, default=100.0, help='ECG sampling rate in Hz')
    parser.add_argument('--samples', type=int, default=100, help='Samples per transmission')
    parser.add_argument('--heart-rate', type=float, default=72.0, help='Simulated heart rate (BPM)')
    args = parser.parse_args()

    print(f"[*] Mock ECG Client: {args.endpoint}, HR={args.heart_rate} BPM, Rate={args.rate_hz} Hz")

    baseline_hz = args.heart_rate / 60.0
    t_offset = 0.0
    sent_count = 0
    success_count = 0

    try:
        while True:
            samples = []
            dt = 1.0 / args.rate_hz
            for i in range(args.samples):
                t = t_offset + i * dt
                samples.append(round(generate_ecg_sample(t, baseline_hz), 4))

            if send_ecg_signal(args.endpoint, args.source, samples, args.rate_hz):
                success_count += 1
            sent_count += 1
            print(f"[{sent_count:04d}] Sent {len(samples)} samples | Success: {100.0 * success_count / sent_count:.1f}%")

            t_offset += args.samples * dt
            if args.count > 0 and sent_count >= args.count:
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n[*] Interrupted")

    print(f"\n[*] Total: {sent_count}, Success: {success_count}")


if __name__ == "__main__":
    main()
