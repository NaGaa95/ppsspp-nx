#!/usr/bin/env python3
"""Verify and unpack the unified Horizon Mesa SDK into the build dependency directory."""

import hashlib
from pathlib import Path, PurePosixPath
import sys
import zipfile

SDK_SHA256 = "c2f78d4c627adcd6950ae1be305f845a059eec325a2d9905c9eb48e1b5ab573f"


def main():
    archive, destination = map(Path, sys.argv[1:])
    with archive.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    if digest != SDK_SHA256:
        raise SystemExit("The Mesa SDK archive does not match mesa-26.2.4-switch-unified-horizon-sdk.zip")
    destination = destination.resolve()
    with zipfile.ZipFile(archive) as source:
        for item in source.infolist():
            name = PurePosixPath(item.filename.replace("\\", "/"))
            if name.is_absolute() or ".." in name.parts or any(":" in part for part in name.parts):
                raise SystemExit("Unsafe path in Mesa SDK archive")
            target = destination.joinpath(*name.parts).resolve()
            if not target.is_relative_to(destination) or (item.external_attr >> 16) & 0o170000 == 0o120000:
                raise SystemExit("Unsafe entry in Mesa SDK archive")
        source.extractall(destination)
    (destination / ".ppsspp-sdk-sha256").write_text(digest + "\n", encoding="ascii", newline="\n")


if __name__ == "__main__":
    main()
