#!/usr/bin/env python3
"""Verify reviewed instruction evidence against the exact local kernel/AppleHV."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from macho import metadata, address_bytes
from abi_profile import load
ROOT = Path(__file__).resolve().parent.parent


def symbols(binary):
    result = subprocess.check_output(['nm', '-arch', 'x86_64', '-n', str(binary)], text=True, timeout=30)
    output = {}
    for line in result.splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                output[parts[2]] = int(parts[0], 16)
            except ValueError:
                pass
    return output


def verify(binary, policy):
    data = binary.read_bytes()
    image = metadata(data)
    if image['uuid'] != policy['uuid'] or hashlib.sha256(data).hexdigest() != policy['sha256']:
        raise ValueError(f'{binary.name}: binary differs from reviewed image')
    exported = symbols(binary)
    for item in policy['evidence']:
        address = exported.get(item['symbol'])
        if address is None:
            raise ValueError('missing evidence symbol: '+item['symbol'])
        expected = bytes.fromhex(item['bytes'])
        if address_bytes(data, address+item['offset'], len(expected)) != expected:
            raise ValueError('instruction evidence differs: '+item['symbol'])
    return len(policy['evidence'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=Path('/System/Library/Kernels/kernel'))
    parser.add_argument('--applehv', type=Path, default=Path('/System/Library/Extensions/AppleHV.kext/Contents/MacOS/AppleHV'))
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    policy = load(ROOT/'config/abi.json')
    result = {kind: verify(binary, policy[kind]) for kind, binary in [('kernel', args.kernel), ('applehv', args.applehv)]}
    for kind, count in result.items():
        print(f'[OK] {kind}: exact binary and {count} reviewed instruction spans')
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({'schema': 1, 'verified_spans': result, 'hardware_execution': False}, indent=2)+'\n')
    print('Static instruction contracts only; no kernel call, CPU write or guest execution performed.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        raise SystemExit(str(exc))
