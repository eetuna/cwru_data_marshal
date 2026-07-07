#!/usr/bin/env python3
"""Mock scanner for the slice-command pushback e2e test.

Connects to the marshal over MRD TCP like a python-ismrmrd-server-style
scanner: sends CONFIG + METADATA + one IMAGE with known slice geometry, then
sits in the standard receive loop. When the marshal pushes the
slice-translation TEXT command, connection.py's read_text parses it and this
script prints it (prefixed TEXT_RECEIVED:) and exits 0. Exits 2 on timeout.
"""
import socket, sys, threading, time
sys.path.insert(0, "/opt/code/python-ismrmrd-server")
import numpy as np
import ismrmrd, ismrmrd.xsd
from connection import Connection

addr = sys.argv[1] if len(sys.argv) > 1 else "slice-cmd-test"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 9100

def build_xml():
    h = ismrmrd.xsd.ismrmrdHeader()
    exp = ismrmrd.xsd.experimentalConditionsType(); exp.H1resonanceFrequency_Hz = 128000000
    h.experimentalConditions = exp
    sysinfo = ismrmrd.xsd.acquisitionSystemInformationType(); sysinfo.receiverChannels = 1
    h.acquisitionSystemInformation = sysinfo
    enc = ismrmrd.xsd.encodingType()
    enc.trajectory = ismrmrd.xsd.trajectoryType.CARTESIAN
    for space in ("encodedSpace", "reconSpace"):
        fov = ismrmrd.xsd.fieldOfViewMm(); fov.x = 300.0; fov.y = 300.0; fov.z = 6.0
        ms = ismrmrd.xsd.matrixSizeType(); ms.x = 16; ms.y = 16; ms.z = 1
        sp = ismrmrd.xsd.encodingSpaceType(); sp.matrixSize = ms; sp.fieldOfView_mm = fov
        setattr(enc, space, sp)
    limits = ismrmrd.xsd.encodingLimitsType()
    l1 = ismrmrd.xsd.limitType(); l1.minimum = 0; l1.maximum = 15; l1.center = 8
    limits.kspace_encoding_step_1 = l1
    enc.encodingLimits = limits
    h.encoding.append(enc)
    return h.toXML('utf-8')

sock = socket.create_connection((addr, port))
conn = Connection(sock, False)
conn.send_config_file("invertcontrast")
conn.send_metadata(build_xml())

img = ismrmrd.Image.from_array(np.ones((16, 16), dtype=np.float32), transpose=False)
img.image_index = 0
img.slice = 2
img.position = (10.5, -20.0, 30.0)
img.read_dir = (1.0, 0.0, 0.0)
img.phase_dir = (0.0, 1.0, 0.0)
img.slice_dir = (0.0, 0.0, 1.0)
img.field_of_view = (300.0, 300.0, 6.0)
conn.send_image(img)
print("IMAGE_SENT slice=2 position=(10.5,-20.0,30.0)", flush=True)

def timeout_kill():
    time.sleep(30)
    print("TIMEOUT waiting for TEXT", flush=True)
    import os; os._exit(2)
threading.Thread(target=timeout_kill, daemon=True).start()

for msg in conn:
    if isinstance(msg, str):
        print("TEXT_RECEIVED:" + msg, flush=True)
        conn.send_close()
        sys.exit(0)
print("stream ended without TEXT", flush=True)
sys.exit(3)
