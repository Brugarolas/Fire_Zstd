#!/usr/bin/env python3
"""Collect the six native builds from GitHub Actions artifacts."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil

from package_zstd import TARGETS


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    for platform, filename in TARGETS.items():
        source = args.artifacts / f"native-{platform}" / filename
        if not source.is_file() or source.is_symlink():
            raise ValueError(f"Missing or invalid artifact: {source}")
        destination = args.output / "native" / platform
        destination.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination / filename)
        print(f"Assembled {platform}")


if __name__ == "__main__":
    main()
