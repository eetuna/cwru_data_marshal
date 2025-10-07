# tools/make_image_message.py
# Usage: python3 tools/make_image_message.py [output_path]
# Default: image_message.bin in CWD
import sys
from pathlib import Path

import numpy as np
import ismrmrd

out = sys.argv[1] if len(sys.argv) > 1 else "data/image_message.bin"

out_path = Path(out)
if out_path.parent != Path(""):
    out_path.parent.mkdir(parents=True, exist_ok=True)

# Tiny 4x4 float32 image with ramp data
data = np.arange(16, dtype=np.float32).reshape(4, 4)
img = ismrmrd.Image.from_array(data)

# Minimal header fields
img.head.channels  = 1
img.head.data_type = ismrmrd.DATATYPE_FLOAT  # float32

with open(out, "wb") as f:
    f.write(img.getHead().tobytes())
    f.write(img.data.tobytes())

print(f"Wrote {out} ({len(img.getHead().tobytes()) + len(img.data.tobytes())} bytes)")
