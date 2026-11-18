#!/usr/bin/env python3
"""Verify and unpack a Horizon Mesa SDK into the build dependency directory.

    unpack-switch-mesa.py ARCHIVE DESTINATION [SHA256 [NAME]]

Without SHA256 the archive must be the unified Switch SDK (SDK_SHA256); the Switch 2 build passes the Switch 2 SDK's.
"""

import hashlib
from pathlib import Path, PurePosixPath
import sys
import zipfile

SDK_SHA256 = "6c919eb1bbb7df5729940b5b4ecaa5e0daaa05bd06ebde9f5b676ce6c3eff04b"


def main():
    if len(sys.argv) not in (3, 4, 5):
        raise SystemExit("usage: unpack-switch-mesa.py ARCHIVE DESTINATION [SHA256 [NAME]]")
    archive, destination = Path(sys.argv[1]), Path(sys.argv[2])
    expected = sys.argv[3] if len(sys.argv) > 3 else SDK_SHA256
    name = sys.argv[4] if len(sys.argv) > 4 else "mesa-26.2.7-switch-unified-horizon-sdk.zip"
    with archive.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    if digest != expected:
        raise SystemExit(f"The archive {archive} does not match {name}")
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
