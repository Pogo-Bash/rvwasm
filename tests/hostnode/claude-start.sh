#!/bin/bash
# Claude Code on this machine, up to where it asks for an account: how long it
# takes with hostnode and with the system's node. Run in Debian:
#   /run/hostnode/tests/claude-start.sh [hostnode|node] [SECONDS]
# Prints the last screen, then when the first output came and when the theme
# was chosen (the screen after that is the choice of a login method, where this
# stops: nothing is entered there).
cd "$HOME" || exit 1
export DISABLE_AUTOUPDATER=1 USE_BUILTIN_RIPGREP=0 TERM=xterm-256color
here="$(dirname "$0")"
case "${1:-hostnode}" in
hostnode) exe="$here/../hostnode" ;;
node) exe=node ;;
*) echo "usage: $0 [hostnode|node] [SECONDS]" >&2; exit 2 ;;
esac
cli=/usr/local/lib/node_modules/@anthropic-ai/claude-code/cli.js
python3 "$here/inpty.py" "${2:-60}" "$exe $cli" "Choosethetextstyle" '\r' "Selectloginmethod" END
