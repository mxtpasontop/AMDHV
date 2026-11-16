#!/usr/bin/env python3
"""Generate a stable source/test/configuration identity without editing source."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
ROOT=Path(__file__).resolve().parent.parent


def identity(root=ROOT):
    files=[*root.glob('src/*'),*root.glob('Headers/Lilu/*'),root/'Info.plist',root/'Makefile',
           root/'dependencies.lock.json',*root.glob('config/*.json'),root/'test_after_reboot.sh',
           *root.glob('tools/*.py'),*root.glob('tests/**/*'),*root.glob('.github/workflows/*')]
    files=sorted({p for p in files if p.is_file() and '__pycache__' not in p.parts and p.suffix!='.pyc' and p.name!='.DS_Store'})
    h=hashlib.sha256()
    for path in files:
        h.update(str(path.relative_to(root)).encode()+b'\0')
        with path.open('rb') as source:
            for chunk in iter(lambda:source.read(65536),b''):
                h.update(chunk)
        h.update(b'\0')
    return h.hexdigest()


def main():
    content='#pragma once\n#define AMDHV_BUILD_ID '+json.dumps(identity())+'\n'
    output=Path(sys.argv[1]);output.parent.mkdir(parents=True,exist_ok=True)
    if not output.exists() or output.read_text()!=content:
        with tempfile.NamedTemporaryFile(mode='w',dir=output.parent,prefix='.identity-',delete=False) as handle:
            handle.write(content);temporary=Path(handle.name)
        try:
            os.replace(temporary,output)
        finally:
            temporary.unlink(missing_ok=True)


if __name__=='__main__':
    main()
