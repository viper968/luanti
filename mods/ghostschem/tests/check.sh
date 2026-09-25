#!/bin/sh
# Syntax-check and run the standalone specs.
#
# Use LuaJIT if it is available, because it is stricter than the Lua 5.1
# reference implementation and is what most Luanti builds actually ship.
# In particular, 5.1's lexer silently accepts unknown escape sequences such
# as "\." (treating the backslash as a literal), while LuaJIT rejects them.
# A file can therefore pass `luac5.1 -p` and still fail to load in-game.

set -e
cd "$(dirname "$0")/.."

if command -v luajit >/dev/null 2>&1; then
	LUA=luajit
	CHECK='luajit -e'
elif command -v lua5.1 >/dev/null 2>&1; then
	LUA=lua5.1
	CHECK='lua5.1 -e'
	echo "warning: luajit not found, falling back to lua5.1, which accepts"
	echo "         invalid escape sequences that LuaJIT rejects"
else
	echo "no lua5.1 or luajit found" >&2
	exit 1
fi

status=0

echo "== syntax ($LUA) =="
for f in *.lua tests/*.lua; do
	if $CHECK "assert(loadfile('$f'))" 2>/tmp/gs_check_err; then
		echo "  ok   $f"
	else
		echo "  FAIL $f"
		sed 's/^/       /' /tmp/gs_check_err
		status=1
	fi
done
rm -f /tmp/gs_check_err

echo
echo "== specs =="
$LUA tests/rotation_spec.lua || status=1
$LUA tests/culling_spec.lua || status=1
$LUA tests/import_spec.lua || status=1

if command -v node >/dev/null 2>&1; then
	echo
	echo "== json2mts (node) =="
	node tests/json2mts_spec.js || status=1
else
	echo
	echo "warning: node not found, skipping tests/json2mts_spec.js"
fi

echo
if [ $status -eq 0 ]; then
	echo "all checks passed"
else
	echo "CHECKS FAILED"
fi
exit $status
