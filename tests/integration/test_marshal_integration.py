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


def wait_for_latest_file(base_url, timeout=5):
    deadline = time.time() + timeout
    latest = None
    while time.time() < deadline:
        status, body = http_get(f"{base_url}/image/latest")
        if status == 200:
            latest = json.loads(body)
            path = latest.get("path", "")
            if path and os.path.exists(path):
                return latest
        time.sleep(0.05)
    return latest


def latest_image_count(path):
    ds = ismrmrd.Dataset(path, '/dataset', create_if_needed=False)
    if hasattr(ds, 'number_of_images'):
        return ds.number_of_images('image_0')
    return ds.getNumberOfImages('image_0')


class TestMarshalIntegration(unittest.TestCase):
    marshal_proc = None
    recon_proc = None
    dump_dir = None

    def setUp(self):
        self.dump_dir = tempfile.mkdtemp(prefix="marshal_test_")

    def start_marshal(self, with_recon=False, dump=True):
        cmd = [
            MARSHAL_BIN,
            "--http", f"0.0.0.0:{MARSHAL_HTTP_PORT}",
            "--mrd-port", str(MARSHAL_MRD_PORT),
            "--dump-dir", self.dump_dir,
        ]
        if dump:
            cmd.append("--dump")
        if with_recon:
            cmd += ["--recon-host", "localhost", "--recon-port", str(RECON_PORT)]
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

    def live_scanner_dir(self):
        return os.path.join(self.dump_dir, "live", "from_scanner")

    def live_recon_dir(self):
        return os.path.join(self.dump_dir, "live", "from_reconstruction")

    def dump_scanner_dir(self):
        return os.path.join(self.dump_dir, "dump", "from_scanner")

    def dump_recon_dir(self):
        return os.path.join(self.dump_dir, "dump", "from_reconstruction")

    def test_t1_scanner_data_archived_without_recon(self):
        self.start_marshal(with_recon=False)
        res = self.run_kspace(volumes=1)
        self.assertEqual(res.returncode, 0, res.stderr + res.stdout)

        scanner_dir = self.dump_scanner_dir()
        h5_files = [f for f in os.listdir(scanner_dir) if f.endswith(".h5")]
        self.assertGreater(len(h5_files), 0, "No HDF5 file in from_scanner/")

        status, body = http_get(f"{self.base()}/dump/scanner")
        self.assertEqual(status, 200)
        dump = json.loads(body)
        paths = "\n".join(item["path"] for item in dump)
        self.assertIn(".h5", paths)

        recon_dir = self.dump_recon_dir()
        if os.path.exists(recon_dir):
            recon_h5 = [f for f in os.listdir(recon_dir) if f.endswith(".h5")]
            self.assertEqual(recon_h5, [], "Recon HDF5 should not exist in scanner-only dump")

    def test_t2_roundtrip_pushes_images_back_to_scanner(self):
        self.start_recon()
        self.start_marshal(with_recon=True)
        res = self.run_kspace(volumes=1)
        self.assertEqual(res.returncode, 0, res.stderr + res.stdout)
        self.assertIn("received", res.stdout)

        latest = wait_for_latest_file(self.base())
        self.assertIsNotNone(latest)
        self.assertFalse(latest["error"])
        self.assertTrue(os.path.exists(latest["path"]), latest)

        scanner_dir = self.dump_scanner_dir()
        recon_dir = self.dump_recon_dir()
        self.assertGreater(len([f for f in os.listdir(scanner_dir) if f.endswith(".h5")]), 0)
        self.assertGreater(len([f for f in os.listdir(recon_dir) if f.endswith(".h5")]), 0)

    def test_t2b_scanner_images_are_saved_not_forwarded_to_recon(self):
        self.start_marshal(with_recon=True)
        res = self.run_image_streamer(frames=3)
        self.assertEqual(res.returncode, 0, res.stderr + res.stdout)

        latest = wait_for_latest_file(self.base())
        self.assertIsNotNone(latest)
        self.assertFalse(latest["error"])
        self.assertTrue(os.path.exists(latest["path"]), latest)
        self.assertIn("from_scanner", latest["path"])

        scanner_dir = self.live_scanner_dir()
        recon_dir = self.dump_recon_dir()
        self.assertGreater(len([f for f in os.listdir(scanner_dir)
                                if f.startswith("scan_") and f.endswith(".h5")]), 0)
        self.assertEqual([f for f in os.listdir(recon_dir)
                          if f.startswith("scan_") and f.endswith(".h5")], [])

    def test_t3_dump_off_still_proxies_without_h5_archives(self):
        self.start_recon()
        self.start_marshal(with_recon=True, dump=False)
        res = self.run_kspace(volumes=1)
        self.assertEqual(res.returncode, 0, res.stderr + res.stdout)
        self.assertIn("received", res.stdout)

        latest = wait_for_latest_file(self.base())
        self.assertIsNotNone(latest)
        self.assertFalse(latest["error"])
        self.assertTrue(os.path.exists(latest["path"]))

        for path in (self.dump_scanner_dir(), self.dump_recon_dir()):
            if not os.path.exists(path):
                continue
            dump_h5_files = [f for f in os.listdir(path)
                             if f.startswith("scan_") and f.endswith(".h5")]
            self.assertEqual(dump_h5_files, [],
                             f"{path} should not contain dump H5 files with dump off")

    def test_t4_recon_kill_marshal_survives_and_reconnects(self):
        self.start_recon()
        self.start_marshal(with_recon=True)

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
        self.start_marshal(with_recon=True)
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

            status, body = http_get(f"{self.base()}/image/latest")
            self.assertEqual(status, 200)
            latest = json.loads(body)
            self.assertTrue(latest["error"])
            self.assertTrue(os.path.exists(latest["path"]))
        finally:
            self.stop_proc(scanner)

    def test_t6_multislice_latest_stays_on_completed_stack(self):
        self.start_recon()
        self.start_marshal(with_recon=True)
        scanner = self.start_kspace_async()
        try:
            saw_completed_stack = False
            deadline = time.time() + 15
            while time.time() < deadline:
                latest = wait_for_latest_file(self.base(), timeout=1)
                if not latest or latest.get('error'):
                    continue
                path = latest.get('path', '')
                if not path or not os.path.exists(path):
                    continue
                count = latest_image_count(path)
                if count == 5:
                    saw_completed_stack = True
                    break
            self.assertTrue(saw_completed_stack, 'Never observed a completed 5-slice latest stack')

            stable_deadline = time.time() + 3
            while time.time() < stable_deadline:
                latest = wait_for_latest_file(self.base(), timeout=1)
                self.assertIsNotNone(latest)
                self.assertFalse(latest['error'])
                count = latest_image_count(latest['path'])
                self.assertEqual(count, 5, f'Latest stack regressed to partial size {count}')
                time.sleep(0.2)
        finally:
            self.stop_proc(scanner)


if __name__ == "__main__":
    unittest.main()
