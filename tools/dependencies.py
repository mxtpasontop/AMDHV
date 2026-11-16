#!/usr/bin/env python3
"""Fetch exact locked dependencies, or verify existing checkouts without network."""
import argparse
import os
import tempfile
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent

def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True, stderr=subprocess.STDOUT, timeout=60).strip()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true')
    args = parser.parse_args()
    for name, dep in json.loads((ROOT/'dependencies.lock.json').read_text()).items():
        path = ROOT/'vendor'/name
        if not path.exists():
            if not args.fetch:
                raise RuntimeError(f'{name} missing; run python3 tools/dependencies.py --fetch')
            path.parent.mkdir(parents=True,exist_ok=True)
            with tempfile.TemporaryDirectory(dir=path.parent,prefix='.fetch-'+name+'-') as tmp:
                checkout=Path(tmp)/name
                checkout.mkdir()
                run('git','init','-q',str(checkout))
                run('git','remote','add','origin',dep['url'],cwd=checkout)
                run('git','fetch','--depth=1','origin',dep['commit'],cwd=checkout)
                run('git','checkout','--detach','FETCH_HEAD',cwd=checkout)
                os.rename(checkout,path)
        if path.is_symlink() or not (path/'.git').is_dir():
            raise RuntimeError(f'{name}: expected an isolated dependency checkout')
        if run('git','rev-parse','HEAD',cwd=path) != dep['commit']:
            raise RuntimeError(f'{name}: wrong revision')
        if run('git','status','--porcelain','--untracked-files=all','--ignored=matching',cwd=path):
            raise RuntimeError(f'{name}: modified dependency source')
        print(f'{name}: {dep["commit"]}')

if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        raise SystemExit(str(exc))
