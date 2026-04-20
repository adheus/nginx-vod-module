#!/usr/bin/env python3
"""
FFSA blob shuttle upstream for testing the nginx-vod-module
vod_encoder_state_location hook.

Regular routes:
  GET  /state/<key>  -> 200 body=blob, or 404 if no blob stored
  POST /state/<key>  -> writes request body to /tmp/state-blobs/<key>

Cold-start warmup (new):
  When a GET comes in with state_end_seg_index = UINT32_MAX (4294967295) —
  the sentinel used by nginx-vod-module for seg-1's "there is no predecessor"
  GET — the server tries to locate a pre-computed "silent-warmed" AAC encoder
  state blob and return THAT instead of 404. The next encoder hydrates with a
  warm-but-silent MDCT window, so seg-1 emits natural fade-in audio rather
  than ~46 ms of AAC priming zeros.

Warmup blobs live in $WARMUP_BLOB_DIR (default /warmup-blobs/) and are indexed
by the FFSA config_hash field (bytes [20..27] of the blob header). Because the
cold-start GET URI carries only the mapping digest — no codec config — we learn
"digest → config_hash" by snooping the first POST we see for each digest (seg-0's
POST, which arrives when the pipeline is healthy).

On cold-start hit we:
  1) look up config_hash from the digest map,
  2) find a warmup blob whose config_hash matches,
  3) return it.

If we haven't seen any POST for this digest yet, we fall through to 404
(existing behavior — nginx-vod-module then uses a fresh encoder exactly as
before, no regression).

Run: python3 state_server.py [port]
Default port: 8889
"""
import os
import sys
import logging
import struct
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote

BLOB_DIR = os.environ.get("STATE_BLOB_DIR", "/tmp/state-blobs")
WARMUP_DIR = os.environ.get("WARMUP_BLOB_DIR", "/warmup-blobs")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8889

# UINT32_MAX sentinel used by nginx-vod-module for seg-1 cold-start GET.
COLD_START_SENTINEL = 4294967295

os.makedirs(BLOB_DIR, exist_ok=True)
logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(message)s")
log = logging.getLogger("state-upstream")

# In-memory maps populated at startup and on every POST we observe.
# config_hash (u64) -> path to warmup blob file
_warmup_by_config_hash = {}
# digest (str) -> config_hash (int)
_config_hash_by_digest = {}


def _ffsa_config_hash(blob: bytes) -> int | None:
    """Extract the FFSA config_hash (bytes [20..27], u64 BE) from a blob.
    Returns None if the blob is too short or doesn't start with 'FFSA'."""
    if len(blob) < 36 or blob[:4] != b"FFSA":
        return None
    return struct.unpack(">Q", blob[20:28])[0]


def _set_ffsa_config_hash(blob: bytes, new_hash: int) -> bytes:
    """Return a copy of the blob with config_hash overwritten. The FFSA CRC
    is computed over the PAYLOAD only (bytes [36..36+payload_length]), not
    over the header — so the trailing CRC remains valid."""
    return blob[:20] + struct.pack(">Q", new_hash) + blob[28:]


def _load_warmup_blobs():
    """Scan WARMUP_DIR and index blobs by their config_hash."""
    _warmup_by_config_hash.clear()
    if not os.path.isdir(WARMUP_DIR):
        log.info("warmup: no directory at %s, warmup disabled", WARMUP_DIR)
        return
    n = 0
    for name in sorted(os.listdir(WARMUP_DIR)):
        path = os.path.join(WARMUP_DIR, name)
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "rb") as f:
                blob = f.read()
        except OSError as e:
            log.warning("warmup: cannot read %s: %s", path, e)
            continue
        ch = _ffsa_config_hash(blob)
        if ch is None:
            log.warning("warmup: %s is not a valid FFSA blob, skipping", name)
            continue
        # Keep first-seen for a given config_hash; log collisions.
        if ch in _warmup_by_config_hash:
            log.info("warmup: duplicate config_hash %016x — %s already mapped, ignoring %s",
                     ch, os.path.basename(_warmup_by_config_hash[ch]), name)
            continue
        _warmup_by_config_hash[ch] = path
        n += 1
        log.info("warmup: indexed %s -> config_hash=%016x (%d bytes)",
                 name, ch, len(blob))
    log.info("warmup: %d blob(s) indexed from %s", n, WARMUP_DIR)


def _parse_key(raw_path: str) -> tuple[str, dict]:
    """Return (flat_filename, parsed_fields).

    nginx routes cold-start through as /state/_state/<digest>/<seq>/<seg>/<track>
    (a double prefix because the nginx location is /_state and proxy_pass is
    http://.../state — the leading /_state is preserved as a path component).
    We accept both forms.

    parsed_fields contains: digest, seg_index, full_key (for logs). digest is
    None if the path doesn't match the expected shape.
    """
    p = unquote(raw_path.lstrip("/"))
    if p.startswith("state/"):
        p = p[len("state/"):]
    safe = p.replace("/", "__").replace("..", "")
    if not safe:
        safe = "__empty__"
    flat = os.path.join(BLOB_DIR, safe)

    # Try to parse the canonical shape: [_state/]<digest>/<seq>/<seg>/<track>
    parts = [x for x in p.split("/") if x]
    if parts and parts[0] == "_state":
        parts = parts[1:]
    fields = {"full_key": p, "digest": None, "seg_index": None}
    if len(parts) == 4:
        digest, seq_s, seg_s, trk_s = parts
        try:
            fields["digest"] = digest
            fields["seq_index"] = int(seq_s)
            fields["seg_index"] = int(seg_s)
            fields["track_index"] = int(trk_s)
        except ValueError:
            pass
    return flat, fields


