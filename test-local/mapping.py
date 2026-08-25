"""
Simple mapping server for nginx-vod-module mapped mode.

Test endpoints (sine tones):
  GET /plain/           → serve tone_440hz.mp4 as-is
  GET /gain_half/       → 50% volume
  GET /key_up_3/        → +3 semitones
  GET /key_down_5/      → -5 semitones
  GET /speed_1_5x/      → 1.5x speed
  GET /key_and_speed/   → +4 semitones + 0.75x speed
  GET /mix_stems/       → mix two tones with different gains
  GET /full_mix/        → gain + key change + mix

Real song endpoints (One Last Breath - Arrocha Remix, 6 stems):
  GET /song_full/       → all stems mixed at full volume
  GET /song_vocals/     → vocals only
  GET /song_no_vocals/  → instrumental (all stems except vocals)
  GET /song_key_up_2/   → full mix shifted up 2 semitones
  GET /song_key_down_3/ → full mix shifted down 3 semitones
  GET /song_custom/     → vocals 100% + drums 80% + bass 60%, guitars muted, +2 key
"""

import json
from http.server import HTTPServer, BaseHTTPRequestHandler

MEDIA_DIR = "/web/content"
STEMS_DIR = f"{MEDIA_DIR}/stems"

def stem(name):
    return {"type": "source", "path": f"{STEMS_DIR}/{name}.m4a"}

# --- remote mode -------------------------------------------------------
# In remote mode the module builds the subrequest URI as
#   vod_remote_upstream_location + path
# (ngx_child_http_request.c:699-707), so `path` must NOT repeat the
# location prefix. /origin_proxy/ strips itself before hitting
# origin_server.py, which serves these straight out of /web/content.
def rstem(name):
    return {"type": "source", "path": f"/stems/{name}.m4a"}

ALL_STEMS = ["vocals", "bass", "drums", "guitars", "piano", "other"]
ALL_STEMS_L = ALL_STEMS

def with_gain(gain, source):
    return {"type": "gainFilter", "gain": gain, "source": source}

def with_key(semitones, source):
    return {"type": "keyChangeFilter", "semitones": semitones, "source": source}

def mix(*sources):
    return {"type": "mixFilter", "sources": list(sources)}

def seq(clip):
    return {"sequences": [{"clips": [clip]}]}

