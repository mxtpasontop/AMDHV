import json
import os
from pathlib import Path
import plistlib
import struct
import sys
import tempfile
import unittest
from unittest import mock
import uuid
import copy

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT/'tools'))
import diagnose
import macho
import package_kext
import records
import abi_profile
import abi_header
import verify_abi
import artifact


class MachOTests(unittest.TestCase):
    def image(self, commands=None):
        command = struct.pack('<II', 0x1b, 24)+uuid.UUID(int=1).bytes
        commands = [command] if commands is None else commands
        payload = b''.join(commands)
        return struct.pack('<8I', 0xfeedfacf, 0x01000007, 3, 11, len(commands), len(payload), 0, 0)+payload

    def test_valid(self):
        self.assertEqual(macho.metadata(self.image())['uuid'], str(uuid.UUID(int=1)).upper())

    def test_all_truncations(self):
        image = self.image()
        for n in range(len(image)):
            with self.subTest(n=n), self.assertRaises(ValueError):
                macho.metadata(image[:n])

    def test_duplicate_missing_and_oversized_uuid(self):
        command = self.image()[32:]
        for commands in ([], [command, command], [struct.pack('<II', 0x1b, 32)+b'x'*24]):
            with self.subTest(commands=commands), self.assertRaises(ValueError):
                macho.metadata(self.image(commands))

    def test_wrong_architecture(self):
        image = bytearray(self.image())
        struct.pack_into('<I', image, 4, 0x0100000c)
        with self.assertRaises(ValueError):
            macho.metadata(image)

    def segment_image(self, **changes):
        fields = dict(vmaddr=0x1000, vmsize=4096, fileoff=128, filesize=16, sections=0)
        fields.update(changes)
        segment = struct.pack('<II16s4QiiII', 0x19, 72, b'__TEXT', fields['vmaddr'], fields['vmsize'],
                              fields['fileoff'], fields['filesize'], 5, 5, fields['sections'], 0)
        return self.image([self.image()[32:], segment])+bytes(range(16))

    def test_file_backed_virtual_ranges(self):
        image = self.segment_image()
        self.assertEqual(macho.address_bytes(image, 0x1004, 4), bytes(range(4, 8)))
        for address, size in ((0xfff, 1), (0x1010, 1), (0x100f, 2), (0x1000, 0), (-1, 1), ((1 << 64)-1, 2)):
            with self.subTest(address=address, size=size), self.assertRaises(ValueError):
                macho.address_bytes(image, address, size)

    def test_invalid_segment_extents(self):
        for changes in ({'fileoff': 10000}, {'filesize': 4097}, {'filesize': 17}, {'sections': 1},
                        {'vmaddr': (1 << 64)-4095}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                macho.address_bytes(self.segment_image(**changes), 0x1000, 1)

    def test_file_only_metadata_is_not_mapped(self):
        with self.assertRaises(ValueError):
            macho.address_bytes(self.segment_image(vmsize=0), 0x1000, 1)

    def test_ambiguous_segments(self):
        image = self.segment_image()
        segment = image[56:128]
        ambiguous = self.image([self.image()[32:], segment, segment])+bytes(range(16))
        with self.assertRaises(ValueError):
            macho.address_bytes(ambiguous, 0x1000, 1)

    def universal(self, second_cpu=0x0100000c, second_offset=0x200):
        thin=self.image()
        return (struct.pack('>II',0xcafebabe,2)+
                struct.pack('>5I',0x01000007,3,0x100,len(thin),8)+
                struct.pack('>5I',second_cpu,0,second_offset,len(thin),8)).ljust(0x100,b'\0')+thin+b'\0'*(0x200-0x100-len(thin))+thin

    def test_universal_x86_selection(self):
        self.assertEqual(macho.x86_slice(self.universal()),self.image())
        self.assertEqual(macho.x86_slice(self.image()),self.image())
        for data in (self.universal(second_cpu=0x01000007),self.universal(second_offset=0x100),self.universal()[:0x220],b'bad'):
            with self.subTest(data=data[:12]),self.assertRaises(ValueError):
                macho.x86_slice(data)
        missing=bytearray(self.universal());struct.pack_into('>I',missing,8,0x0100000c)
        with self.assertRaises(ValueError):
            macho.x86_slice(missing)

    def test_diagnostic_entry_checks_machine_code(self):
        image=self.segment_image()
        artifact.verify_diagnostic_entry(image[:128]+bytes.fromhex('b8ffffffffc3')+image[134:],0x1000)
        for machine in ('0f01d8c39090','e900000000c3','b800000000c3'):
            with self.subTest(machine=machine),self.assertRaises(ValueError):
                artifact.verify_diagnostic_entry(image[:128]+bytes.fromhex(machine)+image[134:],0x1000)
        with self.assertRaises(ValueError):
            artifact.verify_diagnostic_entry(image,None)


class ABIProfileTests(unittest.TestCase):
    def setUp(self):
        self.profile = abi_profile.load(ROOT/'config/abi.json')

    def test_profile_and_generated_header(self):
        header = abi_header.render(self.profile)
        for kind in ('kernel', 'applehv'):
            for evidence in self.profile[kind]['evidence']:
                self.assertIn(evidence['symbol'], header)

    def test_bad_evidence_is_never_accepted(self):
        for field, value in (('offset', -1), ('offset', True), ('offset', 1 << 24), ('bytes', ''), ('bytes', 'zz'),
                             ('bytes', '90'*65), ('symbol', 'bad"symbol'), ('contract', '')):
            profile = copy.deepcopy(self.profile)
            profile['kernel']['evidence'][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                abi_header.render(profile)
        for field, value in (('uuid', 'bad'), ('sha256', 'f'*63), ('evidence', [])):
            profile = copy.deepcopy(self.profile); profile['applehv'][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                abi_profile.validate(profile)
        profile = copy.deepcopy(self.profile)
        profile['kernel']['evidence'].append(profile['kernel']['evidence'][0])
        with self.assertRaises(ValueError):
            abi_profile.validate(profile)

    def test_binary_identity_and_instruction_mismatch(self):
        helper = MachOTests()
        data = helper.segment_image()
        import hashlib
        policy = {'uuid': macho.metadata(data)['uuid'], 'sha256': hashlib.sha256(data).hexdigest(),
                  'evidence': [{'symbol': '_entry', 'offset': 4, 'bytes': '04050607'}]}
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp)/'image'; binary.write_bytes(data)
            with mock.patch('verify_abi.symbols', return_value={'_entry': 0x1000}):
                self.assertEqual(verify_abi.verify(binary, policy), 1)
                for field, value in (('uuid', str(uuid.uuid4()).upper()), ('sha256', 'a'*64)):
                    with self.subTest(field=field), self.assertRaises(ValueError):
                        verify_abi.verify(binary, {**policy, field: value})
                mismatch = copy.deepcopy(policy); mismatch['evidence'][0]['bytes'] = '90909090'
                with self.assertRaises(ValueError):
                    verify_abi.verify(binary, mismatch)
            with mock.patch('verify_abi.symbols', return_value={}):
                with self.assertRaises(ValueError):
                    verify_abi.verify(binary, policy)


class DiagnosticTests(unittest.TestCase):
    identity = 'a'*64
    def test_mode_arguments(self):
        self.assertEqual(diagnose.boot_mode('keepsyms=1 amdhvstage=1'), 'preflight')
        self.assertEqual(diagnose.boot_mode('amdhvmode=preflight amdhvstage=1'), 'preflight')
        self.assertEqual(diagnose.boot_mode(''), 'bootstrap')
        for args in ('amdhvstage=01', 'amdhvmode=full amdhvstage=1', 'amdhvstage=1 amdhvstage=1',
                     'amdhvmode=oops', '-amdhvoff', 'amdhvmode=bootstrap amdhvmode=preflight'):
            with self.subTest(args=args), self.assertRaises(ValueError):
                diagnose.boot_mode(args)

    def test_exact_stage_and_build(self):
        service = {'AMDHVBuild': self.identity, 'AMDHVState': 'diagnostic-applehv-preflight'}
        self.assertEqual(diagnose.evaluate('66 0 com.amdhv.AMDHV (1.0.0)', '0', service, self.identity, 'preflight'), [])
        for state in ('diagnostic-bootstrap', 'diagnostic-xnu-preflight', 'active', 'registration-failed'):
            wrong = {**service, 'AMDHVState': state}
            self.assertTrue(diagnose.evaluate('com.amdhv.AMDHV (1.0.0)', '0', wrong, self.identity, 'preflight'))
        self.assertTrue(diagnose.evaluate('com.amdhv.AMDHV.old', '0', service, self.identity, 'preflight'))
        self.assertTrue(diagnose.evaluate('com.amdhv.AMDHV', '1', service, self.identity, 'preflight'))
        self.assertTrue(diagnose.evaluate('com.amdhv.AMDHV', '0', service, 'b'*64, 'preflight'))

    def test_uuid_must_belong_to_amdhv(self):
        identity = str(uuid.uuid4())
        loaded = 'com.amdhv.AMDHV OTHER-UUID\nother.driver '+identity
        service = {'AMDHVBuild': self.identity, 'AMDHVState': 'diagnostic-bootstrap'}
        self.assertTrue(diagnose.evaluate(loaded, '0', service, self.identity, 'bootstrap', identity))

    def test_registry_cardinality(self):
        service = {'AMDHVBuild': self.identity}
        self.assertEqual(diagnose.registry_service(plistlib.dumps([service])), service)
        for entries in ([], [service, service], {}):
            with self.assertRaises(ValueError):
                diagnose.registry_service(plistlib.dumps(entries))

    def test_probe_failures_preserved(self):
        with mock.patch('diagnose.subprocess.Popen', side_effect=FileNotFoundError('missing')):
            self.assertEqual(diagnose.probe('/missing')['returncode'], -1)

    def test_bounded_probe_capture(self):
        result = diagnose.probe(sys.executable, '-c', 'import sys; print("hello"); print("problem", file=sys.stderr); sys.exit(3)')
        self.assertEqual(result['returncode'], 3)
        self.assertIn('hello', result['stdout'])
        self.assertIn('problem', result['stderr'])
        oversized = diagnose.probe(sys.executable, '-c', 'import sys; sys.stdout.write("x"*5000000)')
        self.assertEqual(oversized['returncode'], -1)
        self.assertIn('output limit', oversized['stderr'])

    def test_snapshots_never_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            record = {'boot_uuid': str(uuid.uuid4()), 'errors': ['example failure']}
            a = diagnose.save_snapshot(Path(tmp), record)
            b = diagnose.save_snapshot(Path(tmp), record)
            self.assertNotEqual(a, b)
            self.assertEqual(json.loads(Path(a).read_text()), record)
            self.assertEqual(Path(a).stat().st_mode & 0o777, 0o600)


class RetentionTests(unittest.TestCase):
    def test_only_owned_regular_records_are_selected(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            data = {'schema': 1, 'kind': 'amdhv-boot-diagnostic'}
            for i in range(3):
                path = directory/f'amdhv-{uuid.uuid4()}-20261116T000000-{i}.json'
                path.write_text(json.dumps(data))
            foreign = directory/'amdhv-unknown-20261116T000000-foreign.json'
            foreign.write_text('{}')
            (directory/'amdhv-unknown-20261116T000000-link.json').symlink_to(foreign)
            selected = records.candidates(directory, 1)
            self.assertEqual(len(selected), 2)
            self.assertNotIn(foreign, selected)
            self.assertFalse(any(path.is_symlink() for path in selected))
            with self.assertRaises(ValueError):
                records.candidates(directory, 0)


class PublicationTests(unittest.TestCase):
    def test_publish_and_exchange(self):
        with tempfile.TemporaryDirectory() as tmp:
            staged, target = Path(tmp)/'stage', Path(tmp)/'bundle'
            staged.mkdir()
            (staged/'value').write_text('first')
            package_kext.publish(staged, target)
            self.assertEqual((target/'value').read_text(), 'first')
            staged.mkdir()
            (staged/'value').write_text('second')
            if sys.platform == 'darwin':
                package_kext.publish(staged, target)
                self.assertEqual((target/'value').read_text(), 'second')
                self.assertEqual((staged/'value').read_text(), 'first')
            else:
                with self.assertRaises(RuntimeError):
                    package_kext.publish(staged, target)
                self.assertEqual((target/'value').read_text(), 'first')

    def test_symlink_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            staged, target = Path(tmp)/'stage', Path(tmp)/'bundle'
            staged.mkdir()
            target.symlink_to(staged, target_is_directory=True)
            with self.assertRaises(ValueError):
                package_kext.publish(staged, target)
            self.assertTrue(staged.is_dir())


if __name__ == '__main__':
    unittest.main()
