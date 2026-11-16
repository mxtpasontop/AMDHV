#!/usr/bin/env python3
"""Remove generated build products while preserving checkpoints and research."""
from pathlib import Path
import shutil
import sys
ROOT=Path(__file__).resolve().parent.parent
PRESERVED={'checkpoints','research','emulation'}


def clean(directory,root=ROOT):
    directory=Path(directory)
    if directory.is_symlink() or (root/'build').is_symlink():
        raise ValueError('refusing a symlink build directory')
    directory=directory.resolve();build=(root/'build').resolve()
    if directory!=build and build not in directory.parents:
        raise ValueError('clean is restricted to this project\'s build directory')
    if any(part in PRESERVED for part in directory.relative_to(build).parts):
        raise ValueError('refusing to clean a preserved evidence/checkpoint directory')
    if not directory.exists():
        return
    for path in directory.iterdir():
        if path.name in PRESERVED:
            continue
        if path.is_symlink() or path.is_file():
            path.unlink()
        elif path.is_dir():
            shutil.rmtree(path)


if __name__=='__main__':
    clean(Path(sys.argv[1]))