MAPPINGS = {
    # Plain passthrough — no filters
    "plain": {
        "sequences": [
            {"clips": [{"type": "source", "path": f"{MEDIA_DIR}/tone_440hz.mp4"}]}
        ]
    },

    # Gain filter — 50% volume
    "gain_half": {
        "sequences": [
            {
                "clips": [
                    {
                        "type": "gainFilter",
                        "gain": 0.5,
                        "source": {"type": "source", "path": f"{MEDIA_DIR}/tone_440hz.mp4"},
                    }
                ]
            }
        ]
    },

    # Key change — up 3 semitones
    "key_up_3": {
        "sequences": [
            {
                "clips": [
                    {
                        "type": "keyChangeFilter",
                        "semitones": 3,
                        "source": {"type": "source", "path": f"{MEDIA_DIR}/tone_440hz.mp4"},
                    }
                ]
            }
        ]
    },

    # Key change — down 5 semitones
    "key_down_5": {
        "sequences": [
            {
                "clips": [
                    {
                        "type": "keyChangeFilter",
                        "semitones": -5,
                        "source": {"type": "source", "path": f"{MEDIA_DIR}/tone_440hz.mp4"},
                    }
                ]
            }
        ]
    },

    # Speed change — 1.5x
    "speed_1_5x": {
        "sequences": [
            {
                "clips": [
                    {
                        "type": "rateFilter",
                        "rate": 1.5,
                        "source": {"type": "source", "path": f"{MEDIA_DIR}/tone_440hz.mp4"},
                    }
                ]
            }
        ]
    },

    # Key change + speed change — up 4 semitones, 0.75x speed
    "key_and_speed": {
        "sequences": [
            {
                "clips": [
                    {
                        "type": "rateFilter",
                        "rate": 0.75,
                        "source": {
                            "type": "keyChangeFilter",
                            "semitones": 4,
                            "source": {"type": "source", "path": f"{MEDIA_DIR}/tone_440hz.mp4"},
                        },
                    }
                ]
            }
        ]
    },

    # Mix two stems — 440Hz at full volume + 330Hz at 30% volume
    "mix_stems": {
        "sequences": [
            {
                "clips": [
                    {
                        "type": "mixFilter",
                        "sources": [
                            {
                                "type": "gainFilter",
                                "gain": 1.0,
                                "source": {"type": "source", "path": f"{MEDIA_DIR}/tone_440hz.mp4"},
                            },
                            {
                                "type": "gainFilter",
                                "gain": 0.3,
                                "source": {"type": "source", "path": f"{MEDIA_DIR}/tone_330hz.mp4"},
                            },
                        ],
                    }
                ]
            }
        ]
    },

    # Full use case: mix stems with gain + key change
    # 440Hz at 80% volume shifted up 2 semitones + 330Hz at 50% volume (no key change)
    "full_mix": {
        "sequences": [
            {
                "clips": [
                    {
                        "type": "mixFilter",
                        "sources": [
                            {
                                "type": "keyChangeFilter",
                                "semitones": 2,
                                "source": {
                                    "type": "gainFilter",
                                    "gain": 0.8,
                                    "source": {"type": "source", "path": f"{MEDIA_DIR}/tone_440hz.mp4"},
                                },
                            },
                            {
                                "type": "gainFilter",
                                "gain": 0.5,
                                "source": {"type": "source", "path": f"{MEDIA_DIR}/tone_330hz.mp4"},
                            },
                        ],
                    }
                ]
            }
        ]
    },

    # ===================================================================
    # Real song: "One Last Breath (Arrocha Remix)" — 6 stems
    # ===================================================================

    # All stems at full volume
    "song_full": seq(mix(
        stem("vocals"), stem("bass"), stem("drums"),
        stem("guitars"), stem("piano"), stem("other"),
    )),

    # Vocals only
    "song_vocals": seq(stem("vocals")),

    # ---- remote-mode twins (HTTP reader; see origin_server.py) --------
    # r_song_full is THE benchmark fixture: 6 sources => 6 serial reads
    # today, ~1 wave once reads go concurrent.
    "r_song_full": seq(mix(*[rstem(n) for n in ALL_STEMS])),
    "r_song_vocals": seq(rstem("vocals")),
    "r_song_key_up_2": seq(with_key(2, mix(*[rstem(n) for n in ALL_STEMS]))),

    # Instrumental — all stems except vocals
    "song_no_vocals": seq(mix(
        stem("bass"), stem("drums"), stem("guitars"),
        stem("piano"), stem("other"),
    )),

    # Full mix shifted up 2 semitones
    "song_key_up_2": seq(mix(
        with_key(2, stem("vocals")),
        with_key(2, stem("bass")),
        with_key(2, stem("drums")),
        with_key(2, stem("guitars")),
        with_key(2, stem("piano")),
        with_key(2, stem("other")),
    )),

    # Full mix shifted down 3 semitones
    "song_key_down_3": seq(mix(
        with_key(-3, stem("vocals")),
        with_key(-3, stem("bass")),
        with_key(-3, stem("drums")),
        with_key(-3, stem("guitars")),
        with_key(-3, stem("piano")),
        with_key(-3, stem("other")),
    )),

    # Custom practice mix: vocals 100%, drums 80%, bass 60%, guitars muted, +2 key
    "song_custom": seq(mix(
        with_key(2, with_gain(1.0, stem("vocals"))),
        with_key(2, with_gain(0.8, stem("drums"))),
        with_key(2, with_gain(0.6, stem("bass"))),
        # guitars: muted (not included)
        with_key(2, with_gain(0.3, stem("piano"))),
        with_key(2, with_gain(0.3, stem("other"))),
    )),

    # ===================================================================
    # Multi-track MP4 tests — video + all stems in one file
    # Used to verify A/V sync and mixFilter from same-file sources.
    # Track order: a1=guitars, a2=bass, a3=drums, a4=vocals, a5=piano, a6=other
    # ===================================================================

    # Individual tracks from multi-track MP4 — to verify track selection
    "mt_a1": seq({"type": "source", "path": f"{MEDIA_DIR}/multitrack.mp4", "tracks": "a1"}),
    "mt_a2": seq({"type": "source", "path": f"{MEDIA_DIR}/multitrack.mp4", "tracks": "a2"}),
    "mt_a3": seq({"type": "source", "path": f"{MEDIA_DIR}/multitrack.mp4", "tracks": "a3"}),
    "mt_a4": seq({"type": "source", "path": f"{MEDIA_DIR}/multitrack.mp4", "tracks": "a4"}),
    "mt_a5": seq({"type": "source", "path": f"{MEDIA_DIR}/multitrack.mp4", "tracks": "a5"}),
    "mt_a6": seq({"type": "source", "path": f"{MEDIA_DIR}/multitrack.mp4", "tracks": "a6"}),

    # Audio-only mix from multi-track MP4 (no video, mixing tracks from same file)
    "mt_audio_only": {
        "sequences": [{
            "clips": [{
                "type": "mixFilter",
                "sources": [
                    {"type": "source", "path": f"{MEDIA_DIR}/multitrack.mp4", "tracks": f"a{i}"}
                    for i in range(1, 7)
                ],
            }],
        }],
    },

    # Mix guitars (a1) + vocals (a4) — both with real audio content, so result
    # should clearly sound like guitars+vocals, not just guitars.
    "mt_guitars_plus_vocals": {
        "sequences": [{
            "clips": [{
                "type": "mixFilter",
                "sources": [
                    {"type": "source", "path": f"{MEDIA_DIR}/multitrack.mp4", "tracks": "a1"},
                    {"type": "source", "path": f"{MEDIA_DIR}/multitrack.mp4", "tracks": "a4"},
                ],
            }],
        }],
    },

    # Video + audio mix from multi-track MP4
    # Sparse-GOP video (keyframes every 10s) + 6-stem audio mix. Reproduces the
    # production shape: source GOP much longer than vod_segment_duration, which
    # is what splits video and audio onto different segment grids.
    "sparse_muxed": {
        "sequences": [
            {"clips": [{"type": "source", "path": f"{MEDIA_DIR}/video_sparse.mp4"}]},
            # "default": True marks this the default audio rendition. Without it
            # the module emits AUTOSELECT=NO,DEFAULT=NO for any adaptation set
            # that is not the first (m3u8_builder.c ~:889), and video is first —
            # so a spec-following player would render video with no audio.
            {"clips": [mix(*[stem(n) for n in ["vocals","bass","drums","guitars","piano","other"]])],
             "default": True, "label": "Mix", "language": "eng"},
        ],
    },

    # ---- tempo / speed-shift experiments -------------------------------
    # Baseline: same sources as sparse_muxed, plain VOD, rate 1.0.
    "tempo_base": {
        "sequences": [
            {"clips": [{"type": "source", "path": f"{MEDIA_DIR}/video_sparse.mp4"}]},
            {"clips": [mix(*[stem(n) for n in ALL_STEMS_L])],
             "default": True, "label": "Mix"},
        ],
    },

    # rateFilter on both renditions, VOD. Answers: does atempo hold pitch, does
    # video retime without re-encode, and — the new risk after unmuxing — does
    # every video segment still START ON A KEYFRAME once timestamps are rescaled?
    "tempo_125": {
        "sequences": [
            {"clips": [{"type": "rateFilter", "rate": 1.25,
                        "source": {"type": "source", "path": f"{MEDIA_DIR}/video_sparse.mp4"}}]},
            {"clips": [{"type": "rateFilter", "rate": 1.25,
                        "source": mix(*[stem(n) for n in ALL_STEMS_L])}],
             "default": True, "label": "Mix"},
        ],
    },

    # EVENT playlist, rate 1.0. Answers: does the module emit
    # EXT-X-PLAYLIST-TYPE:EVENT and omit EXT-X-ENDLIST, so a player keeps
    # polling the SAME url? That is the whole basis for changing tempo without
    # handing the client a new stream URL.
    "tempo_event": {
        "playlistType": "event",
        "liveWindowDuration": -1,
        "sequences": [
            {"clips": [{"type": "source", "path": f"{MEDIA_DIR}/video_sparse.mp4"}]},
            {"clips": [mix(*[stem(n) for n in ALL_STEMS_L])],
             "default": True, "label": "Mix"},
        ],
    },

    # minimal event: single audio sequence, nothing else, to isolate whether
    # playlistType is honoured at all
    "ev_min": {
        "playlistType": "event",
        "sequences": [{"clips": [stem("vocals")]}],
    },

    # tempo compensation: pitch-shift the whole mix by +551 cents so that a
    # client playing at 80/110 = 0.7273x (pitch correction OFF) lands back on
    # the original pitch. 551 cents is what 110->80 BPM requires.
    "comp_551": {
        "sequences": [{"clips": [
            {"type": "keyChangeFilter", "cents": 551,
             "source": mix(*[stem(n) for n in ALL_STEMS_L])}
        ]}],
    },
    # same shift expressed the old way, for A/B: 500 cents == 5 semitones
    "comp_500": {
        "sequences": [{"clips": [
            {"type": "keyChangeFilter", "cents": 500,
             "source": mix(*[stem(n) for n in ALL_STEMS_L])}
        ]}],
    },
    "semi_5": {
        "sequences": [{"clips": [
            {"type": "keyChangeFilter", "semitones": 5,
             "source": mix(*[stem(n) for n in ALL_STEMS_L])}
        ]}],
    },

    # 440Hz tone, pitch-shifted by cents — used to verify the cents math
    # end to end (expected out = 440 * 2^(cents/1200)).
    "tone_0":   {"sequences": [{"clips": [{"type": "source", "path": f"{MEDIA_DIR}/tone_440hz.mp4"}]}]},
    "tone_551": {"sequences": [{"clips": [{"type": "keyChangeFilter", "cents": 551,
                  "source": {"type": "source", "path": f"{MEDIA_DIR}/tone_440hz.mp4"}}]}]},
    "tone_1200":{"sequences": [{"clips": [{"type": "keyChangeFilter", "cents": 1200,
                  "source": {"type": "source", "path": f"{MEDIA_DIR}/tone_440hz.mp4"}}]}]},

    "mt_full": {
        "sequences": [
            {
                "clips": [{
                    "type": "source", "path": f"{MEDIA_DIR}/multitrack.mp4", "tracks": "v1",
                }],
            },
            {
                "clips": [{
                    "type": "mixFilter",
                    "sources": [
                        {"type": "source", "path": f"{MEDIA_DIR}/multitrack.mp4", "tracks": f"a{i}"}
                        for i in range(1, 7)
                    ],
                }],
            },
        ],
    },
}


class MappingHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        # nginx-vod-module sends the full URI path (minus HLS filename):
        #   /mapped/hls/plain  or  /mapped/hls/gain_half
        # We need the last path component as the mapping key
        parts = [p for p in self.path.strip("/").split("/") if p]
        key = parts[-1] if parts else ""

        if key in MAPPINGS:
            body = json.dumps(MAPPINGS[key]).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            msg = f"Unknown mapping: {key}\nAvailable: {', '.join(MAPPINGS.keys())}\n"
            body = msg.encode()
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def log_message(self, format, *args):
        print(f"[mapping] {args[0]}")


if __name__ == "__main__":
    server = HTTPServer(("0.0.0.0", 8888), MappingHandler)
    print("[mapping] Mapping server listening on :8888")
    print(f"[mapping] Available mappings: {', '.join(MAPPINGS.keys())}")
    server.serve_forever()
