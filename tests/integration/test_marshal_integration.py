#!/usr/bin/env python3
"""
Integration tests for the MRI marshal MRD TCP path.

These tests intentionally use MRD TCP for scanner/recon data. HTTP is only used
for health and latest-file query/control checks.
"""

import json
import os
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request

import ismrmrd

MARSHAL_BIN = os.environ.get("MARSHAL_BIN", "./build/marshal")
KSPACE_STREAMER = os.environ.get("KSPACE_STREAMER", "./build/kspace_streamer")
IMAGE_STREAMER = os.environ.get("IMAGE_STREAMER", "./build/image_streamer")
MOCK_RECON = os.environ.get("MOCK_RECON", "./docker/mock-recon/mock_recon.py")
MARSHAL_HTTP_PORT = 18080
MARSHAL_MRD_PORT = 19100
RECON_PORT = 19002


def wait_for_port(port, host="localhost", timeout=10):
    import socket
    start = time.time()
    while time.time() - start < timeout:
        try:
            with socket.create_connection((host, port), timeout=1):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def http_get(url, timeout=5):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            return resp.status, resp.read().decode()
    except Exception as e:
        return 0, str(e)


def get_latest(base_url):
    status, body = http_get(f"{base_url}/image/latest")
    return json.loads(body) if status == 200 else None


def wait_for_latest_file(base_url, timeout=5):
    deadline = time.time() + timeout
    latest = None
    while time.time() < deadline:
        status, body = http_get(f"{base_url}/image/latest")
        if status == 200:
            latest = json.loads(body)
            path = latest.get("path") or ""
            if path and os.path.exists(path):
                return latest
        time.sleep(0.05)
    return latest


def latest_image_count(path):
    ds = ismrmrd.Dataset(path, '/dataset', create_if_needed=False)
    if hasattr(ds, 'number_of_images'):
        return ds.number_of_images('image_0')
    return ds.getNumberOfImages('image_0')


