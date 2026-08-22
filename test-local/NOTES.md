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

### 8. Known nginx-vod-module behavior: URL track mask defaults to `a1`

**THIS IS THE BIG ONE.** For non-master playlist URLs (e.g., `index-f1-v1-f2-a1.m3u8`), the default track mask is `0x1` (bit 0 = track a1 only). The URL pattern `-a1-v1` means "first track of each type."

This gets ANDed with the JSON mapping's `"tracks": "aN"` at:
```
ngx_http_vod_module.c:1554  vod_track_mask_and_bits(tracks_mask[media_type], cur_source->tracks_mask[media_type], request_tracks_mask[media_type]);
```

**Consequence:** When the JSON mapping's mixFilter has sources with `"tracks": "a2"`, `"a3"`, etc., they all get masked to 0 at segment-serving time. Only sources with `"tracks": "a1"` survive. The mix collapses to a single source (a1).

**Workaround options:**
- Use master playlist URL pattern (`master.m3u8`) — this uses `PARSE_FILE_NAME_MULTI_STREAMS_PER_TYPE` flag which defaults all bits to set
- Generate URLs with `-a0` (all audio tracks) — but this isn't what nginx naturally emits
- Fork fix: change `ngx_http_vod_parse_uri_file_name` in `ngx_http_vod_request_parse.c` so `default_tracks_mask` is always "all bits set" regardless of flag

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

## Remote mode — the ONLY path that exercises concurrent source reads

`/local/hls/` and `/mapped/hls/` read media as **files**. The file reader is
synchronous and binds its completion callback per *source* at open time, so any
change to the read scheduler is **silently inactive** there: the code never runs
and the tests pass anyway. Production uses the HTTP reader
(`vod_remote_upstream_location`), and so does `/remote/hls/`.

```bash
# 6-stem mix over HTTP, 200ms synthetic TTFB per read
curl -s -o /dev/null -w "%{time_total}\n" \
  http://localhost:8080/remote/hls/r_song_full/seg-1-a1.ts

# did reads actually overlap? peak_concurrent is the harness's own check
docker exec vod-test python3 -c \
  'import urllib.request;print(urllib.request.urlopen("http://127.0.0.1:8890/__stats__").read().decode())'
docker exec vod-test python3 -c \
  'import urllib.request;urllib.request.urlopen("http://127.0.0.1:8890/__reset__")'
```

Endpoints: `r_song_full` (6 stems — the benchmark), `r_song_vocals`,
`r_song_key_up_2`. Also `/remote-nostate/hls/` to isolate read-scheduler
behaviour from FFSA when bisecting.

`ORIGIN_DELAY_MS` (default 200) is the synthetic time-to-first-byte. Without it,
loopback reads cost ~0ms and serial vs concurrent are indistinguishable — the
delay is what makes the win measurable at all.

### Serial baseline, measured 2026-08-21 (before any concurrency work)

| metric | value |
|---|---|
| `seg-1/2/3` wall time | **1.37 / 1.33 / 1.31 s** |
| origin requests per segment | 6 (one per stem) |
| **`peak_concurrent`** | **1** |

1.3s ≈ 6 sources × 200ms — i.e. `N × delay`, the serial signature. After the
concurrency work at cap=6 this should approach ~1 wave (~0.2-0.4s) and
`peak_concurrent` should reach 6. **If `peak_concurrent` stays 1, the change is
not doing anything**, regardless of what wall time says.

### The correctness gate

Output must be **bit-identical** across serial and concurrent builds — frame
consumption order is independent of I/O completion order, so wrong-bytes-in-
wrong-slot cannot survive an md5 A/B.

```
seg-1  51ca19b62da96c281006cb4555149672
seg-2  2dea7f04bf1a59a68af214786b53e737
seg-3  c478d9af872736ecacad32eba2d96d42
```

Note seg-1 over HTTP has the **same md5 as the local-file twin**
(`/mapped/hls/song_full/seg-1-a1.ts`), so the two readers are also cross-checkable.
