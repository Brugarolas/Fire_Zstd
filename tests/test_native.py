"""Exercise the stable Fire Zstd C ABI against a built native library."""

from __future__ import annotations

import argparse
import ctypes
import os
from pathlib import Path
import tempfile
import unittest


class NativeZstdTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        library_path = os.environ.get("FIRE_ZSTD_TEST_LIBRARY")
        if not library_path:
            raise RuntimeError("FIRE_ZSTD_TEST_LIBRARY must name a built native library")
        cls.lib = ctypes.CDLL(str(Path(library_path).resolve()))
        cls.lib.fire_zstd_abi_version.restype = ctypes.c_int32
        cls.lib.fire_zstd_version_number.restype = ctypes.c_int32
        cls.lib.fire_zstd_last_error.restype = ctypes.c_char_p
        cls.lib.fire_zstd_compress_file.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int32]
        cls.lib.fire_zstd_compress_file.restype = ctypes.c_int32
        cls.lib.fire_zstd_decompress_file.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64]
        cls.lib.fire_zstd_decompress_file.restype = ctypes.c_int32

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="fire-zstd-")
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name)
        self.source = self.folder / "original-🔥.dat"
        self.compressed = self.folder / "archive-🔥.zst"
        self.output = self.folder / "restored-🔥.dat"

    def check_ok(self, code: int) -> None:
        self.assertEqual(code, 0, self.lib.fire_zstd_last_error().decode("utf-8"))

    def compress(self, level: int = 3) -> int:
        return self.lib.fire_zstd_compress_file(
            os.fsencode(self.source), os.fsencode(self.compressed), level
        )

    def decompress(self, limit: int = 1024 * 1024 * 64) -> int:
        return self.lib.fire_zstd_decompress_file(
            os.fsencode(self.compressed), os.fsencode(self.output), limit
        )

    def test_abi_and_version(self) -> None:
        self.assertEqual(self.lib.fire_zstd_abi_version(), 1)
        self.assertGreaterEqual(self.lib.fire_zstd_version_number(), 10500)

    def test_roundtrip_empty(self) -> None:
        self.source.write_bytes(b"")
        self.check_ok(self.compress())
        self.check_ok(self.decompress())
        self.assertEqual(self.output.read_bytes(), b"")

    def test_roundtrip_multichunk(self) -> None:
        payload = os.urandom(1024 * 1024 + 173)
        self.source.write_bytes(payload)
        self.check_ok(self.compress(10))
        self.check_ok(self.decompress(2 * 1024 * 1024))
        self.assertEqual(self.output.read_bytes(), payload)

    def test_lowest_compression_level(self) -> None:
        self.source.write_bytes(b"Napalm" * 10000)
        self.check_ok(self.compress(-5))
        self.check_ok(self.decompress())
        self.assertEqual(self.output.read_bytes(), self.source.read_bytes())

    def test_invalid_level_leaves_output_untouched(self) -> None:
        self.source.write_bytes(b"source")
        self.compressed.write_bytes(b"unchanged")
        self.assertNotEqual(self.compress(99999), 0)
        self.assertEqual(self.compressed.read_bytes(), b"unchanged")

    def test_truncation_does_not_replace_existing_file(self) -> None:
        self.source.write_bytes(os.urandom(500000))
        self.check_ok(self.compress())
        self.compressed.write_bytes(self.compressed.read_bytes()[:-7])
        self.output.write_bytes(b"keep")
        self.assertNotEqual(self.decompress(), 0)
        self.assertEqual(self.output.read_bytes(), b"keep")
        self.assertFalse(list(self.folder.glob("*.fire-tmp-*")))

    def test_corrupt_frame_does_not_publish_output(self) -> None:
        self.compressed.write_bytes(b"not zstandard data")
        self.assertNotEqual(self.decompress(), 0)
        self.assertFalse(self.output.exists())

    def test_expansion_limit_does_not_publish_output(self) -> None:
        self.source.write_bytes(b"X" * (1024 * 1024))
        self.check_ok(self.compress())
        self.assertEqual(self.decompress(17), 4)
        self.assertFalse(self.output.exists())

    def test_concatenated_frames(self) -> None:
        self.source.write_bytes(b"first-" * 3000)
        self.check_ok(self.compress())
        first = self.compressed.read_bytes()
        self.source.write_bytes(b"second-" * 1000)
        self.check_ok(self.compress())
        self.compressed.write_bytes(first + self.compressed.read_bytes())
        self.check_ok(self.decompress())
        self.assertEqual(self.output.read_bytes(), b"first-" * 3000 + b"second-" * 1000)

    def test_missing_source_preserves_destination(self) -> None:
        self.compressed.write_bytes(b"stay")
        self.assertNotEqual(self.compress(), 0)
        self.assertEqual(self.compressed.read_bytes(), b"stay")

    def test_same_source_destination_is_rejected(self) -> None:
        self.source.write_bytes(b"source")
        code = self.lib.fire_zstd_compress_file(
            os.fsencode(self.source), os.fsencode(self.source), 3
        )
        self.assertEqual(code, 1)
        self.assertEqual(self.source.read_bytes(), b"source")


if __name__ == "__main__":
    unittest.main()
