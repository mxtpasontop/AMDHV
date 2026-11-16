#!/usr/bin/env python3
"""Embed manually reviewed instruction spans for read-only runtime matching."""
import json
import os
from pathlib import Path
import sys
import tempfile
from abi_profile import load, validate
ROOT = Path(__file__).resolve().parent.parent


def render(profile):
    validate(profile)
    lines = ['#pragma once', '#include <stddef.h>', '#include <stdint.h>', 'namespace ABIProfile {',
             'struct Evidence { const char *symbol; size_t offset; const uint8_t *bytes; size_t size; bool kernel; };']
    entries = []
    for kind in ('kernel', 'applehv'):
        for index, item in enumerate(profile[kind]['evidence']):
            data = bytes.fromhex(item['bytes'])
            name = f'{kind}{index}'
            lines.append('constexpr uint8_t '+name+'[] = {'+','.join(map(str, data))+'};')
            entries.append('{'+json.dumps(item['symbol'])+','+str(item['offset'])+','+name+',sizeof('+name+'),'+str(kind == 'kernel').lower()+'}')
    lines += ['constexpr Evidence evidence[] = {', ',\n'.join(entries), '};', '}']
    return '\n'.join(lines)+'\n'


def main():
    content = render(load(ROOT/'config/abi.json'))
    out = Path(sys.argv[1]); out.parent.mkdir(parents=True, exist_ok=True)
    if not out.exists() or out.read_text() != content:
        with tempfile.NamedTemporaryFile(mode='w', dir=out.parent, prefix='.abi-', delete=False) as handle:
            handle.write(content); temporary = Path(handle.name)
        try:
            os.replace(temporary, out)
        finally:
            temporary.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
