import sys
import tempfile
import unittest
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile


SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))

from package_release_apk import inspect_archive, rewrite_unsigned_apk


class PackageReleaseApkTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "stock.apk"
        self.rewritten = self.root / "rewritten.apk"
        self.engine = self.root / "libflutter.so"
        self.payload = self.root / "libpayload.so"
        self.engine.write_bytes(b"custom engine")
        self.payload.write_bytes(b"signed packed payload")

    def _stock_apk(self, extra_abi=False):
        with ZipFile(self.source, "w") as archive:
            archive.writestr("AndroidManifest.xml", b"manifest", ZIP_STORED)
            archive.writestr("classes.dex", b"classes", ZIP_DEFLATED)
            archive.writestr("lib/arm64-v8a/libflutter.so", b"stock engine", ZIP_STORED)
            archive.writestr("lib/arm64-v8a/libapp.so", b"app", ZIP_STORED)
            archive.writestr("assets/flutter_assets/FontManifest.json", b"[]", ZIP_DEFLATED)
            archive.writestr("assets/flutter_assets/fonts/Test.ttf", b"font", ZIP_DEFLATED)
            archive.writestr("assets/dolby/DolbySound.apk", b"unused", ZIP_DEFLATED)
            archive.writestr("META-INF/CERT.RSA", b"old signature", ZIP_DEFLATED)
            if extra_abi:
                archive.writestr("lib/armeabi-v7a/libflutter.so", b"other", ZIP_STORED)

    def test_replaces_engine_and_removes_obsolete_assets(self):
        self._stock_apk()
        rewrite_unsigned_apk(
            self.source, self.rewritten, "arm64-v8a", self.engine, self.payload
        )
        with ZipFile(self.rewritten) as archive:
            names = set(archive.namelist())
            self.assertEqual(archive.read("lib/arm64-v8a/libflutter.so"), b"custom engine")
            self.assertEqual(archive.read("lib/arm64-v8a/libpayload.so"), b"signed packed payload")
            self.assertEqual(archive.read("lib/arm64-v8a/libapp.so"), b"app")
            self.assertIn("AndroidManifest.xml", names)
            self.assertNotIn("META-INF/CERT.RSA", names)
            self.assertFalse(any(name.startswith("assets/flutter_assets/") for name in names))
            self.assertFalse(any(name.startswith("assets/dolby/") for name in names))
            self.assertEqual(archive.getinfo("lib/arm64-v8a/libpayload.so").compress_type, ZIP_STORED)
        inspect_archive(self.rewritten, "arm64-v8a")

    def test_rejects_unexpected_abi_in_source(self):
        self._stock_apk(extra_abi=True)
        with self.assertRaisesRegex(ValueError, "ABI"):
            rewrite_unsigned_apk(
                self.source, self.rewritten, "arm64-v8a", self.engine, self.payload
            )

    def test_inspection_rejects_flutter_assets_and_dolby(self):
        self._stock_apk()
        with self.assertRaisesRegex(ValueError, "flutter_assets"):
            inspect_archive(self.source, "arm64-v8a")

    def test_inspection_rejects_dart_symbol_archive(self):
        self._stock_apk()
        rewrite_unsigned_apk(
            self.source, self.rewritten, "arm64-v8a", self.engine, self.payload
        )
        with ZipFile(self.rewritten, "a") as archive:
            archive.writestr("app.android-arm64.symbols", b"private symbols")
        with self.assertRaisesRegex(ValueError, "symbol archive"):
            inspect_archive(self.rewritten, "arm64-v8a")

    def test_rejects_path_traversal_in_input_apk(self):
        self._stock_apk()
        with ZipFile(self.source, "a") as archive:
            archive.writestr("../escape.txt", b"unsafe")
        with self.assertRaisesRegex(ValueError, "unsafe ZIP path"):
            rewrite_unsigned_apk(
                self.source, self.rewritten, "arm64-v8a", self.engine, self.payload
            )


if __name__ == "__main__":
    unittest.main()
