#!/usr/bin/env bash
# Measure nginx worker memory growth per audio segment request.
#
# Usage: test-local/measure_worker_rss.sh <image> [rounds] [playlist-path]
#   image          e.g. vod-test:leakfix
#   rounds         passes over every segment of the playlist (default 3)
#   playlist-path  default /mapped/hls/song_full/index.m3u8 (6-stem mix)
#
# Runs the image, requests every audio segment `rounds` times (the response
# cache is off in test-local, so each request runs the full decode -> filter
# -> encode pipeline), and prints the summed RSS of all nginx workers before,
# after the first pass (warm-up: metadata cache, allocator arenas) and at the
# end. Growth after warm-up divided by requests is the per-segment leak.
set -euo pipefail

IMAGE=${1:?image}
ROUNDS=${2:-3}
PLAYLIST=${3:-/mapped/hls/song_full/index.m3u8}
NAME=rss-probe-$$
PORT=18080

docker run -d --rm --name "$NAME" -p "$PORT:8080" "$IMAGE" >/dev/null
trap 'docker stop "$NAME" >/dev/null 2>&1 || true' EXIT
for _ in $(seq 1 30); do
	curl -sf -o /dev/null "http://localhost:$PORT$PLAYLIST" && break
	sleep 1
done

worker_rss_kb() {
	docker exec "$NAME" sh -c '
		total=0
		for s in /proc/[0-9]*/status; do
			grep -q "^Name:.*nginx" "$s" 2>/dev/null || continue
			pid=${s#/proc/}; pid=${pid%/status}
			grep -q "worker process" "/proc/$pid/cmdline" 2>/dev/null || continue
			rss=$(awk "/^VmRSS:/{print \$2}" "$s")
			total=$((total + rss))
		done
		echo $total'
}

base_url="http://localhost:$PORT${PLAYLIST%/*}"
segments=$(curl -sf "http://localhost:$PORT$PLAYLIST" | grep -v '^#' | grep .)
count=$(echo "$segments" | wc -l | tr -d ' ')

pass() {
	local failed=0
	while read -r seg; do
		code=$(curl -s -o /dev/null -w '%{http_code}' "$base_url/$seg")
		[ "$code" = 200 ] || failed=$((failed + 1))
	done <<< "$segments"
	echo "$failed"
}

rss0=$(worker_rss_kb)
f=$(pass)
rss1=$(worker_rss_kb)
failed=$f
for _ in $(seq 2 "$ROUNDS"); do
	f=$(pass)
	failed=$((failed + f))
done
rss2=$(worker_rss_kb)

reqs=$((count * (ROUNDS - 1)))
echo "image=$IMAGE segments=$count rounds=$ROUNDS non-200=$failed"
echo "worker RSS: start=$((rss0 / 1024))MB after-warmup=$((rss1 / 1024))MB end=$((rss2 / 1024))MB"
echo "growth after warm-up: $(((rss2 - rss1) / 1024))MB over $reqs requests = $(((rss2 - rss1) / (reqs > 0 ? reqs : 1)))KB/request"
