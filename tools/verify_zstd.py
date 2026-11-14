#!/usr/bin/env python3
"""Validate archive integrity, zstd frames, and archive member safety."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import subprocess
import tarfile

from package_zstd import TARGETS


def verify(package_dir: Path, allow_partial: bool = False) -> None:
    metadata = json.loads((package_dir / "package.json").read_text(encoding="utf-8"))
    source = package_dir / "package.fire"
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    assert digest == metadata["sha256"], "SHA-256 mismatch"
    assert source.stat().st_size == metadata["size"], "Archive length mismatch"
    assert metadata["id"] == "zstd" and metadata["version"] == "0.1.0"
    assert metadata["nativeAbiVersion"] == 1
    assert metadata["platforms"] == list(p for p in TARGETS if p in metadata["platforms"])
    if not allow_partial:
        assert metadata["platforms"] == list(TARGETS), "Production package needs all platforms"

    # zstd's CLI must decompress without error, including checksum verification.
    completed = subprocess.run(
        ["zstd", "-q", "-d", "-c", str(source)],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    with tarfile.open(fileobj=io.BytesIO(completed.stdout), mode="r:") as archive:
        names = set()
        for member in archive.getmembers():
            path = PurePosixPath(member.name)
            assert member.isfile(), f"Non-regular member: {member.name}"
            assert not path.is_absolute() and ".." not in path.parts, "Unsafe tar member"
            assert path.parts[0] == "zstd", "Archive escaped the module root"
            assert member.name not in names, "Duplicate archive member"
            names.add(member.name)
        assert "zstd/index.napalm" in names
        assert "zstd/LICENSE.zstd" in names
        for platform in metadata["platforms"]:
            assert f"zstd/native/{platform}/{TARGETS[platform]}" in names
        assert len(names) == len(metadata["platforms"]) + 2
    print(f"Verified {source}: {digest} ({len(names)} safe members)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--allow-partial", action="store_true")
    arguments = parser.parse_args()
    verify(arguments.package, allow_partial=arguments.allow_partial)


if __name__ == "__main__":
    main()
