#!/usr/bin/env python3
"""
Range-capable origin server for exercising nginx-vod-module's REMOTE reader.

Why this exists
---------------
The rest of test-local serves media as local files (`vod_mode local`, or mapped
mode with on-disk paths). That path uses the FILE reader, which is synchronous
and per-source — so any work on concurrent HTTP source reads is *silently
inactive* under the existing harness: the code never runs, and the tests pass
anyway. That is the worst possible failure mode for a performance change.

This server plus the `/remote/hls/` location in nginx.conf reproduce the
production topology: nginx-vod-module fetches every source over HTTP via
`vod_remote_upstream_location`, one Range request per source per segment.

The delay is the point
----------------------
Production reads cost 120-735ms each (CDN time-to-first-byte, not bandwidth —
see docs/segment-latency-investigation.md in moises-stream-server). On loopback
a read costs ~0ms, so a serial pipeline and a concurrent one look identical and
the win is unmeasurable. ORIGIN_DELAY_MS injects a synthetic TTFB so that:

    serial     segment time ~= N_sources * delay
    concurrent segment time ~= ceil(N_sources / cap) * delay

With the default 200ms and the 6-stem song_full fixture, that is ~1.4s serial
versus ~0.4s at cap=6 — a difference no amount of run-to-run noise can hide.

Env:
  ORIGIN_PORT      (default 8890)
  ORIGIN_ROOT      (default /web/content)
  ORIGIN_DELAY_MS  (default 200)  per-request sleep BEFORE the first byte
  ORIGIN_LOG       (default 1)    log every range read

The delay is applied before headers so it models time-to-first-byte, matching
what `$upstream_header_time` measures against the real CDN.
"""

import os
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.environ.get("ORIGIN_ROOT", "/web/content")
PORT = int(os.environ.get("ORIGIN_PORT", "8890"))
DELAY_MS = int(os.environ.get("ORIGIN_DELAY_MS", "200"))
LOG = os.environ.get("ORIGIN_LOG", "1") == "1"

RANGE_RE = re.compile(r"^bytes=(\d*)-(\d*)$")

# Concurrency observability: if reads are serialised somewhere, peak stays 1
# no matter what the cap is set to. This is the harness's own check that the
# thing under test is actually happening.
_lock = threading.Lock()
_inflight = 0
_peak = 0
_count = 0


def _enter():
    global _inflight, _peak, _count
    with _lock:
        _inflight += 1
        _count += 1
        if _inflight > _peak:
            _peak = _inflight
        return _inflight


def _exit():
    global _inflight
    with _lock:
        _inflight -= 1


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _resolve(self):
        # Strip query, normalise, and confine to ROOT.
        path = self.path.split("?", 1)[0]
        full = os.path.normpath(os.path.join(ROOT, path.lstrip("/")))
        if not full.startswith(os.path.realpath(ROOT)) and not full.startswith(ROOT):
            return None
        return full if os.path.isfile(full) else None

    def do_GET(self):
        self._serve(body=True)

    def do_HEAD(self):
        self._serve(body=False)

    def _serve(self, body):
        global _peak, _count

        # /__stats__ reports observed concurrency — see module docstring.
        if self.path.startswith("/__stats__"):
            with _lock:
                payload = f'{{"peak_concurrent":{_peak},"inflight":{_inflight},"requests":{_count}}}'.encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            if body:
                self.wfile.write(payload)
            return

        if self.path.startswith("/__reset__"):
            with _lock:
                _peak, _count = 0, 0
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        full = self._resolve()
        if full is None:
            self.send_error(404, "not found")
            return

        size = os.path.getsize(full)
        start, end = 0, size - 1
        partial = False

        rng = self.headers.get("Range")
        if rng:
            m = RANGE_RE.match(rng.strip())
            if not m:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            s, e = m.group(1), m.group(2)
            if s == "":
                # suffix range: last N bytes
                n = int(e)
                start, end = max(0, size - n), size - 1
            else:
                start = int(s)
                end = int(e) if e else size - 1
            if start >= size:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            end = min(end, size - 1)
            partial = True

        n_inflight = _enter()
        try:
            # Model time-to-first-byte, not bandwidth.
            if DELAY_MS:
                time.sleep(DELAY_MS / 1000.0)

            length = end - start + 1
            self.send_response(206 if partial else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(length))
            if partial:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()

            if body:
                with open(full, "rb") as f:
                    f.seek(start)
                    remaining = length
                    while remaining > 0:
                        chunk = f.read(min(65536, remaining))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        remaining -= len(chunk)

            if LOG:
                print(
                    f"[origin] {self.path.split('?')[0]} "
                    f"bytes={start}-{end}/{size} inflight={n_inflight} peak={_peak}",
                    flush=True,
                )
        finally:
            _exit()

    def log_message(self, fmt, *args):
        pass  # we do our own logging above


if __name__ == "__main__":
    if len(sys.argv) > 1:
        PORT = int(sys.argv[1])
    print(
        f"[origin] serving {ROOT} on :{PORT} delay={DELAY_MS}ms (threaded)",
        flush=True,
    )
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
