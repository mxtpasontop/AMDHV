#!/usr/bin/env python3
"""Preview or prune old AMDHV diagnostic records; never touch EFI backups."""
import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
NAME = re.compile(r'amdhv-(?:[0-9a-f-]{36}|unknown)-[0-9]{8}T[0-9]{6}-[a-zA-Z0-9_-]+\.json\Z')


def candidates(directory, keep):
    if keep < 1:
        raise ValueError('keep must be at least one')
    records = []
    for path in directory.glob('amdhv-*.json'):
        if path.is_symlink() or not path.is_file() or not NAME.fullmatch(path.name) or path.stat().st_size > 4*1024*1024:
            continue
        try:
            data = json.loads(path.read_text())
            if data.get('schema') == 1 and data.get('kind') == 'amdhv-boot-diagnostic':
                records.append(path)
        except (OSError, ValueError, AttributeError):
            continue
    return sorted(records, key=lambda p: (p.stat().st_mtime_ns, p.name), reverse=True)[keep:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=ROOT/'diagnostics')
    parser.add_argument('--keep', type=int, default=20)
    parser.add_argument('--apply', action='store_true', help='delete the listed old diagnostic records')
    args = parser.parse_args()
    if args.keep < 1:
        parser.error('--keep must be at least one')
    for path in candidates(args.directory, args.keep):
        print(('DELETE ' if args.apply else 'WOULD DELETE ')+str(path))
        if args.apply:
            path.unlink()


if __name__ == '__main__':
    main()
