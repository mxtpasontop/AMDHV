#!/usr/bin/env python3
"""Verify exact on-disk UUIDs, hook symbols, and optional Lilu binary exports."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import hashlib
from macho import metadata, x86_slice

ROOT = Path(__file__).resolve().parent.parent


def exports(binary):
    return set(subprocess.check_output(['nm', '-arch', 'x86_64', '-gjU', str(binary)], text=True, stderr=subprocess.STDOUT, timeout=30).splitlines())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path('/System/Library/Kernels/kernel'))
    parser.add_argument('--applehv', type=Path, default=Path('/System/Library/Extensions/AppleHV.kext/Contents/MacOS/AppleHV'))
    parser.add_argument('--lilu', type=Path)
    args = parser.parse_args()
    config = json.loads((ROOT/'config/compatibility.json').read_text())
    policy = json.loads((ROOT/'config/imports.json').read_text())
    source = (ROOT/'src/AMDHV.cpp').read_text()
    for array, kind, binary in [('kernelRequests', 'kernel', args.kernel), ('hvRequests', 'applehv', args.applehv)]:
        block = re.search(r'RouteRequest '+array+r'\[\] = \{(.*?)\n    \};', source, re.S)
        if block is None:
            raise ValueError('missing route definitions: '+array)
        declared = set(re.findall(r'\{"([^"]+)"', block.group(1)))
        required = set(config[kind+'_symbols'])
        if not declared <= required:
            raise ValueError('hook definitions differ from compatibility manifest')
        image = metadata(binary.read_bytes())
        if image['uuid'] != config[kind+'_uuid']:
            raise ValueError(f'{kind}: unsupported UUID {image["uuid"]}')
        symbols = exports(binary)
        if kind == 'kernel':
            required |= set(policy['kernel'])
        if required-symbols:
            raise ValueError(f'{kind}: missing symbols '+', '.join(sorted(required-symbols)))
        print(f'[OK] {kind}: exact UUID and {len(required)} required exports')
    if args.lilu:
        lilu_data=args.lilu.read_bytes()
        lilu_slice=x86_slice(lilu_data)
        lilu_image=metadata(lilu_slice)
        expected=config['lilu']
        if lilu_image['filetype']!=11 or lilu_image['uuid']!=expected['uuid'] or hashlib.sha256(lilu_data).hexdigest()!=expected['file_sha256'] or hashlib.sha256(lilu_slice).hexdigest()!=expected['slice_sha256']:
            raise ValueError('Lilu binary differs from the reviewed deployed image')
        missing = set(policy['lilu'])-exports(args.lilu)
        if missing:
            raise ValueError('Lilu ABI exports missing: '+', '.join(sorted(missing)))
        print('[OK] Lilu exact x86_64 UUID, file/slice hashes and compiled ABI exports match')
    else:
        print('[UNVERIFIED] Lilu binary exports: pass --lilu PATH to check the deployed binary')
    print('Read-only preflight complete. Private layouts, recovery and hardware execution remain unverified.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f'[FAIL] {exc}', file=sys.stderr)
        sys.exit(1)
