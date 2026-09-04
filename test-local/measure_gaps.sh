#!/usr/bin/env bash
# measure_gaps.sh — per-segment AAC frame count + inter-segment PTS gap for an
# HLS audio rendition served by the test-local container.
#
# This is the fix-validation tool for the "keyChangeFilter loses 2-3 AAC frames
# at every segment boundary" defect. It fetches N consecutive audio segments IN
# ORDER (so the FFSA state chain is exercised exactly like a player would), runs
# ffprobe over each one and reports:
#
#   frames        AAC packets in the segment (expect 4.0 * 44100 / 1024 = 172.27)
#   first / last  pts_time of the first and last packet
#   gap_prev      first_pts(n) - (last_pts(n-1) + frame_dur), in ms.
#                 0 = perfectly contiguous; +23.2 = one frame missing at the
#                 boundary; negative = overlap.
#   lost          gap_prev expressed in frames
#
# and a summary with mean frames/segment, mean frames lost per boundary, the
# cumulative timeline gap over the run and the extrapolated drift per minute.
#
# Usage:
#   test-local/measure_gaps.sh [options] MAPPING
#
#   MAPPING   mapping key served by mapping.py. For the repro use the parametric
#             remote-reader fixtures:  r_kc_s0 (baseline), r_kc_s-1, r_kc_s1,
#             r_kc_c551 (cents / tempo comp), r_kcps_s-1 (per-stem shape).
#             Local-file twins drop the r_ prefix (use with -l /mapped/hls).
#
# Options:
#   -b BASE       server base URL            (default http://localhost:8081)
#   -l LOCATION   nginx location prefix      (default /prod/hls)
#                 /prod/hls          = production twin (remote reader + FFSA + unmuxed)
#                 /prod-nostate/hls  = same without the FFSA state hook
#                 /mapped/hls        = local file reader + FFSA (drop the r_ prefix)
#   -n N          number of consecutive segments to measure (default 8)
#   -c CONTAINER  container name; its /tmp/state-blobs is wiped before the run so
#                 the FFSA chain starts cold, like a fresh production session
#                 (default vod-repro; "" = do not touch the container)
#   -o OUTDIR     where to keep the downloaded segments (default: temp dir)
#   -r RATE       sample rate               (default 44100)
#   -f FRAME      samples per AAC frame     (default 1024)
#   -d SECONDS    nominal segment duration  (default 4.0)
#   -q            quiet: only print the summary lines
#   -h            help
#
# Exit status is 0 if the run completed; the numbers are the result, the script
# does not pass/fail on its own.

set -euo pipefail

BASE="http://localhost:8081"
LOCATION="/prod/hls"
N=8
CONTAINER="vod-repro"
OUTDIR=""
RATE=44100
FRAME=1024
SEGDUR=4.0
QUIET=0

usage() { sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while getopts "b:l:n:c:o:r:f:d:qh" opt; do
    case "$opt" in
        b) BASE="$OPTARG" ;;
        l) LOCATION="$OPTARG" ;;
        n) N="$OPTARG" ;;
        c) CONTAINER="$OPTARG" ;;
        o) OUTDIR="$OPTARG" ;;
        r) RATE="$OPTARG" ;;
        f) FRAME="$OPTARG" ;;
        d) SEGDUR="$OPTARG" ;;
        q) QUIET=1 ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || usage 1
MAPPING="$1"

for tool in curl ffprobe awk; do
    command -v "$tool" >/dev/null || { echo "missing: $tool" >&2; exit 2; }
done

if [ -z "$OUTDIR" ]; then
    OUTDIR=$(mktemp -d "/tmp/measure_gaps.${MAPPING}.XXXX")
else
    mkdir -p "$OUTDIR"
fi

LOCATION="${LOCATION%/}"
PREFIX="$BASE$LOCATION/$MAPPING"

