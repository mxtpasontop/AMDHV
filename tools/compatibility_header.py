#!/usr/bin/env python3
import json
from pathlib import Path
import sys
import uuid
import os
import tempfile
root = Path(__file__).resolve().parent.parent
config = json.loads((root/'config/compatibility.json').read_text())
text = '#pragma once\nnamespace Compatibility {\n'
for field,name in [('kernel_uuid','kernelUUID'),('applehv_uuid','applehvUUID')]:
    text += 'constexpr unsigned char '+name+'[16] = {'+','.join(str(b) for b in uuid.UUID(config[field]).bytes)+'};\n'
text += '}\n'
out = Path(sys.argv[1]);out.parent.mkdir(parents=True,exist_ok=True)
if not out.exists() or out.read_text()!=text:
    with tempfile.NamedTemporaryFile(mode='w',dir=out.parent,prefix='.compatibility-',delete=False) as handle:
        handle.write(text);temporary=Path(handle.name)
    try:
        os.replace(temporary,out)
    finally:
        temporary.unlink(missing_ok=True)
