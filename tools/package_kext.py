#!/usr/bin/env python3
"""Validate, stage and atomically publish a complete diagnostic bundle."""
import ctypes
import json
import os
from pathlib import Path
import plistlib
import shutil
import sys
import tempfile
from artifact import inspect


def publish(staged, target):
    if target.is_symlink():
        raise ValueError('refusing a symlink bundle destination')
    if not target.exists():
        os.replace(staged, target)
        return
    if sys.platform != 'darwin':
        raise RuntimeError('atomic directory exchange requires macOS')
    # Atomic exchange prevents a missing published path on process interruption. The old bundle becomes staged and is cleaned up.
    libc = ctypes.CDLL(None, use_errno=True)
    exchange = libc.renameatx_np
    exchange.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    exchange.restype = ctypes.c_int
    if exchange(-2, os.fsencode(staged), -2, os.fsencode(target), 2):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))


def main():
    if len(sys.argv) != 4:
        raise SystemExit('usage: package_kext.py BINARY INFO_PLIST TARGET')
    binary, info, target = map(Path, sys.argv[1:])
    info_bytes=info.read_bytes()
    metadata = plistlib.loads(info_bytes)
    if metadata.get('CFBundleExecutable') != 'AMDHV' or metadata.get('CFBundleIdentifier') != 'com.amdhv.AMDHV':
        raise ValueError('unexpected bundle metadata')
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=target.parent, prefix='.amdhv-package-') as tmp:
        staged = Path(tmp)/'AMDHV.kext'
        executable = staged/'Contents/MacOS/AMDHV'
        executable.parent.mkdir(parents=True)
        shutil.copy2(binary, executable)
        executable.chmod(0o755)
        # Validate the private staged copy, then publish these exact bytes. A
        # concurrent link cannot swap the inspected input before we copy it.
        manifest=inspect(executable)
        (staged/'Contents/Info.plist').write_bytes(info_bytes)
        resources = staged/'Contents/Resources'
        resources.mkdir()
        (resources/'BuildManifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        publish(staged, target)
    print(f"Diagnostic bundle {manifest['source_id'][:12]} packaged; execution is blocked.")


if __name__ == '__main__':
    main()
