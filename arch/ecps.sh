#!/bin/sh
# fan loads its files from the working directory and writes its settings there: run from a per-user folder that
# links the installed, read-only ones. The same folder the Nix package uses
dir="${XDG_DATA_HOME:-$HOME/.local/share}/ecps"
mkdir -p "$dir"
for f in /usr/share/ecps/*; do
  ln -sfn "$f" "$dir/"
done
cd "$dir" || exit 1
export ECPS_PIANO_DIR="${ECPS_PIANO_DIR:-/usr/share/ecps/steinway-keys}"
export ECPS_BUILD_INFO=/usr/share/ecps/build-info
exec /usr/lib/ecps/ecps "$@"
