#!/bin/bash
# Generates a test world with the given game and mapgen.
# usage: make_world.sh <luantiserver binary> <output dir> <gameid> <mapgen> <seed>
set -e
server=$1; world=$2; game=$3; mg=$4; seed=$5
here=$(cd "$(dirname "$0")" && pwd)
rm -rf "$world"; mkdir -p "$world/worldmods"
cp -r "$here/emerger" "$world/worldmods/"
printf "gameid = %s\nbackend = sqlite3\n" "$game" > "$world/world.mt"
printf "mg_name = %s\nseed = %s\n[end_of_params]\n" "$mg" "$seed" > "$world/map_meta.txt"
conf=$(mktemp)
printf "num_emerge_threads = 4\nipv6_server = false\nenable_ipv6 = false\n" > "$conf"
"$server" --world "$world" --config "$conf" --gameid "$game"
rm -f "$conf"
