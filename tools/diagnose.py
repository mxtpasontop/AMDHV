#!/usr/bin/env python3
"""Read-only verification of this boot's exact AMDHV mode and source identity."""
import argparse
import hashlib
from datetime import datetime, timezone
import json
import os
import selectors
import signal
import time
from pathlib import Path
import plistlib
import re
import shlex
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parent.parent
IDENTITY = re.compile(r'[0-9a-f]{64}\Z')


def probe(*args):
    output = {'stdout': bytearray(), 'stderr': bytearray()}
    process, failure = None, None
    try:
        process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ, 'stdout')
            selector.register(process.stderr, selectors.EVENT_READ, 'stderr')
            deadline = time.monotonic()+20
            while selector.get_map():
                remaining = deadline-time.monotonic()
                if remaining <= 0:
                    failure = 'probe exceeded 20-second deadline'
                    break
                for key, _ in selector.select(remaining):
                    chunk = os.read(key.fd, 65536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    output[key.data].extend(chunk)
                    if sum(map(len, output.values())) > 4*1024*1024:
                        failure = 'probe exceeded 4 MiB output limit'
                        break
                if failure:
                    break
        if not failure:
            try:
                process.wait(timeout=max(0.001, deadline-time.monotonic()))
            except subprocess.TimeoutExpired:
                failure = 'probe did not exit before deadline'
        result = {key: value.decode(errors='replace') for key, value in output.items()}
        if failure:
            result['stderr'] += '\n'+failure
        return {'returncode': -1 if failure else process.returncode, **result}
    except OSError as exc:
        return {'returncode': -1, 'stdout': '', 'stderr': str(exc)}
    finally:
        # Also cancel descendants when interrupted while reading or selecting.
        # Reaping is bounded even if a kernel-side probe cannot exit immediately.
        if process is not None:
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except OSError:
                    pass
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass
            process.stdout.close()
            process.stderr.close()


def boot_mode(arguments):
    names, stages = [], []
    for arg in shlex.split(arguments):
        if arg.startswith('amdhvmode='):
            names.append(arg.split('=', 1)[1])
        if arg.startswith('amdhvstage='):
            value = arg.split('=', 1)[1]
            if value not in ('0', '1', '2', '3'):
                raise ValueError('invalid boot stage')
            stages.append({'0': 'bootstrap', '1': 'preflight', '2': 'full', '3': 'trap'}[value])
        if arg == '-amdhvoff':
            raise ValueError('AMDHV disabled by boot argument')
    if len(names) > 1 or len(stages) > 1:
        raise ValueError('duplicate mode/stage boot arguments')
    if names and stages and names != stages:
        raise ValueError('conflicting mode/stage boot arguments')
    mode = (names or stages or ['bootstrap'])[0]
    if mode not in ('bootstrap', 'preflight', 'trap', 'full'):
        raise ValueError('unrecognized boot mode')
    return mode


def registry_service(data):
    entries = plistlib.loads(data)
    found = []
    def visit(nodes):
        if not isinstance(nodes, list):
            raise ValueError('invalid registry root')
        for node in nodes:
            if not isinstance(node, dict):
                raise ValueError('invalid registry entry')
            if any(key.startswith('AMDHV') for key in node):
                found.append(node)
            visit(node.get('IORegistryEntryChildren', []))
    visit(entries)
    if len(found) != 1:
        raise ValueError(f'expected one AMDHV service, got {len(found)}')
    return {key: value for key, value in found[0].items() if key.startswith('AMDHV')}


def evaluate(loaded, support, service, expected_build, expected_mode, expected_uuid=None):
    errors = []
    lines = [line for line in loaded.splitlines() if re.search(r'(?<!\S)com\.amdhv\.AMDHV(?=\s|\()', line)]
    if len(lines) != 1:
        errors.append('expected exactly one loaded AMDHV extension')
    if expected_uuid and (len(lines) != 1 or expected_uuid.lower() not in lines[0].lower()):
        errors.append('loaded executable UUID differs from the expected artifact')
    if service.get('AMDHVBuild') != expected_build:
        errors.append('loaded build differs from the expected build')
    expected_state = {'bootstrap': 'diagnostic-bootstrap', 'preflight': 'diagnostic-applehv-preflight'}.get(expected_mode)
    if not expected_state or service.get('AMDHVState') != expected_state:
        errors.append(f'incomplete or unexpected state for mode {expected_mode}')
    if support.strip() != '0':
        errors.append('diagnostic mode must leave kern.hv_support at zero')
    return errors


def save_snapshot(directory, snapshot):
    directory.mkdir(parents=True, exist_ok=True)
    # mkstemp is exclusive, mode 0600, and never overwrites an earlier boot.
    prefix = f"amdhv-{snapshot['boot_uuid']}-{datetime.now(timezone.utc):%Y%m%dT%H%M%S}-"
    with tempfile.NamedTemporaryFile(mode='w', dir=directory, prefix=prefix, suffix='.json', delete=False) as handle:
        json.dump(snapshot, handle, indent=2)
        handle.write('\n')
        return handle.name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--diagnose-only', action='store_true')
    parser.add_argument('--expect-mode', choices=('bootstrap', 'preflight'))
    parser.add_argument('--expect-build')
    parser.add_argument('--expect-uuid', type=lambda value: str(uuid.UUID(value)))
    parser.add_argument('--output-dir', type=Path, default=ROOT/'diagnostics')
    args = parser.parse_args()
    expected = args.expect_build
    expected_uuid = args.expect_uuid
    if expected is None:
        manifest = ROOT/'build/AMDHV.kext/Contents/Resources/BuildManifest.json'
        try:
            record = json.loads(manifest.read_text())
            expected = record['source_id']
            expected_uuid = expected_uuid or str(uuid.UUID(record['uuid']))
            binary = manifest.parent.parent/'MacOS/AMDHV'
            if hashlib.sha256(binary.read_bytes()).hexdigest() != record['sha256']:
                parser.error('local executable differs from its build manifest')
        except (OSError, ValueError, KeyError) as exc:
            parser.error(f'provide --expect-build or build the local diagnostic bundle: {exc}')
    if not isinstance(expected, str) or not IDENTITY.fullmatch(expected):
        parser.error('--expect-build must be the 64-character lowercase source identity')
    commands = {
        'loaded': ('/usr/bin/kmutil', 'showloaded'),
        'support': ('/usr/sbin/sysctl', '-n', 'kern.hv_support'),
        'boot_args': ('/usr/sbin/sysctl', '-n', 'kern.bootargs'),
        'boot_uuid': ('/usr/sbin/sysctl', '-n', 'kern.bootsessionuuid'),
        'os_build': ('/usr/sbin/sysctl', '-n', 'kern.osversion'),
        'registry': ('/usr/sbin/ioreg', '-a', '-r', '-c', 'AMDHV', '-l'),
        'logs': ('/usr/bin/log', 'show', '--last', 'boot', '--style', 'compact', '--predicate',
                 'eventMessage CONTAINS "AMDHV"'),
    }
    results = {name: probe(*command) for name, command in commands.items()}
    errors = [f'{name} probe failed: {result["stderr"].strip()}' for name, result in results.items()
              if name != 'logs' and result['returncode']]
    service, mode, boot_uuid = {}, args.expect_mode, 'unknown'
    try:
        boot_uuid = str(uuid.UUID(results['boot_uuid']['stdout'].strip()))
        actual_mode = boot_mode(results['boot_args']['stdout'])
        if mode and actual_mode != mode:
            errors.append(f'boot mode {actual_mode} differs from expected mode {mode}')
        mode = mode or actual_mode
        service = registry_service(results['registry']['stdout'].encode())
        errors += evaluate(results['loaded']['stdout'], results['support']['stdout'], service, expected, mode, expected_uuid)
    except (ValueError, TypeError, plistlib.InvalidFileException) as exc:
        errors.append(str(exc))
    snapshot = {'schema': 1, 'kind': 'amdhv-boot-diagnostic', 'boot_uuid': boot_uuid,
                'captured_utc': datetime.now(timezone.utc).isoformat(), 'expected_build': expected,
                'expected_mode': mode, 'expected_uuid': expected_uuid, 'service': service, 'errors': errors, 'probes': results}
    destination = save_snapshot(args.output_dir, snapshot)
    print(f"AMDHV build: {service.get('AMDHVBuild', 'unavailable')}")
    print(f"State: {service.get('AMDHVState', 'unavailable')} | mode: {mode} | boot: {boot_uuid}")
    print(f'Diagnostic record: {destination}')
    for error in errors:
        print(f'[FAIL] {error}')
    if results['logs']['returncode']:
        print('[INFO] Current-boot log collection failed; see the record for details.')
    if errors:
        return 1
    print('[OK] Exact diagnostic mode and build verified. Guest execution is inactive.')
    if not args.diagnose_only:
        print('[SKIP] This diagnostic build has no accepted guest execution backend.')
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
