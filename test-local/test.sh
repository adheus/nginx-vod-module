#!/bin/bash
# Test script for nginx-vod-module with keyChangeFilter
# Run after: docker build -t vod-test . && docker run -p 8080:8080 vod-test

BASE="http://localhost:8080"
PASS=0
FAIL=0

test_endpoint() {
    local name="$1"
    local url="$2"
    local expect_status="${3:-200}"

    status=$(curl -s -o /dev/null -w "%{http_code}" "$url")
    if [ "$status" = "$expect_status" ]; then
        echo "  PASS  $name (HTTP $status)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name — expected $expect_status, got $status"
        FAIL=$((FAIL + 1))
    fi
}

test_hls_playable() {
    local name="$1"
    local url="$2"

    # Fetch master playlist
    playlist=$(curl -s "$url")
    if echo "$playlist" | grep -q "#EXTM3U"; then
        echo "  PASS  $name — valid HLS playlist"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name — invalid HLS playlist"
        FAIL=$((FAIL + 1))
        return
    fi

    # Try to fetch first segment
    seg_url=$(echo "$playlist" | grep -v "^#" | grep -v "^$" | head -1)
    if [ -n "$seg_url" ]; then
        base_url=$(dirname "$url")
        seg_status=$(curl -s -o /dev/null -w "%{http_code}" "$base_url/$seg_url")
        if [ "$seg_status" = "200" ]; then
            echo "  PASS  $name — first segment OK"
            PASS=$((PASS + 1))
        else
            echo "  FAIL  $name — segment HTTP $seg_status"
            FAIL=$((FAIL + 1))
        fi
    fi
}

download_and_check() {
    local name="$1"
    local url="$2"
    local output="$3"

    # Download all segments as TS, convert to WAV for analysis
    playlist=$(curl -s "$url")
    base_url=$(dirname "$url")

    # Get first segment only for quick test
    seg_url=$(echo "$playlist" | grep -v "^#" | grep -v "^$" | head -1)
    if [ -n "$seg_url" ]; then
        curl -s "$base_url/$seg_url" -o "/tmp/$output"
        size=$(wc -c < "/tmp/$output")
        if [ "$size" -gt 100 ]; then
            echo "  PASS  $name — segment downloaded ($size bytes)"
            PASS=$((PASS + 1))
        else
            echo "  FAIL  $name — segment too small ($size bytes)"
            FAIL=$((FAIL + 1))
        fi
    fi
}

echo ""
echo "=== nginx-vod-module keyChangeFilter tests ==="
echo ""

echo "--- Local mode (baseline) ---"
test_endpoint "health check" "$BASE/"
test_hls_playable "local HLS" "$BASE/local/hls/tone_440hz.mp4/index.m3u8"

echo ""
echo "--- Mapped mode: plain (no filters) ---"
test_hls_playable "plain passthrough" "$BASE/mapped/hls/plain/index.m3u8"

echo ""
echo "--- Mapped mode: gain filter ---"
test_hls_playable "gain 50%" "$BASE/mapped/hls/gain_half/index.m3u8"
download_and_check "gain segment" "$BASE/mapped/hls/gain_half/index.m3u8" "gain_seg.ts"

echo ""
echo "--- Mapped mode: key change filter ---"
test_hls_playable "key +3 semitones" "$BASE/mapped/hls/key_up_3/index.m3u8"
download_and_check "key +3 segment" "$BASE/mapped/hls/key_up_3/index.m3u8" "key_up_3_seg.ts"

test_hls_playable "key -5 semitones" "$BASE/mapped/hls/key_down_5/index.m3u8"
download_and_check "key -5 segment" "$BASE/mapped/hls/key_down_5/index.m3u8" "key_down_5_seg.ts"

echo ""
echo "--- Mapped mode: speed change ---"
test_hls_playable "speed 1.5x" "$BASE/mapped/hls/speed_1_5x/index.m3u8"

echo ""
echo "--- Mapped mode: key + speed combined ---"
test_hls_playable "key+speed" "$BASE/mapped/hls/key_and_speed/index.m3u8"

echo ""
echo "--- Mapped mode: stem mixing ---"
test_hls_playable "mix 2 stems" "$BASE/mapped/hls/mix_stems/index.m3u8"
download_and_check "mix segment" "$BASE/mapped/hls/mix_stems/index.m3u8" "mix_seg.ts"

echo ""
echo "--- Mapped mode: full mix (gain + key + mix) ---"
test_hls_playable "full mix" "$BASE/mapped/hls/full_mix/index.m3u8"
download_and_check "full mix segment" "$BASE/mapped/hls/full_mix/index.m3u8" "full_mix_seg.ts"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
echo ""

if [ "$FAIL" -gt 0 ]; then
    echo "Check nginx error log for details:"
    echo "  docker exec <container> cat /var/log/nginx/error.log"
    exit 1
fi
