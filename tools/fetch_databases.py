#!/usr/bin/env python3
"""Fetch the raw Amiibo metadata databases used by Amiibo Zero.

The device builds and validates its binary search index locally; this helper only
downloads the upstream JSON source files into a chosen application-data folder.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import urllib.request

AMIIBO_URL = "https://raw.githubusercontent.com/8bitDream/AmiiboAPI/dev/database/amiibo.json"
GAMES_URL = "https://raw.githubusercontent.com/8bitDream/AmiiboAPI/dev/database/games_info.json"


def download_raw(url: str, destination: Path) -> int:
    """Download one URL atomically and return the number of bytes written."""
    request = urllib.request.Request(
        url, headers={"User-Agent": "Amiibo-Zero-database-fetcher/0.1.0"}
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read()
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".download")
    temporary.write_bytes(data)
    temporary.replace(destination)
    return len(data)


def fetch_databases(output_directory: Path) -> None:
    """Download both required JSON databases into the target directory."""
    output_directory.mkdir(parents=True, exist_ok=True)
    for filename, url in (("amiibo.json", AMIIBO_URL), ("games_info.json", GAMES_URL)):
        destination = output_directory / filename
        size = download_raw(url, destination)
        print(f"Downloaded {filename} ({size} bytes)")


def main() -> int:
    """Parse command-line arguments, fetch the databases, and return a process status."""
    parser = argparse.ArgumentParser(
        description=(
            "Download raw AmiiboAPI JSON for Amiibo Zero; "
            "index validation/building stay on-device."
        )
    )
    parser.add_argument(
        "output_directory",
        type=Path,
        help=(
            "Destination directory, normally the SD card's "
            "apps_data/amiibo_zero directory."
        ),
    )
    args = parser.parse_args()
    fetch_databases(args.output_directory)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
