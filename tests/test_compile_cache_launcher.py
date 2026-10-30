import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/ci/compile_cache_launcher.py"


class CompilerCache(unittest.TestCase):
    def case(self, cache_result, compiler_result=0, missing=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "calls.jsonl"
            cache = root / "cache.py"
            compiler = root / "compiler.py"
            cache.write_text("import sys,json,os\nfrom pathlib import Path\n"
                             "with open(os.environ['FIXTURE_LOG'],'a') as f:f.write(json.dumps(['cache',*sys.argv[1:]])+'\\n')\n"
                             f"sys.exit({cache_result})\n", encoding="utf-8")
            compiler.write_text("import sys,json,os\n"
                                "with open(os.environ['FIXTURE_LOG'],'a') as f:f.write(json.dumps(['compiler',*sys.argv[1:]])+'\\n')\n"
                                f"sys.exit({compiler_result})\n", encoding="utf-8")
            flag = root / "build" / ".msime-sccache-disabled"
            argv = [sys.executable, str(SCRIPT), "--cache", str(root / "missing-cache.exe") if missing else sys.executable]
            if not missing:
                argv += ["--cache-argument", str(cache)]
            argv += ["--disable-flag", str(flag), "--", sys.executable, str(compiler), "-DVALUE=hello world", "中文 source.cpp"]
            env = dict(os.environ, FIXTURE_LOG=str(log))
            first = subprocess.run(argv, env=env, capture_output=True, text=True)
            second = subprocess.run(argv, env=env, capture_output=True, text=True)
            calls = [json.loads(x) for x in log.read_text().splitlines()]
            return first, second, calls, flag.exists()

    def test_cache_success_does_not_duplicate_compile(self):
        first, second, calls, disabled = self.case(0)
        self.assertEqual((first.returncode, second.returncode), (0, 0))
        self.assertEqual([x[0] for x in calls], ["cache", "cache"])
        self.assertFalse(disabled)

    def test_service_failure_recovers_and_disables_cache(self):
        first, second, calls, disabled = self.case(9)
        self.assertEqual((first.returncode, second.returncode), (0, 0))
        self.assertEqual([x[0] for x in calls], ["cache", "compiler", "compiler"])
        self.assertTrue(disabled)
        self.assertEqual(calls[-1][1:], ["-DVALUE=hello world", "中文 source.cpp"])

    def test_missing_cache_recovers(self):
        first, second, calls, disabled = self.case(0, missing=True)
        self.assertEqual((first.returncode, second.returncode), (0, 0))
        self.assertEqual([x[0] for x in calls], ["compiler", "compiler"])
        self.assertTrue(disabled)

    def test_real_compiler_failure_is_not_hidden(self):
        first, second, calls, disabled = self.case(3, compiler_result=7)
        self.assertEqual((first.returncode, second.returncode), (7, 7))
        self.assertTrue(disabled)


if __name__ == "__main__":
    unittest.main()
