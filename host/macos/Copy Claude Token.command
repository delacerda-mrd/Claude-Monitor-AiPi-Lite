#!/bin/zsh
# Double-clicked .command files don't get a login-shell PATH — point the
# script at claude explicitly (same trick as the Linux systemd unit).
export CLAUDE_BIN="$HOME/.local/bin/claude"
python3 "$(dirname "$0")/copy_claude_token.py"
