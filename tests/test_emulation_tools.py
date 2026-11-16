from pathlib import Path
import struct
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'tools'))
import build_identity
import clean
import emulate_transition


class EmulatedImageTests(unittest.TestCase):
    def object(self,kind=10,position=0,addend=8,duplicate=False,undefined=False):
        names=b'\0.shstrtab\0.text\0.symtab\0.rela.text\0'
        image=bytearray(64)
        def payload(data):
            image.extend(b'\0'*((-len(image))%8));offset=len(image);image.extend(data);return offset,len(data)
        names_extent=payload(names);text_extent=payload(b'\0'*8)
        symbol=struct.pack('<IBBHQQ',0,3,0,0 if undefined else 2,0,0)
        sym_extent=payload(b'\0'*24+symbol)
        reloc=struct.pack('<QQq',position,(1<<32)|kind,addend)
        reloc_extent=payload(reloc*(2 if duplicate else 1))
        image.extend(b'\0'*((-len(image))%8));section_offset=len(image)
        sections=[(0,0,0,0,0,0,0,0,0,0),
                  (names.index(b'.shstrtab'),3,0,0,*names_extent,0,0,1,0),
                  (names.index(b'.text'),1,6,0,*text_extent,0,0,8,0),
                  (names.index(b'.symtab'),2,0,0,*sym_extent,1,2,8,24),
                  (names.index(b'.rela.text'),4,0,0,*reloc_extent,3,2,8,24)]
        for section in sections:
            image.extend(struct.pack('<IIQQQQIIQQ',*section))
        header=struct.pack('<16sHHIQQQIHHHHHH',b'\x7fELF\x02\x01\x01'+b'\0'*9,1,62,1,0,0,section_offset,0,64,0,0,64,len(sections),1)
        image[:64]=header
        return bytes(image)

    def test_absolute_and_relative_relocation(self):
        self.assertEqual(emulate_transition.image_from_object(self.object())[:4],struct.pack('<I',0x100008))
        self.assertEqual(emulate_transition.image_from_object(self.object(kind=2))[:4],struct.pack('<i',8))

    def test_every_truncation(self):
        image=self.object()
        for length in range(len(image)):
            with self.subTest(length=length),self.assertRaises(ValueError):
                emulate_transition.image_from_object(image[:length])

    def test_relocation_bounds_and_ownership(self):
        for args in ({'kind':100},{'position':6},{'addend':1<<40},{'duplicate':True},{'undefined':True}):
            with self.subTest(args=args),self.assertRaises(ValueError):
                emulate_transition.image_from_object(self.object(**args))


class BuildEvidenceTests(unittest.TestCase):
    def test_identity_includes_nested_tests_and_ignores_generated_caches(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);test=root/'tests/emulation/test.S';test.parent.mkdir(parents=True);test.write_text('first')
            first=build_identity.identity(root);test.write_text('second');self.assertNotEqual(first,build_identity.identity(root))
            second=build_identity.identity(root)
            cache=test.parent/'__pycache__';cache.mkdir();(cache/'test.pyc').write_bytes(b'ignored')
            (root/'docs').mkdir();(root/'docs/VALIDATION.md').write_text('identity is recorded here')
            self.assertEqual(second,build_identity.identity(root))

    def test_clean_preserves_evidence_and_refuses_unsafe_roots(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);build=root/'build';build.mkdir()
            for name in ('checkpoints','research','emulation','ndebug'):
                directory=build/name;directory.mkdir();(directory/'data').write_text(name)
            (build/'artifact.o').write_bytes(b'generated');(build/'link').symlink_to(root)
            clean.clean(build,root)
            for name in clean.PRESERVED:
                self.assertEqual((build/name/'data').read_text(),name)
                with self.assertRaises(ValueError):
                    clean.clean(build/name,root)
            self.assertFalse((build/'ndebug').exists());self.assertFalse((build/'artifact.o').exists());self.assertFalse((build/'link').exists())
            for path in (root,root.parent):
                with self.assertRaises(ValueError):
                    clean.clean(path,root)
            (root/'alias').symlink_to(build,target_is_directory=True)
            with self.assertRaises(ValueError):
                clean.clean(root/'alias',root)


if __name__=='__main__':
    unittest.main()