# Resolve a playlist entry against the URL it came from (absolute, root-relative
# or relative) — production emits relative URLs (vod_base_url ''), the harness
# emits absolute ones.
resolve() {
    local ref="$1" from="$2"
    case "$ref" in
        http://*|https://*) echo "$ref" ;;
        /*) echo "$BASE$ref" ;;
        *) echo "${from%/*}/$ref" ;;
    esac
}

# Cold FFSA chain: wipe the state server's blob dir so seg-1 has no
# predecessor and every boundary is produced by THIS run.
if [ -n "$CONTAINER" ]; then
    if ! docker exec "$CONTAINER" sh -c 'rm -f /tmp/state-blobs/*' 2>/dev/null; then
        echo "warn: could not wipe /tmp/state-blobs in container '$CONTAINER' (is it running?)" >&2
    fi
fi

master_url="$PREFIX/master.m3u8"
master=$(curl -fsS "$master_url") || { echo "failed to fetch $master_url" >&2; exit 3; }

# Audio rendition: prefer an EXT-X-MEDIA TYPE=AUDIO entry (unmuxed video+audio
# mappings), else the first variant playlist (audio-only mappings).
audio_ref=$(printf '%s\n' "$master" | awk -F'URI="' '/#EXT-X-MEDIA:.*TYPE=AUDIO/ { split($2, a, "\""); print a[1]; exit }')
if [ -z "$audio_ref" ]; then
    audio_ref=$(printf '%s\n' "$master" | grep -v '^#' | grep -v '^[[:space:]]*$' | head -1)
fi
[ -n "$audio_ref" ] || { echo "no audio playlist in $master_url:" >&2; printf '%s\n' "$master" >&2; exit 3; }
audio_url=$(resolve "$audio_ref" "$master_url")

media=$(curl -fsS "$audio_url") || { echo "failed to fetch $audio_url" >&2; exit 3; }
# (no mapfile: macOS ships bash 3.2)
seg_refs=()
while IFS= read -r line; do seg_refs+=("$line"); done < <(printf '%s\n' "$media" | grep -v '^#' | grep -v '^[[:space:]]*$' | head -n "$N")
[ "${#seg_refs[@]}" -gt 0 ] || { echo "no segments in $audio_url" >&2; exit 3; }
[ "${#seg_refs[@]}" -ge "$N" ] || echo "note: playlist only has ${#seg_refs[@]} segments, measuring those" >&2

if [ "$QUIET" -eq 0 ]; then
    echo "mapping   : $MAPPING"
    echo "location  : $LOCATION   base: $BASE"
    echo "playlist  : $audio_url"
    echo "segments  : ${#seg_refs[@]}   outdir: $OUTDIR"
    echo "expected  : $(awk -v d="$SEGDUR" -v r="$RATE" -v f="$FRAME" 'BEGIN{printf "%.2f", d*r/f}') frames/seg, frame_dur $(awk -v r="$RATE" -v f="$FRAME" 'BEGIN{printf "%.4f", 1000*f/r}') ms"
    echo
    printf '%-4s %-7s %-11s %-11s %-9s %-13s %-8s %-8s\n' seg frames first_pts last_pts dur_s gap_prev_ms lost_fr wall_s
fi

data="$OUTDIR/frames.tsv"
: > "$data"

i=0
for ref in "${seg_refs[@]}"; do
    i=$((i + 1))
    url=$(resolve "$ref" "$audio_url")
    file="$OUTDIR/seg-$i.ts"
    wall=$(curl -fsS -o "$file" -w '%{time_total}' "$url") || { echo "seg $i: fetch failed: $url" >&2; exit 4; }
    # ffprobe csv=p=0 prints "0.101000," per packet; strip the delimiter.
    stats=$(ffprobe -v error -select_streams a:0 -show_entries packet=pts_time -of csv=p=0 "$file" \
        | tr -d ',' | awk 'NR==1{f=$1} {l=$1; n++} END{printf "%d %s %s", n, f, l}')
    printf '%d\t%s\t%s\n' "$i" "$stats" "$wall" | tr ' ' '\t' >> "$data"
done

awk -v rate="$RATE" -v frame="$FRAME" -v segdur="$SEGDUR" -v quiet="$QUIET" -v mapping="$MAPPING" '
BEGIN { fd = frame / rate; expected = segdur * rate / frame; prev_last = ""; nb = 0; nb2 = 0 }
{
    seg = $1; frames = $2; first = $3; last = $4; wall = $5
    dur = last - first + fd
    gap_ms = ""; lost = ""
    if (prev_last != "") {
        gap = first - (prev_last + fd)
        gap_ms = gap * 1000; lost = gap / fd
        nb++; sum_gap += gap; sum_lost += lost
        if (nb >= 2) { nb2++; sum_gap2 += gap; sum_lost2 += lost }
    }
    nseg++; sum_frames += frames
    if (!quiet) {
        if (gap_ms == "") printf "%-4d %-7d %-11.6f %-11.6f %-9.4f %-13s %-8s %-8.3f\n", seg, frames, first, last, dur, "-", "-", wall
        else              printf "%-4d %-7d %-11.6f %-11.6f %-9.4f %-+13.2f %-+8.2f %-8.3f\n", seg, frames, first, last, dur, gap_ms, lost, wall
    }
    prev_last = last
}
END {
    if (nseg == 0) exit 1
    mean_frames = sum_frames / nseg
    printf "\n"
    printf "SUMMARY %s: segments=%d mean_frames=%.2f (expected %.2f, deficit %.2f/seg)\n", mapping, nseg, mean_frames, expected, expected - mean_frames
    if (nb > 0)
        printf "SUMMARY %s: boundaries=%d mean_gap=%.2f ms (%.2f frames lost/boundary), cumulative=%.1f ms, drift=%.0f ms/min\n",
            mapping, nb, sum_gap * 1000 / nb, sum_lost / nb, sum_gap * 1000, (sum_gap / nb) * 1000 * (60 / segdur)
    if (nb2 > 0)
        printf "SUMMARY %s: excluding seg1->2 boundary: mean_gap=%.2f ms (%.2f frames lost/boundary)\n",
            mapping, sum_gap2 * 1000 / nb2, sum_lost2 / nb2
}' "$data"
