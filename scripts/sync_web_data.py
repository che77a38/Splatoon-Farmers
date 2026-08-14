#!/usr/bin/env python3
"""Sync web/* into firmware/data/ before every PlatformIO build.

The web/ directory is the authoritative source for the user-facing
WebUI assets (index.html, app.js, styles.css, etc.) and firmware/
data/ is the build-time staging area that gets packaged into the
LittleFS image by `pio run -t uploadfs`. Without this sync the
board serves stale assets.

This script is wired in via platformio.ini:

    extra_scripts = pre:scripts/sync_web_data.py

so every `pio run` and `pio run -t uploadfs` reflects the latest web/
tree. Idempotent and cheap (~5 ms for the current 8-file payload).
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "web")
DST = os.path.join(ROOT, "firmware", "data")

if not os.path.isdir(SRC):
    sys.exit(f"sync_web_data: source not found: {SRC}")

os.makedirs(DST, exist_ok=True)
copied = 0
for entry in os.listdir(SRC):
    src_path = os.path.join(SRC, entry)
    dst_path = os.path.join(DST, entry)
    if os.path.isdir(src_path):
        continue
    # Always overwrite the destination so the data partition
    # reflects the latest web/ tree on every build, even when the
    # source and destination mtimes are identical (copy2's default
    # is "copy only if source is newer or destination is missing",
    # which is not what we want here).
    shutil.copyfile(src_path, dst_path)
    copied += 1
print(f"sync_web_data: refreshed {copied} file(s) from web/ to firmware/data/",
      flush=True)
