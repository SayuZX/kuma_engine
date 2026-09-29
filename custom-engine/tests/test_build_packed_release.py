import sys
import tempfile
import unittest
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from build_packed_release import _size_breakdown, extract_matching_assets


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

    def test_size_report_counts_actual_zip_entries(self):
        apk = self.root / "size.apk"
        with ZipFile(apk, "w") as archive:
            archive.writestr("assets/flutter_assets/data.json", b"x" * 1000,
                             compress_type=ZIP_DEFLATED)
            archive.writestr("assets/dolby/DolbySound.apk", b"d" * 20)
            archive.writestr("lib/arm64-v8a/libflutter.so", b"engine")
        with ZipFile(apk) as archive:
            compressed = archive.getinfo("assets/flutter_assets/data.json").compress_size
        sizes = _size_breakdown(apk, "arm64-v8a")
        self.assertEqual(sizes["flutter_assets_zip_bytes"], compressed)
        self.assertEqual(sizes["dolby_apks_zip_bytes"], 20)
        self.assertEqual(sizes["libflutter_bytes"], 6)
        self.assertEqual(sizes["libpayload_bytes"], 0)


if __name__ == "__main__":
    unittest.main()
