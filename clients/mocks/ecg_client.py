#!/usr/bin/env python3
"""
ECG Client - ISMRMRD waveform producer via POST /frame

Sends simulated ECG waveforms as ISMRMRD waveform wire format:
  40B WaveformHeader + (number_of_samples * channels * 4) bytes of uint32 samples

waveform_id=0 is the ECG convention per ISMRMRD.
"""

import argparse
import math
import struct
import time
import urllib.request

WAVEFORM_HEADER_BYTES = 40


def build_waveform(scan_counter: int, nsamples: int = 256,
                   channels: int = 1, sample_rate_hz: float = 500.0) -> bytes:
    """Build an ISMRMRD waveform wire-format message."""
    # WaveformHeader layout (40 bytes, see ismrmrd/waveform.h):
    #   uint16 version (offset 0)
    #   6 bytes padding to offset 8
    #   uint64 flags (offset 8)
    #   uint32 measurement_uid (offset 16)
    #   uint32 scan_counter (offset 20)
    #   uint32 time_stamp (offset 24)
    #   uint16 number_of_samples (offset 28)
    #   uint16 channels (offset 30)
    #   float  sample_time_us (offset 32)
    #   uint16 waveform_id (offset 36)
    #   2 bytes padding to 40

    sample_time_us = 1e6 / sample_rate_hz
    time_stamp = int(time.time() * 1000) & 0xFFFFFFFF

    header = bytearray(WAVEFORM_HEADER_BYTES)
    struct.pack_into('<H', header, 0, 1)                  # version
    struct.pack_into('<Q', header, 8, 0)                   # flags
    struct.pack_into('<I', header, 16, 0)                  # measurement_uid
    struct.pack_into('<I', header, 20, scan_counter)       # scan_counter
    struct.pack_into('<I', header, 24, time_stamp)         # time_stamp
    struct.pack_into('<H', header, 28, nsamples)           # number_of_samples
    struct.pack_into('<H', header, 30, channels)           # channels
    struct.pack_into('<f', header, 32, sample_time_us)     # sample_time_us
    struct.pack_into('<H', header, 36, 0)                  # waveform_id = 0 (ECG)

    # Generate synthetic ECG-like signal (sine wave + R-peak spikes)
    samples = bytearray(nsamples * channels * 4)
    for ch in range(channels):
        for s in range(nsamples):
            t = s / sample_rate_hz
            # Simple ECG approximation
            val = int(2000 + 1000 * math.sin(2 * math.pi * 1.2 * t)  # heart rate ~72 bpm
                      + 3000 * max(0, math.exp(-((t % 0.83 - 0.1) ** 2) / 0.001)))  # R-peak
            val = max(0, min(val, 0xFFFFFFFF))
            offset = (ch * nsamples + s) * 4
            struct.pack_into('<I', samples, offset, val)

    return bytes(header) + bytes(samples)


def main():
    parser = argparse.ArgumentParser(description='ECG waveform producer')
    parser.add_argument('--http', default='http://localhost:8080')
    parser.add_argument('--interval', type=float, default=0.5)
    parser.add_argument('--count', type=int, default=0, help='0=infinite')
    parser.add_argument('--samples', type=int, default=256)
    parser.add_argument('--channels', type=int, default=1)
    args = parser.parse_args()

    print(f"ecg_client: sending waveforms to {args.http}/frame")

    counter = 0
    while args.count == 0 or counter < args.count:
        body = build_waveform(counter, args.samples, args.channels)

        req = urllib.request.Request(
            f"{args.http}/frame",
            data=body,
            headers={'Content-Type': 'application/octet-stream'},
            method='POST'
        )
        try:
            with urllib.request.urlopen(req, timeout=5) as resp:
                if counter % 10 == 0:
                    print(f"waveform {counter}: HTTP {resp.status}")
        except Exception as e:
            print(f"waveform {counter}: error: {e}")

        counter += 1
        time.sleep(args.interval)


if __name__ == '__main__':
    main()
