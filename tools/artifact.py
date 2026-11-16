"""Check imports and produce reproducible source/toolchain/artifact provenance."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
from macho import metadata, address_bytes
from abi_profile import load
from build_identity import identity

ROOT = Path(__file__).resolve().parent.parent


def verify_diagnostic_entry(data,address):
    if address is None or address_bytes(data,address,6)!=bytes.fromhex('b8ffffffffc3'):
        raise ValueError('assembled SVM entry is not the reviewed preserving failure stub')


def command(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT, timeout=30).strip()


def inspect(binary):
    data = Path(binary).read_bytes()
    image = metadata(data)
    if image['filetype'] != 11:
        raise ValueError('artifact is not MH_KEXT_BUNDLE')
    policy = json.loads((ROOT/'config/imports.json').read_text())
    compatibility = json.loads((ROOT/'config/compatibility.json').read_text())
    abi=load(ROOT/'config/abi.json')
    for kind in ('kernel','applehv'):
        if abi[kind]['uuid']!=compatibility[kind+'_uuid']:
            raise ValueError('ABI evidence targets a different '+kind+' UUID')
    if policy['kernel_uuid'] != compatibility['kernel_uuid']:
        raise ValueError('import approval targets a different kernel UUID')
    imports = set(command('nm', '-uj', str(binary)).splitlines())
    allowed = set(policy['kernel']) | set(policy['lilu'])
    if imports-allowed:
        raise ValueError('unreviewed imports: ' + ', '.join(sorted(imports-allowed)))
    source_id = re.search(r'AMDHV_BUILD_ID "([0-9a-f]{64})"', (ROOT/'build/BuildInfo.hpp').read_text())
    if not source_id or source_id.group(1).encode() not in data:
        raise ValueError('embedded source identity is missing or stale')
    if source_id.group(1)!=identity(ROOT):
        raise ValueError('source/tests/configuration changed since the build identity was generated')
    # The assembled entry point must be a preserving failure stub until recovery
    # exists; reject any accidentally reintroduced privileged SVM opcode.
    assembly = (ROOT/'src/SVMEntry.S').read_text()
    if re.search(r'^\s*(?:clgi|stgi|vmrun|vmload|vmsave)\s', assembly, re.M):
        raise ValueError('unverified privileged SVM transition in source')
    addresses={}
    for line in command('nm','-arch','x86_64','-n',str(binary)).splitlines():
        parts=line.split()
        if len(parts)==3:
            try:
                addresses[parts[2]]=int(parts[0],16)
            except ValueError:
                pass
    entry=addresses.get('_amd_svm_vcpu_run')
    verify_diagnostic_entry(data,entry)
    return {
        'schema': 1, 'source_id': source_id.group(1),
        'sha256': hashlib.sha256(data).hexdigest(), **image,
        'dependencies': json.loads((ROOT/'dependencies.lock.json').read_text()),
        'compatibility': compatibility,
        'reviewed_abi':abi,
        'imports': sorted(imports), 'lilu_binary_verified': policy['lilu_binary_verified'],
        'compiler': command('xcrun', 'clang++', '--version'),
        'sdk': command('xcrun', '--sdk', 'macosx', '--show-sdk-version'),
        'execution_enabled': False,
        'experimental_transition_compiled':True,
    }
