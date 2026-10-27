#!/usr/bin/env python3
"""Download pinned public build inputs; never silently accept changed artifacts."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

REPO = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open("rb") as stream:
        checksum = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
        return checksum.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()
    for item in json.loads((REPO / "macos/downloads.json").read_text()):
        target = REPO / item["path"]
        if target.is_file():
            if digest(target) != item["sha256"]:
                raise SystemExit(f"Checksum mismatch: {target}. Inspect the file before rebuilding.")
        else:
            if args.offline:
                raise SystemExit(f"Missing offline build input: {target}")
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = target.with_suffix(target.suffix + ".partial")
            subprocess.run(["curl", "--fail", "--location", "--retry", "3", "--output",
                            str(temporary), item["url"]], check=True)
            if digest(temporary) != item["sha256"]:
                raise SystemExit(f"Downloaded artifact changed: {temporary}. Do not bypass the checksum.")
            temporary.replace(target)
        print(f"Verified {item['path']}", flush=True)


if __name__ == "__main__":
    main()
