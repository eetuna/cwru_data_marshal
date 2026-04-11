#!/usr/bin/env python3
"""
Integration tests for the MRI marshal MRD TCP path.

These tests intentionally use MRD TCP for scanner/recon data. HTTP is only used
for health and latest-file query/control checks.
"""

import json
import os
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request

MARSHAL_BIN = os.environ.get("MARSHAL_BIN", "./build/marshal")
KSPACE_STREAMER = os.environ.get("KSPACE_STREAMER", "./build/kspace_streamer")
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


class TestMarshalIntegration(unittest.TestCase):
    marshal_proc = None
    recon_proc = None
    dump_dir = None

    @classmethod
    def setUpClass(cls):
        cls.dump_dir = tempfile.mkdtemp(prefix="marshal_test_")

    def start_marshal(self, with_recon=False):
        cmd = [
            MARSHAL_BIN,
            "--http", f"0.0.0.0:{MARSHAL_HTTP_PORT}",
            "--mrd-port", str(MARSHAL_MRD_PORT),
            "--dump-dir", self.dump_dir,
        ]
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

    def base(self):
        return f"http://localhost:{MARSHAL_HTTP_PORT}"

    def test_t1_scanner_data_archived_without_recon(self):
        self.start_marshal(with_recon=False)
        res = self.run_kspace(volumes=1)
        self.assertEqual(res.returncode, 0, res.stderr + res.stdout)

        scanner_dir = os.path.join(self.dump_dir, "from_scanner")
        h5_files = [f for f in os.listdir(scanner_dir) if f.endswith(".h5")]
        self.assertGreater(len(h5_files), 0, "No HDF5 file in from_scanner/")

    def test_t2_roundtrip_pushes_images_back_to_scanner(self):
        self.start_recon()
        self.start_marshal(with_recon=True)
        res = self.run_kspace(volumes=1)
        self.assertEqual(res.returncode, 0, res.stderr + res.stdout)
        self.assertIn("received", res.stdout)

        status, body = http_get(f"{self.base()}/image/latest")
        self.assertEqual(status, 200)
        latest = json.loads(body)
        self.assertFalse(latest["error"])
        self.assertTrue(os.path.exists(latest["path"]))

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


if __name__ == "__main__":
    unittest.main()
