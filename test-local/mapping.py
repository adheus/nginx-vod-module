"""
Simple mapping server for nginx-vod-module mapped mode.

Endpoints (as seen by nginx via /mapping_proxy/):
  GET /plain/           → serve tone_440hz.mp4 as-is
  GET /gain_half/       → serve tone_440hz.mp4 at 50% volume
  GET /key_up_3/        → serve tone_440hz.mp4 shifted up 3 semitones
  GET /key_down_5/      → serve tone_440hz.mp4 shifted down 5 semitones
  GET /speed_1_5x/      → serve tone_440hz.mp4 at 1.5x speed
  GET /key_and_speed/   → shift up 4 semitones + 0.75x speed
  GET /mix_stems/       → mix two tones with different gains (simulates stem mixing)
  GET /full_mix/        → mix two tones with gain + key change (the full use case)
"""

import json
from http.server import HTTPServer, BaseHTTPRequestHandler

MEDIA_DIR = "/web/content"

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
