# test-local — notes & gotchas

This directory provides an isolated test environment for the nginx-vod-module fork. Read this BEFORE adding debug code or rebuilds — it documents the pitfalls I hit repeatedly.

## Quick reference

```bash
# Build (always use --no-cache when testing code changes)
cd /Users/adheus/moises/nginx-vod-module
docker build --no-cache -t vod-test -f test-local/Dockerfile .

# Run
docker stop vod-test 2>/dev/null; docker rm vod-test 2>/dev/null
docker run -d --name vod-test -p 8080:8080 vod-test && sleep 3

# Test: audio-only mix
curl -s "http://localhost:8080/mapped/hls/song_full/index.m3u8"

# Test: video+audio multi-track
ffmpeg -y -i "http://localhost:8080/mapped/hls/mt_full/master.m3u8" -c copy /tmp/out.mp4

# Check logs
docker exec vod-test cat /var/log/nginx/error.log | tail -50
```

## Gotchas

### 1. Always use `--no-cache` when changing module source

The Dockerfile does `COPY . /tmp/nginx-vod-module` — Docker will cache this layer based on content. But changes to `vod/filters/*.c` might not always invalidate the layer on a regular `docker build`. Use `--no-cache` when you know you changed source and want a fresh build.

### 2. Response cache MUST be disabled to exercise the filter pipeline

In `nginx.conf`, `vod_response_cache` caches full segment responses. Repeat requests hit the cache and never re-execute the filter code — making all your debug logs and code changes invisible. Keep this OFF for dev.

```nginx
# vod caches — response cache OFF so we actually exercise the filter pipeline
vod_metadata_cache metadata_cache 128m;
# vod_response_cache disabled intentionally
```

### 3. nginx logs: `error_log /var/log/nginx/error.log debug`

Set in `test-local/nginx.conf`. Debug level is needed to see module-level debug messages. Access via:
```bash
docker exec vod-test cat /var/log/nginx/error.log
```

### 4. `docker logs vod-test` is EMPTY

The CMD daemonizes nginx and starts Python for the mapping server — neither go to stdout. All nginx output lands in `/var/log/nginx/error.log` inside the container. Don't waste time checking `docker logs`.

### 5. Debug logging from C code — use `vod_log_error(VOD_LOG_WARN, ...)`

Use `VOD_LOG_WARN` (maps to NGX_LOG_WARN = 6). It will show up in `/var/log/nginx/error.log` as long as `error_log` is set to `warn` or `debug`.

```c
vod_log_error(VOD_LOG_WARN, request_context->log, 0,
    "MY_DEBUG: value=%uD", some_value);
```

Format specifiers (nginx ngx_sprintf syntax, NOT printf):
- `%uD` = uint32_t
- `%uL` = uint64_t
- `%i` = ngx_int_t (signed)
- `%d` = int (don't use — use `%i`)
- `%V` = ngx_str_t*

### 6. `fprintf(stderr, ...)` from C code does NOT work

nginx redirects stderr on startup. Even with `-g "daemon off;"`, worker processes don't write to the docker stdout. Use `vod_log_error` instead.

### 7. Docker build context is the parent dir, not test-local/

The Dockerfile runs from the repo root, not `test-local/`. So:
- `COPY . /tmp/nginx-vod-module` copies the entire fork
- `COPY test-local/nginx.conf` references the file relative to build context
- Always run `docker build -f test-local/Dockerfile .` from the fork root

### 8. FIXED: multi-track MP4 mixFilter vs URL track mask

**Previous bug (fixed in commit e45e559):** For non-master playlist URLs (e.g., `seg-1-a1.ts`), the URL track mask is `0x1` (bit 0 = track a1 only). This was being ANDed with each filter source's JSON `"tracks": "aN"` spec at `ngx_http_vod_module.c:1554`, causing sources with `"tracks": "a2"` or higher to be masked to `0x0` and dropped. Result: the mix would collapse to just the `a1` source.

**Fix:** when a source is wrapped inside a filter (its parent clip is not itself a source), skip the intersection and use the JSON's `tracks_mask` directly. The URL's mask applies to the OUTPUT selection, not to filter sources.

**Regression test:** `/mapped/hls/mt_guitars_plus_vocals/master.m3u8` — download via ffmpeg, output must sound like both guitars and vocals mixed (RMS around -25 dB). If it sounds like just guitars (RMS -20 dB = a1 alone), the bug regressed.

### 9. Metadata cache can mask test results

`vod_metadata_cache` caches moov atom parsing keyed by file_key (MD5 of URI + host). If you're debugging "does the module see all tracks?", the cache may return stale parsed data. Always restart the container (or disable the cache) when testing track selection.

### 10. Test URLs

| Endpoint | Purpose |
|---|---|
| `/mapped/hls/song_full/index.m3u8` | Audio-only mix (6 stems from separate files) — reference implementation, always works |
| `/mapped/hls/mt_full/master.m3u8` | Video + mix from multi-track MP4 — the tricky case |
| `/mapped/hls/mt_a1/index.m3u8` through `mt_a6` | Individual tracks from multi-track MP4 — only a1 works unless URL fix applied |
| `/mapped/hls/mt_audio_only/index.m3u8` | Audio-only mix from multi-track MP4 — exhibits the tracks_mask bug |

## Known-working source files

- `media/multitrack.mp4` — 1 video + 6 audio tracks. Create with:
  ```bash
  ffmpeg -y \
    -i /path/to/video.mp4 \
    -i /path/to/stem_vocals.m4a \
    -i /path/to/stem_bass.m4a \
    -i /path/to/stem_drums.m4a \
    -i /path/to/stem_guitars.m4a \
    -i /path/to/stem_piano.m4a \
    -i /path/to/stem_other.m4a \
    -map 0:v:0 \
    -map 1:a:0 -map 2:a:0 -map 3:a:0 -map 4:a:0 -map 5:a:0 -map 6:a:0 \
    -c copy -movflags +faststart \
    test-local/media/multitrack.mp4
  ```

## A/V sync reality check

The "drift" measurement between video and audio PTS within a single segment is NOT playback drift. Video at 29.97fps and audio at 44100Hz AAC have different frame durations (~33.3ms vs ~23.2ms). They naturally split into different frame counts per segment. Players use PTS to sync independently — each track plays at its own PTS.

**To verify sync works:** download the full HLS stream via ffmpeg and play the resulting MP4. If video and audio are in sync in that file, the server output is correct. If a player (e.g., Shaka) seems to drift, it's a player-side buffering/tolerance issue, not a server-side sync issue.

```bash
ffmpeg -y -i "http://localhost:8080/mapped/hls/mt_full/master.m3u8" -c copy /tmp/out.mp4
open /tmp/out.mp4  # play it and check sync
```

## Directory layout

```
test-local/
  Dockerfile       # builds nginx + custom ffmpeg + vod module (our fork)
  nginx.conf       # server config (response cache OFF, debug log level)
  mapping.py       # HTTP mapping server (port 8888) — returns JSON for mapped-mode requests
  media/           # test audio/video files (gitignored except tones)
  output/          # downloaded test outputs (gitignored)
  test.sh          # smoke tests via curl
  NOTES.md         # this file
```
