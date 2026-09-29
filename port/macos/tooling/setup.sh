#!/bin/bash
# Works from a file or curl | bash -s -- '/path/to/user-owned.iso'. Never reads stdin.
set -euo pipefail
export GIT_TERMINAL_PROMPT=0
fail() { printf 'Setup failed: %s\n' "$*" >&2; exit 1; }
[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || fail 'Apple Silicon macOS is required.'
iso=${1:-}
[ -n "$iso" ] || fail 'Supply your local Xbox Halo ISO: bash setup.sh "/path/Halo.iso"'
[ -f "$iso" ] || fail "ISO does not exist: $iso"
shift
work=${HALO_WORK_DIR:-"$HOME/Library/Caches/halo-ce-apple-silicon/$(date +%Y%m%d-%H%M%S)-$$"}
output=${HALO_APP_OUTPUT:-"$HOME/Applications/Halo Combat Evolved.app"}
[ ! -e "$output" ] || fail "Output already exists; preserve it and set HALO_APP_OUTPUT to a new app path: $output"
xcode-select -p >/dev/null 2>&1 || fail 'Install Apple Command Line Tools with xcode-select --install, then rerun.'
if ! command -v brew >/dev/null 2>&1; then
  [ ! -x /opt/homebrew/bin/brew ] || export PATH="/opt/homebrew/bin:$PATH"
fi
command -v brew >/dev/null 2>&1 || fail 'Install Homebrew from https://brew.sh first, then rerun (its administrator setup cannot prompt inside a piped script).'
# Validate the optional current-checkout locations before creating staging.
source_checkout_arg=''
need_source_path=0
for setup_argument in "$@"; do
  if [ "$need_source_path" = 1 ]; then
    source_checkout_arg=$setup_argument
    need_source_path=0
    continue
  fi
  case "$setup_argument" in
    --source-checkout) need_source_path=1 ;;
    --source-checkout=*) source_checkout_arg=${setup_argument#*=} ;;
  esac
done
[ "$need_source_path" = 0 ] || fail '--source-checkout needs a directory argument.'
preflight_python=$(command -v python3 || true)
[ -n "$preflight_python" ] || preflight_python=/usr/bin/python3
if [ -n "$source_checkout_arg" ]; then
  "$preflight_python" - "$work" "$output" "$source_checkout_arg" <<'PY_CHECKOUT'
from pathlib import Path
import sys
work, output, checkout = [Path(value).expanduser().resolve() for value in sys.argv[1:]]
if not checkout.is_dir():
    raise SystemExit('Source checkout directory is missing.')
if work.is_relative_to(checkout) or output.is_relative_to(checkout):
    raise SystemExit('Keep build workspace and app output outside the source checkout.')
PY_CHECKOUT
fi
mkdir -p "$work" "$(dirname "$output")"
# A local checkout uses its own reviewed tools; a piped script fetches the public project.
script_dir=''
if [ -n "${BASH_SOURCE[0]:-}" ] && [ -f "${BASH_SOURCE[0]}" ]; then
  script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
fi
if [ -n "$script_dir" ] && [ -f "$script_dir/pins.json" ]; then
  tooling=$script_dir
else
  tooling="$work/tooling"
  git clone --depth=1 https://github.com/pkyanam/halo-ce-apple-silicon.git "$tooling" </dev/null
fi
"$preflight_python" "$tooling/scripts/xbox_assets.py" --inspect "$iso" </dev/null
brew install python cmake pkgconf sdl3 ffmpeg llvm lld </dev/null
python="$(brew --prefix)/bin/python3"
"$python" - "$work" "$tooling" <<'PY'
from pathlib import Path
import sys
if Path(sys.argv[1]).resolve().is_relative_to(Path(sys.argv[2]).resolve()):
    raise SystemExit('Use HALO_WORK_DIR outside the tooling repository; generated game data/code stays outside its tree.')
PY
"$python" -m venv "$work/venv"
python="$work/venv/bin/python"
"$python" -m pip install --disable-pip-version-check 'capstone==5.0.6' </dev/null
"$python" -m unittest discover -s "$tooling/tests" </dev/null
"$python" "$tooling/scripts/xbox_assets.py" "$iso" "$work/extracted-assets" </dev/null
"$python" "$tooling/scripts/build.py" --work-dir "$work/build" --assets "$work/extracted-assets" --output "$output" "$@" </dev/null

if [ ! -d "$output" ]; then
  printf "Preparation completed; extraction staging remains in the selected workspace.\n"
  exit 0
fi

# Only this freshly extracted, owned staging directory is removed. The ISO and
# existing user state are preserved; the signed/check-verified app owns its data.
"$python" - "$work/extracted-assets" "$output" <<'PY'
import json, shutil, sys
from pathlib import Path
stage, app = map(Path, sys.argv[1:])
original = json.loads((stage/'extraction-provenance.json').read_text())
seal = json.loads((app/'Contents/Resources/GameData/asset-manifest.json').read_text())
if original['image_sha256'] != seal['source_image'].get('image_sha256'):
    raise SystemExit('App data provenance mismatch; preserving extraction staging.')
shutil.rmtree(stage)
PY
