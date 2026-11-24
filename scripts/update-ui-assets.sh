#!/usr/bin/env bash
# Update only application-owned web UI/PWA assets and preserve user-added files.
set -Eeuo pipefail
if [[ $# -ne 2 ]]; then
  echo "Usage: $0 SOURCE_UI_DIR DEST_UI_DIR" >&2
  exit 2
fi
source_dir="$(cd -- "$1" && pwd -P)"
dest_dir="$2"
install -d -m 755 "$dest_dir"
for asset in index.html app.js style.css manifest.webmanifest sw.js icon-192.png icon-512.png; do
  if [[ ! -f "$source_dir/$asset" ]]; then
    echo "Missing UI asset: $source_dir/$asset" >&2
    exit 1
  fi
  install -m 644 "$source_dir/$asset" "$dest_dir/$asset"
done