def wait_for_h5(dirpath, timeout=10):
    """Wait for at least one .h5 file to appear in dirpath (e.g. the dump
    recorder's spool->H5 conversion, which runs at scan close)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.isdir(dirpath) and any(f.endswith('.h5') for f in os.listdir(dirpath)):
            return True
        time.sleep(0.1)
    return False


class TestMarshalIntegration(unittest.TestCase):
    marshal_proc = None
    recon_proc = None
    dump_dir = None

    def setUp(self):
        self.dump_dir = tempfile.mkdtemp(prefix="marshal_test_")

    def start_marshal(self):
        # Recon is always configured; tests without a recon simply do not
        # start the mock, so the marshal finds nothing listening there.
        cmd = [
            MARSHAL_BIN,
            "--http", f"0.0.0.0:{MARSHAL_HTTP_PORT}",
            "--mrd-port", str(MARSHAL_MRD_PORT),
            "--dump-dir", self.dump_dir,
            "--recon-host", "localhost", "--recon-port", str(RECON_PORT),
        ]
        self.marshal_proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertTrue(wait_for_port(MARSHAL_HTTP_PORT), "Marshal HTTP did not start")
        self.assertTrue(wait_for_port(MARSHAL_MRD_PORT), "Marshal MRD did not start")

    def start_recon(self):
        self.recon_proc = subprocess.Popen(
            [sys.executable, MOCK_RECON, "--port", str(RECON_PORT)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertTrue(wait_for_port(RECON_PORT), "Mock recon did not start")

    def run_kspace(self, volumes=1, timeout=15):
        cmd = [
            KSPACE_STREAMER,
            "--host", "localhost",
            "--port", str(MARSHAL_MRD_PORT),
            "--volumes", str(volumes),
            "--interval", "0.01",
            "--samples", "32",
            "--lines", "32",
            "--slices", "1",
            "--log-stride", "1",
        ]
        return subprocess.run(cmd, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=timeout)

    def run_image_streamer(self, frames=3, timeout=15):
        cmd = [
            IMAGE_STREAMER,
            "--host", "localhost",
            "--port", str(MARSHAL_MRD_PORT),
            "--frames", str(frames),
            "--interval", "0",
            "--size", "16",
            "--log-stride", "1",
        ]
        return subprocess.run(cmd, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=timeout)

    def start_kspace_async(self):
        cmd = [
            KSPACE_STREAMER,
            "--host", "localhost",
            "--port", str(MARSHAL_MRD_PORT),
            "--volumes", "0",
            "--interval", "0.02",
            "--samples", "32",
            "--lines", "32",
            "--slices", "5",
            "--log-stride", "1",
        ]
        return subprocess.Popen(cmd, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)

    def stop_proc(self, proc):
        if proc and proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        elif proc:
            proc.wait(timeout=0)
        if proc:
            if proc.stdout:
                proc.stdout.close()
            if proc.stderr:
                proc.stderr.close()

    def stop_all(self):
        for proc in [self.marshal_proc, self.recon_proc]:
            self.stop_proc(proc)
        self.marshal_proc = None
        self.recon_proc = None

    def tearDown(self):
        self.stop_all()
        if self.dump_dir and os.path.exists(self.dump_dir):
            shutil.rmtree(self.dump_dir, ignore_errors=True)

    def base(self):
        return f"http://localhost:{MARSHAL_HTTP_PORT}"

    def dump_scanner_dir(self):
        return os.path.join(self.dump_dir, "dump", "from_scanner")

    def dump_recon_dir(self):
        return os.path.join(self.dump_dir, "dump", "from_reconstruction")

    def test_t1_scanner_data_archived_without_recon(self):
        # Nothing listens at the recon address: the scan still finalizes
        # (RECON FAILED image + CLOSE) and everything the scanner sent is
        # archived.
        self.start_marshal()
        res = self.run_kspace(volumes=1)
        self.assertEqual(res.returncode, 0, res.stderr + res.stdout)
        self.assertIn("received CLOSE from marshal", res.stdout)

        self.assertTrue(wait_for_h5(self.dump_scanner_dir()),
                        "No scanner archive H5 after scan close")
        status, body = http_get(f"{self.base()}/dump/scanner")
        self.assertEqual(status, 200)
        self.assertIn(".h5", "\n".join(item["path"] for item in json.loads(body)))

        latest = get_latest(self.base())
        self.assertIsNone(latest["path"])
        self.assertEqual(latest["status"], "recon failed")

    def test_t2_roundtrip_archives_and_shows_images(self):
        # One mode: the round trip, both archives and the viewer snapshot
        # all happen in the same scan.
        self.start_recon()
        self.start_marshal()
        res = self.run_kspace(volumes=1)
        self.assertEqual(res.returncode, 0, res.stderr + res.stdout)
        self.assertIn("received", res.stdout)

        latest = wait_for_latest_file(self.base())
        self.assertIsNotNone(latest)
        self.assertTrue(os.path.exists(latest["path"]), latest)
        self.assertTrue(wait_for_h5(self.dump_scanner_dir()),
                        "No scanner archive H5 after scan close")
        self.assertTrue(wait_for_h5(self.dump_recon_dir()),
                        "No recon archive H5 after scan close")
        self.assertEqual(get_latest(self.base())["status"], "scan finished")

    def test_t2b_scanner_images_are_archived_and_forwarded_to_recon(self):
        # The marshal is transparent: scanner images go to recon like
        # everything else (the mock recon skips them) and are archived.
        self.start_recon()
        self.start_marshal()
        res = self.run_image_streamer(frames=3)
        self.assertEqual(res.returncode, 0, res.stderr + res.stdout)

        self.assertTrue(wait_for_h5(self.dump_scanner_dir()),
                        "No scanner archive H5 after scan close")
        # The scan is over (archive written); stop the mock and read its log.
        self.recon_proc.send_signal(signal.SIGTERM)
        self.recon_proc.wait(timeout=5)
        recon_log = self.recon_proc.stderr.read()
        self.assertEqual(recon_log.count("IMAGE from client"), 3, recon_log)

    def test_t4_recon_kill_marshal_survives_and_reconnects(self):
        self.start_recon()
        self.start_marshal()

        first = self.run_kspace(volumes=1)
        self.assertEqual(first.returncode, 0, first.stderr + first.stdout)

        self.recon_proc.send_signal(signal.SIGTERM)
        self.recon_proc.wait(timeout=5)
        if self.recon_proc.stdout:
            self.recon_proc.stdout.close()
        if self.recon_proc.stderr:
            self.recon_proc.stderr.close()
        self.recon_proc = None
        time.sleep(0.5)

        status, body = http_get(f"{self.base()}/health")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["status"], "ok")
        self.assertIsNone(self.marshal_proc.poll(), "Marshal exited after recon death")

        self.start_recon()
        second = self.run_kspace(volumes=1)
        self.assertEqual(second.returncode, 0, second.stderr + second.stdout)
        self.assertIn("received", second.stdout)

    def test_t5_recon_failure_pushes_mrd_error_image_to_scanner(self):
        self.start_recon()
        self.start_marshal()
        scanner = self.start_kspace_async()
        try:
            saw_normal_image = False
            deadline = time.time() + 10
            while time.time() < deadline:
                ready, _, _ = select.select([scanner.stdout], [], [], 0.2)
                if not ready:
                    continue
                line = scanner.stdout.readline()
                if "received 1 reconstructed image" in line:
                    saw_normal_image = True
                    break
            self.assertTrue(saw_normal_image, "Scanner did not receive initial recon image")

            self.recon_proc.kill()
            self.recon_proc.wait(timeout=5)
            self.stop_proc(self.recon_proc)
            self.recon_proc = None

            saw_failure_image = False
            deadline = time.time() + 10
            while time.time() < deadline:
                ready, _, _ = select.select([scanner.stdout], [], [], 0.2)
                if not ready:
                    if scanner.poll() is not None:
                        break
                    continue
                line = scanner.stdout.readline()
                if "received recon failure image" in line:
                    saw_failure_image = True
                    break
                if scanner.poll() is not None:
                    break
            self.assertTrue(saw_failure_image, "Scanner did not receive MRD failure image")

            # The viewer is told in words, and keeps the images it had.
            latest = get_latest(self.base())
            self.assertEqual(latest["status"], "recon failed")
            self.assertTrue(os.path.exists(latest["path"]))
        finally:
            self.stop_proc(scanner)

    def test_t6_multislice_latest_grows_then_completes(self):
        # The snapshot holds the newest image per slice position: it grows
        # 1..5 as the first volume arrives, then stays at 5 (later volumes
        # replace, never accumulate), and the generation climbs while
        # streaming.
        self.start_recon()
        self.start_marshal()
        scanner = self.start_kspace_async()
        try:
            saw_completed_stack = False
            first_generation = None
            last_generation = None
            generation_samples = 0
            deadline = time.time() + 15
            while time.time() < deadline:
                latest = wait_for_latest_file(self.base(), timeout=1)
                if not latest:
                    continue
                path = latest.get('path') or ''
                if not path or not os.path.exists(path):
                    continue
                count = latest_image_count(path)
                self.assertGreaterEqual(count, 1)
                self.assertLessEqual(
                    count, 5, f'Latest stack exceeded the 5-slice volume: {count}')
                gen = latest.get('generation')
                if gen is not None:
                    generation_samples += 1
                    if first_generation is None:
                        first_generation = gen
                    if last_generation is not None:
                        self.assertGreaterEqual(
                            gen, last_generation, 'publish generation went backwards')
                    last_generation = gen
                if count == 5:
                    saw_completed_stack = True
                    break
            self.assertTrue(saw_completed_stack, 'Never observed a completed 5-slice latest stack')
            if generation_samples >= 2:
                self.assertGreater(
                    last_generation, first_generation,
                    'publish generation stayed flat while the stack was streaming')
        finally:
            self.stop_proc(scanner)


if __name__ == "__main__":
    unittest.main()
