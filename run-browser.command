#!/bin/sh
CDPATH= cd -P "$(dirname "$0")" || exit 1
exec python3 tools/serve_browser.py "$@"
