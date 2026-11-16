#!/bin/zsh
set -eu
setopt PIPE_FAIL
SCRIPT_DIR="${0:A:h}"
exec python3 "$SCRIPT_DIR/tools/diagnose.py" "$@"
