"""End-to-end and adversarial unpack tests for the FireZstd native ABI.

Usage: python tests/unpack_native_smoke.py PATH_TO_LIBZSTD
Uses only Python's standard library and the supplied native shared library.
"""

from __future__ import annotations

import ctypes
import io
import os
from pathlib import Path
import sys
import tarfile
from tempfile import TemporaryDirectory


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main(library_path: str) -> None:
    library = ctypes.CDLL(str(Path(library_path).resolve()))
    library.fire_zstd_abi_version.argtypes = ()
    library.fire_zstd_abi_version.restype = ctypes.c_int32
    library.fire_zstd_last_error.argtypes = ()
    library.fire_zstd_last_error.restype = ctypes.c_char_p
    library.fire_zstd_pack_files.argtypes = (
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int32,
    )
    library.fire_zstd_pack_files.restype = ctypes.c_int32
    library.fire_zstd_unpack_files.argtypes = (
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64,
    )
    library.fire_zstd_unpack_files.restype = ctypes.c_int32
    library.fire_zstd_compress_file.argtypes = (
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int32,
    )
    library.fire_zstd_compress_file.restype = ctypes.c_int32
    library.fire_zstd_decompress_file.argtypes = (
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64,
    )
    library.fire_zstd_decompress_file.restype = ctypes.c_int32
    check(library.fire_zstd_abi_version() == 3, "native ABI must be 3")

    def pack(directory: Path, manifest: str, archive: Path, level: int = 18) -> int:
        return library.fire_zstd_pack_files(
            os.fsencode(directory), manifest.encode(), os.fsencode(archive), level,
        )

    def unpack(archive: Path, destination: Path, maximum: int = 1024 * 1024 * 16) -> int:
        return library.fire_zstd_unpack_files(
            os.fsencode(archive), os.fsencode(destination), maximum,
        )

    def compress(source: Path, destination: Path) -> None:
        result = library.fire_zstd_compress_file(
            os.fsencode(source), os.fsencode(destination), 3,
        )
        check(result == 0, f"could not create test archive: {library.fire_zstd_last_error()!r}")

    def assert_failed(archive: Path, destination: Path, expected: int | None = None) -> None:
        code = unpack(archive, destination) if expected is None else unpack(archive, destination, expected)
        check(code != 0, f"archive unexpectedly extracted: {archive}")
        check(library.fire_zstd_last_error(), "missing extraction error details")
        check(not destination.exists(), f"incomplete extraction was published: {destination}")
        check(not any(destination.parent.glob(destination.name + ".fire-unpack-*")),
              "private extraction directory was not discarded")

    def make_tar(entries: list[tuple[str, bytes, bytes | None]], dest: Path) -> Path:
        raw = dest.with_suffix(".tar")
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w", format=tarfile.USTAR_FORMAT) as archive:
            for name, body, entry_type in entries:
                info = tarfile.TarInfo(name)
                info.mode = 0o644
                if entry_type is not None:
                    info.type = entry_type
                    info.linkname = "index.napalm"
                    info.size = 0
                    archive.addfile(info)
                else:
                    info.size = len(body)
                    archive.addfile(info, io.BytesIO(body))
        raw.write_bytes(stream.getvalue())
        compress(raw, dest)
        return dest

    with TemporaryDirectory(prefix="firezstd-unpack-") as temp:
        root = Path(temp)
        sources = root / "input"
        sources.mkdir()
        original_data = {
            "index.napalm": bytes((i * 13 + (i >> 5)) % 256 for i in range(262144)),
            "LICENSE.zstd": b"FireZstd license test\n",
            "empty.bin": b"",
            "compressible.bin": b"z" * (128 * 1024 * 6),
        }
        for name, content in original_data.items():
            (sources / name).write_bytes(content)
        archive = root / "package.tar.zst"
        names = "\n".join(original_data)
        code = pack(sources, names, archive)
        check(code == 0, f"packer failed: {library.fire_zstd_last_error()!r}")
        extracted = root / "unpacked"
        code = unpack(archive, extracted)
        check(code == 0, f"unpack failed: {library.fire_zstd_last_error()!r}")
        check(not library.fire_zstd_last_error(), "stale error string after successful extraction")
        for name, content in original_data.items():
            check((extracted / name).read_bytes() == content, f"mismatch: {name}")
        check(set(p.name for p in extracted.iterdir()) == set(original_data), "extra unpacked files")

        # No silent overwrite, even if the archive is otherwise valid.
        check(unpack(archive, extracted) != 0, "re-extraction overwrote an existing directory")
        check((extracted / "LICENSE.zstd").read_bytes() == original_data["LICENSE.zstd"],
              "preexisting directory was modified")

        # Open the TAR with Python's standard reader to verify compatibility.
        decompressed = root / "reference.tar"
        code = library.fire_zstd_decompress_file(os.fsencode(archive), os.fsencode(decompressed), 5_000_000)
        check(code == 0, "could not decompress reference TAR")
        with tarfile.open(decompressed) as reference:
            check(reference.getnames() == list(original_data), "TAR names or order differ")

        # Extraction budget is checked on TAR header entry sizes.
        small_limit = root / "limited"
        assert_failed(archive, small_limit, 1000)

        wrong_checksum = root / "checksum.tar.zst"
        data = bytearray(decompressed.read_bytes())
        data[10] ^= 2
        raw_corrupt = root / "checksum.tar"
        raw_corrupt.write_bytes(data)
        compress(raw_corrupt, wrong_checksum)
        assert_failed(wrong_checksum, root / "checksum_output")

        for label, name, file_type in [
            ("traversal", "../escape", None),
            ("nested", "dir/child.txt", None),
            ("absolute", "/tmp/escape", None),
            ("windows-drive", "C:escape", None),
            ("device", "CON.txt", None),
            ("link", "pointer", tarfile.SYMTYPE),
            ("hardlink", "pointer", tarfile.LNKTYPE),
            ("directory", "folder", tarfile.DIRTYPE),
        ]:
            malicious = root / f"{label}.tar.zst"
            make_tar([(name, b"unsafe", file_type)], malicious)
            assert_failed(malicious, root / f"{label}_out")
            check(not (root / "escape").exists(), "path traversal escaped the extraction directory")

        duplicate = root / "duplicate.tar.zst"
        make_tar([("File.txt", b"1", None), ("file.txt", b"2", None)], duplicate)
        assert_failed(duplicate, root / "duplicate_out")

        too_many = root / "too_many.tar.zst"
        make_tar([(f"file_{index}.txt", b"x", None) for index in range(65)], too_many)
        assert_failed(too_many, root / "too_many_out")

        # A single Zstd frame is required, even if both frames are individually valid.
        multiple = root / "multiple.tar.zst"
        multiple.write_bytes(archive.read_bytes() + archive.read_bytes())
        assert_failed(multiple, root / "multiple_out")

        trailing = root / "trailing.tar.zst"
        trailing.write_bytes(archive.read_bytes() + b"junk")
        assert_failed(trailing, root / "trailing_out")

        truncated = root / "truncated.tar.zst"
        truncated.write_bytes(archive.read_bytes()[:-5])
        assert_failed(truncated, root / "truncated_out")

        bad_padding_raw = root / "bad_padding.tar"
        data = bytearray(decompressed.read_bytes())
        # First entry is exactly 262144 bytes, and the next entry begins at
        # 512 + 262144. Corrupt the alignment between later small entries.
        later_body = 512 + 262144 + 512
        data[later_body + len(original_data["LICENSE.zstd"])] = 0xAA
        bad_padding_raw.write_bytes(data)
        bad_padding = root / "bad_padding.tar.zst"
        compress(bad_padding_raw, bad_padding)
        assert_failed(bad_padding, root / "bad_padding_out")

        # Check the maximum supported entry count, including zero-length files.
        many = root / "many.tar.zst"
        make_tar([(f"f{index:02}.txt", b"", None) for index in range(64)], many)
        code = unpack(many, root / "many_out")
        check(code == 0, f"64-file archive incorrectly rejected: {library.fire_zstd_last_error()!r}")
        check(len(list((root / "many_out").iterdir())) == 64, "missing empty files")

    print("FireZstd unpack tests passed: round-trips, 64 files, limits, malformed archives, security")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: python tests/unpack_native_smoke.py PATH_TO_LIBZSTD")
    main(sys.argv[1])
