#!/usr/bin/env python3
"""Create a deterministic Fire archive and its detached SHA-256 manifest."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import tempfile

VERSION = "0.1.0"
TARGETS = {
    "windows-x86_64": "fire_zstd.dll",
    "windows-aarch64": "fire_zstd.dll",
    "linux-x86_64": "libfire_zstd.so",
    "linux-aarch64": "libfire_zstd.so",
    "macos-x86_64": "libfire_zstd.dylib",
    "macos-aarch64": "libfire_zstd.dylib",
}


def run(source: Path, destination: Path, native_library: Path, allow_partial: bool) -> None:
    source = source.resolve(strict=True)
    native_library = native_library.resolve(strict=True)
    index_file = source / "index.napalm"
    if not index_file.is_file() or index_file.is_symlink():
        raise ValueError("Missing regular source/index.napalm")

    present = []
    for platform, filename in TARGETS.items():
        target = source / "native" / platform / filename
        if target.is_file() and not target.is_symlink():
            present.append(platform)
        elif not allow_partial:
            raise ValueError(f"Missing required platform binary: {target}")

    if not present:
        raise ValueError("No platform binaries available")
    license_file = source / "LICENSE.zstd"
    if not license_file.is_file() and not allow_partial:
        raise ValueError("Release packages must carry the upstream Zstd license")

    files = [index_file]
    if license_file.is_file():
        files.append(license_file)
    files.extend(source / "native" / platform / TARGETS[platform] for platform in present)

    destination.mkdir(parents=True, exist_ok=True)
    lib = ctypes.CDLL(str(native_library))
    lib.fire_zstd_abi_version.restype = ctypes.c_int32
    lib.fire_zstd_version_number.restype = ctypes.c_int32
    lib.fire_zstd_compress_file.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int32]
    lib.fire_zstd_compress_file.restype = ctypes.c_int32
    lib.fire_zstd_last_error.restype = ctypes.c_char_p
    if lib.fire_zstd_abi_version() != 1:
        raise RuntimeError("Native library has incompatible Fire ABI version")

    with tempfile.TemporaryDirectory(prefix="fire-package-") as temporary:
        tar_path = Path(temporary) / "payload.tar"
        fire_path = Path(temporary) / "package.fire"
        with tarfile.open(tar_path, "w", format=tarfile.USTAR_FORMAT) as archive:
            for path in sorted(files, key=lambda value: value.relative_to(source).as_posix()):
                if path.is_symlink() or not path.is_file():
                    raise ValueError(f"Only regular files allowed: {path}")
                arc_name = "zstd/" + path.relative_to(source).as_posix()
                metadata = tarfile.TarInfo(arc_name)
                metadata.size = path.stat().st_size
                metadata.mode = 0o644
                metadata.mtime = 0
                metadata.uid = metadata.gid = 0
                metadata.uname = metadata.gname = ""
                with path.open("rb") as stream:
                    archive.addfile(metadata, stream)

        status = lib.fire_zstd_compress_file(
            str(tar_path).encode("utf-8"), str(fire_path).encode("utf-8"), 19
        )
        if status:
            detail = lib.fire_zstd_last_error()
            raise RuntimeError(f"Native zstd compression failed: {detail!r}")

        sha256 = hashlib.sha256()
        with fire_path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                sha256.update(chunk)
        digest = sha256.hexdigest()
        metadata = {
            "id": "zstd",
            "name": "zstd",
            "version": VERSION,
            "file": "package.fire",
            "format": "tar.zstd",
            "sha256": digest,
            "size": fire_path.stat().st_size,
            "user": "Brugarolas",
            "repository": "https://github.com/Brugarolas/fire",
            "homepage": "https://github.com/facebook/zstd",
            "description": "Streaming Zstandard compression and decompression for Napalm via a portable native ABI.",
            "license": "NOASSERTION",
            "thirdPartyLicenses": [{
                "package": "Zstandard",
                "version": "1.5.7",
                "license": "BSD-3-Clause",
                "file": "zstd/LICENSE.zstd",
            }],
            "entry": "zstd/index.napalm",
            "nativeAbiVersion": 1,
            "zstdVersionNumber": lib.fire_zstd_version_number(),
            "platforms": present,
            "dependencies": [],
        }
        manifest_tmp = Path(temporary) / "package.json"
        manifest_tmp.write_text(json.dumps(metadata, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        # Publish the manifest last so readers never see it before its archive.
        shutil.copyfile(fire_path, destination / "package.fire")
        shutil.copyfile(manifest_tmp, destination / "package.json")
    print(f"Built {destination} (SHA-256 {digest}, targets: {', '.join(present)})")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="Directory containing index.napalm and native/")
    parser.add_argument("--output", type=Path, required=True, help="packages/zstd/0.1.0")
    parser.add_argument("--library", type=Path, required=True, help="Linux native library for packaging")
    parser.add_argument("--allow-partial", action="store_true", help="Development only: permit missing targets/license")
    args = parser.parse_args()
    run(args.source, args.output, args.library, args.allow_partial)


if __name__ == "__main__":
    main()