def _lookup_warmup(digest: str) -> tuple[str, int] | None:
    """For the given mapping digest, return (warmup_blob_path, target_config_hash)
    if we've previously observed a POST for that digest. Returns None otherwise.

    Preference order:
      1) Exact config_hash match in the warmup store.
      2) Fallback to any available warmup blob — the server will rewrite the
         blob's config_hash field to match the target (the CRC is computed over
         payload only, so the header may be freely rewritten).

    (2) works because the FFSA atoms that depend on nb_channels / frame_size
    are sized by those fields only, not by the codec config hash. Our warmup
    blobs are stereo 44.1k (channel-count=2, 1024 frame_size) to match the
    test endpoints. For other channel counts, generate separate warmup blobs.
    """
    if digest is None:
        return None
    target_ch = _config_hash_by_digest.get(digest)
    if target_ch is not None:
        # Prefer exact match.
        if target_ch in _warmup_by_config_hash:
            return _warmup_by_config_hash[target_ch], target_ch
        # Learned digest but no matching hash — fall back to any warmup blob
        # and rewrite the hash to the learned target.
        if _warmup_by_config_hash:
            any_ch = min(_warmup_by_config_hash.keys())
            return _warmup_by_config_hash[any_ch], target_ch
        return None

    # No digest→config_hash learned yet. If we have exactly one warmup blob,
    # use its own config_hash (no rewrite needed). This covers the very-first
    # seg-1 cold-start on a fresh server. For multiple warmup blobs, we can't
    # pick blindly — fall through to 404 and wait for a POST to learn.
    if len(_warmup_by_config_hash) == 1:
        only_ch, only_path = next(iter(_warmup_by_config_hash.items()))
        return only_path, only_ch
    return None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # silence default noisy line
        pass

    def do_GET(self):
        flat, fields = _parse_key(self.path)
        # Normal path: a stored blob exists.
        if os.path.exists(flat):
            with open(flat, "rb") as f:
                blob = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(blob)))
            self.end_headers()
            self.wfile.write(blob)
            log.info("GET  %s -> 200 (%d bytes)", self.path, len(blob))
            return

        # Cold-start path: state_end_seg_index == UINT32_MAX.
        if fields.get("seg_index") == COLD_START_SENTINEL:
            hit = _lookup_warmup(fields.get("digest"))
            if hit is not None:
                path, ch = hit
                with open(path, "rb") as f:
                    blob = f.read()
                # The warmup blob was generated with a generic config_hash
                # that won't match the live encoder. Patch the header to the
                # observed-at-POST config_hash (learned from the very first
                # POST for this digest). CRC covers payload only, so no
                # recomputation is needed.
                patched = _set_ffsa_config_hash(blob, ch)
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(patched)))
                self.end_headers()
                self.wfile.write(patched)
                log.info("GET  %s -> 200 WARMUP (%s, config_hash=%016x, %d bytes)",
                         self.path, os.path.basename(path), ch, len(patched))
                return
            # No learned config_hash for this digest yet: fall through to 404.
            log.info("GET  %s -> 404 (cold-start, no warmup: digest=%s learned=%s)",
                     self.path, fields.get("digest"),
                     fields.get("digest") in _config_hash_by_digest)

        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()
        if fields.get("seg_index") != COLD_START_SENTINEL:
            log.info("GET  %s -> 404", self.path)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b""
        flat, fields = _parse_key(self.path)
        with open(flat, "wb") as f:
            f.write(body)

        # Learn digest -> config_hash from every POST we see.
        digest = fields.get("digest")
        ch = _ffsa_config_hash(body)
        learned_note = ""
        if digest and ch is not None:
            prev = _config_hash_by_digest.get(digest)
            if prev != ch:
                _config_hash_by_digest[digest] = ch
                warmup_path = _warmup_by_config_hash.get(ch)
                warmup_tag = os.path.basename(warmup_path) if warmup_path else "<none>"
                learned_note = f" [learned digest={digest} config_hash={ch:016x} warmup={warmup_tag}]"

        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()
        log.info("POST %s -> 204 (%d bytes stored)%s", self.path, len(body), learned_note)


def main():
    _load_warmup_blobs()
    with ThreadingHTTPServer(("0.0.0.0", PORT), Handler) as srv:
        log.info("state-upstream listening on :%d (blob dir=%s, warmup dir=%s)",
                 PORT, BLOB_DIR, WARMUP_DIR)
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            log.info("shutting down")


if __name__ == "__main__":
    main()
