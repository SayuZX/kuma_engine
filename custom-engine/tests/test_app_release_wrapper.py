"""Exercise the app wrapper delivered by the reproducible KumaNime patch."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path


PATCH = Path(__file__).resolve().parents[1] / "app_patch/0001-kumaanime-packed-release.patch"


def _wrapper_from_patch() -> str:
    marker = "+++ b/tool/build_packed_release.sh\n"
    text = PATCH.read_text(encoding="utf-8")
    if marker not in text:
        raise ValueError("app patch does not add the packed release wrapper")
    lines = text.split(marker, 1)[1].splitlines(keepends=True)
    return "".join(line[1:] for line in lines if line.startswith("+") and not line.startswith("+++"))


class AppReleaseWrapperTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / "app"
        self.tool = self.app / "tool"
        self.tool.mkdir(parents=True)
        (self.tool / "build_packed_release.sh").write_text(_wrapper_from_patch())

        scripts = self.root / "sdk/custom-engine/scripts"
        scripts.mkdir(parents=True)
        (scripts / "verify_packed_pair.py").write_text("raise SystemExit(0)\n")
        (scripts / "build_packed_release.py").write_text(
            "from pathlib import Path\n"
            "import sys\n"
            "app = Path(sys.argv[sys.argv.index('--app') + 1])\n"
            "root = app / 'build/app/outputs'\n"
            "for folder in (root / 'flutter-apk', root / 'apk/release'):\n"
            "    folder.mkdir(parents=True, exist_ok=True)\n"
            "    for abi in ('arm64-v8a', 'armeabi-v7a'):\n"
            "        (folder / f'app-{abi}-release.apk').write_bytes(b'stock')\n"
            "    (folder / 'app-release.apk').write_bytes(b'stale')\n"
            "(root / 'packed-release-report.json').write_text('stale')\n"
            "raise SystemExit(1)\n"
        )

    def _run(self):
        env = os.environ.copy()
        env["FLUTTER_ROOT"] = str(self.root / "sdk")
        env["KUMA_SYMBOL_ROOT"] = str(self.root / "symbols")
        return subprocess.run(
            ["bash", str(self.tool / "build_packed_release.sh")],
            env=env, capture_output=True, text=True,
        )

    def test_failed_build_restores_previous_packed_pair(self):
        outputs = self.app / "build/app/outputs"
        flutter = outputs / "flutter-apk"
        flutter.mkdir(parents=True)
        for abi in ("arm64-v8a", "armeabi-v7a"):
            (flutter / f"app-{abi}-release.apk").write_bytes(b"previous-packed")
        (outputs / "packed-release-report.json").write_text("previous-report")

        self.assertEqual(self._run().returncode, 1)
        for abi in ("arm64-v8a", "armeabi-v7a"):
            name = f"app-{abi}-release.apk"
            self.assertEqual((flutter / name).read_bytes(), b"previous-packed")
            self.assertEqual((outputs / "apk/release" / name).read_bytes(), b"previous-packed")
        self.assertEqual((outputs / "packed-release-report.json").read_text(), "previous-report")
        self.assertFalse((flutter / "app-release.apk").exists())

    def test_failed_first_build_removes_stock_outputs(self):
        self.assertEqual(self._run().returncode, 1)
        outputs = self.app / "build/app/outputs"
        for folder in (outputs / "flutter-apk", outputs / "apk/release"):
            self.assertFalse(any(folder.glob("*.apk")))
        self.assertFalse((outputs / "packed-release-report.json").exists())


if __name__ == "__main__":
    unittest.main()
