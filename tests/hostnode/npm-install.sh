#!/bin/bash
# npm on this machine with hostnode: install a small package into a fresh
# directory and require it. Needs the network. Run in Debian:
#   /run/hostnode/tests/npm-install.sh [hostnode|node] [PACKAGE]
here="$(dirname "$0")"
case "${1:-hostnode}" in
hostnode) exe="$here/../hostnode" ;;
node) exe=node ;;
*) echo "usage: $0 [hostnode|node] [PACKAGE]" >&2; exit 2 ;;
esac
package="${2:-left-pad}"
dir="$(mktemp -d /tmp/npm-test.XXXXXX)"
cd "$dir" || exit 1
echo '{ "name": "npm-test", "version": "1.0.0", "private": true }' > package.json
export HOME="$dir" npm_config_cache="$dir/.npm-cache" npm_config_update_notifier=false npm_config_fund=false
npm_cli=/usr/share/nodejs/npm/bin/npm-cli.js
time "$exe" "$npm_cli" install --no-audit --loglevel=warn "$package"
echo "npm rc $?"
ls node_modules | head
echo "require: $("$exe" -e "const m = require('$package'); console.log(typeof m, Object.keys(require('./node_modules/$package/package.json')).length > 3)")"
