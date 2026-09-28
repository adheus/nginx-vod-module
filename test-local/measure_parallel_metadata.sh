#!/usr/bin/env bash
# measure_parallel_metadata.sh — A/B of vod_parallel_metadata_reads on the
# test-local container: /pm-off/hls/ (serial baseline) vs /pm-on/hls/ (wave),
# same build, same fixtures. Requires the container to run with a synthetic
# origin TTFB (ORIGIN_DELAY_MS) - without it serial and parallel are
# indistinguishable on loopback.
#
# Usage:
#   test-local/measure_parallel_metadata.sh timing [-n RUNS] FIXTURE...
#       cold master.m3u8 wall time per location, RUNS runs each, every run a
#       fresh cb<N>_ prefix (cold metadata cache), plus the origin's
#       peak_concurrent and request count for that run.
#   test-local/measure_parallel_metadata.sh identical FIXTURE...
#       for both locations: wipe the FFSA blobs, cold master, every index
#       playlist, the first 3 segments of every rendition; print sha256 of
#       each and whether the two locations match byte for byte.
#
# Options: -b BASE (default http://localhost:8082), -c CONTAINER (default vod-pmeta),
#          -l "LOCS" for timing (default "pm-off pm-on pm-on8")
set -uo pipefail

BASE="http://localhost:8082"
CONTAINER="vod-pmeta"
RUNS=5
LOCS="pm-off pm-on pm-on8"
MODE=${1:?mode: timing|identical}; shift
while getopts "b:c:n:l:" opt; do
	case $opt in
		b) BASE=$OPTARG ;;
		l) LOCS=$OPTARG ;;
		c) CONTAINER=$OPTARG ;;
		n) RUNS=$OPTARG ;;
		*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || { echo "no fixture given" >&2; exit 2; }

# monotonically unique cache-busting ids across invocations
STAMP_FILE=/tmp/pm_cb_counter
next_cb() {
	local n
	n=$(( $(cat "$STAMP_FILE" 2>/dev/null || echo $(( $(date +%s) % 100000 * 10 ))) + 1 ))
	echo "$n" > "$STAMP_FILE"
	echo "$n"
}

origin() {  # __stats__ | __reset__
	docker exec "$CONTAINER" python3 -c "import urllib.request;print(urllib.request.urlopen('http://127.0.0.1:8890/$1').read().decode())"
}

wipe_state() {
	docker exec "$CONTAINER" sh -c 'rm -rf /tmp/state-blobs/* 2>/dev/null; mkdir -p /tmp/state-blobs' >/dev/null
}

case $MODE in
timing)
	printf '%-16s %-7s %-4s %-8s %-6s %-16s %-9s\n' fixture loc run seconds http peak_concurrent requests
	for fx in "$@"; do
		for loc in $LOCS; do
			for i in $(seq 1 "$RUNS"); do
				n=$(next_cb)
				origin __reset__ >/dev/null
				out=$(curl -s -o /dev/null -w '%{time_total} %{http_code}' "$BASE/$loc/hls/cb${n}_${fx}/master.m3u8")
				stats=$(origin __stats__)
				peak=$(echo "$stats" | sed -E 's/.*"peak_concurrent":([0-9]+).*/\1/')
				reqs=$(echo "$stats" | sed -E 's/.*"requests":([0-9]+).*/\1/')
				printf '%-16s %-7s %-4s %-8s %-6s %-16s %-9s\n' "$fx" "$loc" "$i" "${out% *}" "${out#* }" "$peak" "$reqs"
			done
		done
	done
	;;
identical)
	# playlists carry absolute URLs (vod_hls_absolute_*_urls default on), so they
	# embed the location: hash them with /pm-<x>/ normalised, follow their URLs as-is
	tmp=$(mktemp -d)
	norm() { sed -E 's#/pm-(on|off|on8)/#/pm-X/#g' "$1"; }
	fetch() {  # fetch <base> <ref> <out>: ref may be absolute or relative to base
		case $2 in http*) curl -sf "$2" -o "$3" ;; *) curl -sf "$1/$2" -o "$3" ;; esac
	}
	for fx in "$@"; do
		n=$(next_cb)
		for loc in pm-off pm-on; do
			wipe_state
			dir="$tmp/$fx/$loc"; mkdir -p "$dir"
			base="$BASE/$loc/hls/cb${n}_${fx}"
			curl -sf "$base/master.m3u8" -o "$dir/master.m3u8" || { echo "$fx $loc: master failed" >&2; continue; }
			norm "$dir/master.m3u8" > "$dir/master.m3u8.norm"
			# every playlist referenced by the master (variants and EXT-X-MEDIA URIs)
			pls=$(grep -oE '[A-Za-z0-9_./:-]*index[A-Za-z0-9_-]*\.m3u8' "$dir/master.m3u8" | sort -u)
			for pl in $pls; do
				name=${pl##*/}
				fetch "$base" "$pl" "$dir/$name" || { echo "$fx $loc: $name failed" >&2; continue; }
				norm "$dir/$name" > "$dir/$name.norm"
				segs=$(grep -v '^#' "$dir/$name" | grep . | sed -n '1,3p')
				for seg in $segs; do
					fetch "$base" "$seg" "$dir/${seg##*/}" || echo "$fx $loc: ${seg##*/} failed" >&2
				done
			done
			( cd "$dir" && find . -type f -name '*.norm' -o -type f -name '*.ts' -o -type f -name '*.m4s' -o -type f -name '*.mp4' | sort | xargs shasum -a 256 ) > "$tmp/$fx/$loc.sha"
		done
		echo "== $fx"
		sed 's/^/   /' "$tmp/$fx/pm-on.sha"
		if cmp -s "$tmp/$fx/pm-off.sha" "$tmp/$fx/pm-on.sha"; then
			echo "   IDENTICAL: $(wc -l < "$tmp/$fx/pm-on.sha" | tr -d ' ') files match byte for byte (pm-off == pm-on; playlists compared with the location path normalised)"
		else
			echo "   DIFFERENT:"; diff "$tmp/$fx/pm-off.sha" "$tmp/$fx/pm-on.sha" | sed 's/^/   /' || true
		fi
	done
	echo "artifacts in $tmp"
	;;
*)
	echo "unknown mode $MODE" >&2; exit 2 ;;
esac
