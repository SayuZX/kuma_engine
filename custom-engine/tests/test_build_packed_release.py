import sys
import tempfile
import unittest
from pathlib import Path
from zipfile import ZipFile


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from build_packed_release import extract_matching_assets


class ExtractAssetsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def _apk(self, name, content):
        path = self.root / name
        with ZipFile(path, "w") as archive:
            archive.writestr("assets/flutter_assets/FontManifest.json", content)
            archive.writestr("lib/arm64-v8a/libflutter.so", b"engine")
        return path

    def test_extracts_the_same_assets_for_both_abis(self):
        arm64 = self._apk("arm64.apk", b"font manifest")
        armv7 = self._apk("armv7.apk", b"font manifest")
        output = self.root / "assets"
        names = extract_matching_assets([arm64, armv7], output)
        self.assertEqual(names, ["FontManifest.json"])
        self.assertEqual((output / "FontManifest.json").read_bytes(), b"font manifest")

    def test_rejects_asset_drift_between_abis(self):
        arm64 = self._apk("arm64.apk", b"first")
        armv7 = self._apk("armv7.apk", b"different")
        with self.assertRaisesRegex(ValueError, "different Flutter asset"):
            extract_matching_assets([arm64, armv7], self.root / "assets")


if __name__ == "__main__":
    unittest.main()
