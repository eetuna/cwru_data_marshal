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
img = ismrmrd.Image.from_array(data, transpose=False)

# Minimal header fields (Image.head was removed in ismrmrd>=1.14)
head = img.getHead()
head.channels = 1
head.data_type = ismrmrd.DATATYPE_FLOAT  # float32
img.setHead(head)


header_bytes = bytes(head)
with open(out, "wb") as f:
    f.write(header_bytes)
    f.write(img.data.tobytes())

print(f"Wrote {out} ({len(header_bytes) + len(img.data.tobytes())} bytes)")
