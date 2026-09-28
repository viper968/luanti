#!/bin/bash
# Runs luantiserver with the Tiny game, two headless bot clients walking in
# different directions, and the memcap heap tracker; writes a per-second CSV.
#
#   run_memtest.sh <luanti-root-with-bin/> <server.conf> <seconds> <outdir> [limit_kb]
#
# Needs: bin/luantiserver in the given root, bin/luanti for the bots (taken
# from BOT_ROOT if set, else the same root), and esp32/tools/memcap/libmemcap.so
# (built by this script if missing).
set -u
root=$(realpath "$1"); conf=$(realpath "$2"); secs=$3; out=$(realpath -m "$4"); limit=${5:-}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
port=${PORT:-30055}

mkdir -p "$out"
memcap="$here/../memcap/libmemcap.so"
[ -f "$memcap" ] || gcc -O2 -shared -fPIC -o "$memcap" "$here/../memcap/memcap.c" -ldl -lpthread || exit 1

# Fresh world using the Tiny game (found through LUANTI_GAME_PATH)
world="$out/world"; rm -rf "$world"; mkdir -p "$world/worldmods" "$out/games"
ln -sfn "$repo/esp32/tinygame" "$out/games/tiny"
ln -sfn "$here/esp32test_walker" "$world/worldmods/esp32test_walker"
printf 'gameid = tiny\nbackend = sqlite3\nplayer_backend = sqlite3\nauth_backend = sqlite3\nmod_storage_backend = sqlite3\n' > "$world/world.mt"
export LUANTI_GAME_PATH="$out/games"

echo "server: $root/bin/luantiserver  conf: $conf  duration: ${secs}s  limit: ${limit:-none}"
# NO_MEMCAP=1 runs without the heap tracker (e.g. for AddressSanitizer builds)
preload="$memcap"; [ -n "${NO_MEMCAP:-}" ] && preload=""
env MEMCAP_REPORT="$out/mem_now.txt" ${limit:+MEMCAP_LIMIT_KB=$limit} ${preload:+LD_PRELOAD=$preload} \
	"$root/bin/luantiserver" --config "$conf" --world "$world" --port $port --logfile "$out/server.log" \
	> "$out/server.out" 2>&1 &
server=$!
sleep 3

bots=()
for n in 1 2; do
	cp "$here/client.conf" "$out/client$n.conf"
	echo "name = bot$n" >> "$out/client$n.conf"
	"${BOT_ROOT:-$root}/bin/luanti" --config "$out/client$n.conf" --go --address 127.0.0.1 --port $port \
		--logfile "$out/client$n.log" > "$out/client$n.out" 2>&1 &
	bots+=($!)
done

# Sample: seconds, live KB, peak KB, failed allocations, server CPU seconds, RSS KB
echo "t,live_kb,peak_kb,failed,cpu_s,rss_kb" > "$out/mem.csv"
hz=$(getconf CLK_TCK)
for ((t = 1; t <= secs; t++)); do
	sleep 1
	kill -0 $server 2>/dev/null || { echo "server exited early at ${t}s"; break; }
	live=0 peak=0 failed=0
	[ -z "${NO_MEMCAP:-}" ] && { read -r live peak failed < "$out/mem_now.txt" 2>/dev/null || continue; }
	read -r -a st < /proc/$server/stat
	cpu=$(( (st[13] + st[14]) / hz ))
	rss=$(awk '/VmRSS/{print $2}' /proc/$server/status)
	echo "$t,$live,$peak,$failed,$cpu,$rss" >> "$out/mem.csv"
done

kill "${bots[@]}" 2>/dev/null
kill -TERM $server 2>/dev/null
for i in $(seq 20); do kill -0 $server 2>/dev/null || break; sleep 0.5; done
kill -9 $server 2>/dev/null
wait 2>/dev/null

echo "joins: $(grep -c 'joins game' "$out/server.log")   walker: $(grep '\[walker\]' "$out/server.log" | tail -1 | sed 's/.*\[walker\] //')"
tail -1 "$out/mem.csv" | awk -F, '{printf "last sample: t=%ss live=%s KB peak=%s KB failed=%s cpu=%ss rss=%s KB\n", $1,$2,$3,$4,$5,$6}'
grep -h "\[memcap\]" "$out/server.out" | tail -1
grep -m3 -E "ERROR|out of memory|bad_alloc" "$out/server.log" "$out/server.out" | head -3
