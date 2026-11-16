import unittest
import sys
import shutil
import subprocess
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from compiler_warnings import strict_flags


class CompilerWarningTests(unittest.TestCase):
    def test_restore_each_driver_exception_once(self):
        output='Clang version\n "/tool/clang" "-cc1" "-Wno-error=shadow" "-Wno-error=sign-conversion" "-Wno-error=shadow"\n'
        self.assertEqual(strict_flags(output),['-Xclang','-Werror=shadow','-Xclang','-Werror=sign-conversion'])
        self.assertEqual(strict_flags('"clang" "-cc1" "-Werror"'),[])
        self.assertEqual(strict_flags('"clang" "-cc1" "-Wno-error"'),['-Xclang','-Werror'])

    def test_invalid_or_ambiguous_driver_output(self):
        for output in ('', '"clang" "-cc1"\n"clang" "-cc1"', '"clang" "-cc1" "-Wno-error=bad group"', '"clang" "-cc1" "-Wno-error="'):
            with self.subTest(output=output),self.assertRaises(ValueError):
                strict_flags(output)

    @unittest.skipUnless(sys.platform=='darwin' and shutil.which('xcrun'),'requires Apple Clang')
    def test_injected_signedness_warning_is_an_error(self):
        compiler=['xcrun','clang++','-target','x86_64-apple-macos12.0','-mkernel']
        driver=subprocess.run(compiler+['-###','-x','c++','-c','/dev/null'],capture_output=True,text=True,check=True,timeout=15)
        result=subprocess.run(compiler+['-Wall','-Wextra','-Wsign-conversion','-Werror',*strict_flags(driver.stderr),'-fsyntax-only','-x','c++','-'],
                              input='unsigned convert(int value) { return value; }\n',capture_output=True,text=True,timeout=15)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('error:',result.stderr)
        self.assertIn('-Wsign-conversion',result.stderr)
