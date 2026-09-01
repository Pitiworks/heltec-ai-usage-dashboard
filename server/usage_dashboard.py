#!/usr/bin/env python3
"""Usage dashboard: 5h and weekly limit bars for Claude and Codex.

Single file: background refresher + mini HTTP server (stdlib only).

- Codex: reads the most recently logged rate_limits from
  ~/.codex/sessions/**/rollout-*.jsonl (local, free).
- Claude: reads the existing OAuth accessToken READ-ONLY from
  ~/.claude/.credentials.json and makes a minimal /v1/messages call
  (max_tokens:1) every CLAUDE_POLL_SECONDS to read the
  anthropic-ratelimit-unified-5h/7d-* headers. The token is never
  modified or renewed. Each poll counts minimally against its own limit.

Start:  python3 usage_dashboard.py    ->    http://localhost:8791
"""
from __future__ import annotations

import glob
import json
import os
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HOME = Path.home()
CODEX_SESSIONS = HOME / ".codex" / "sessions"
CLAUDE_CREDS = HOME / ".claude" / ".credentials.json"

PORT = 8791
CODEX_POLL_SECONDS = 60
CLAUDE_POLL_SECONDS = 300   # each poll is one tiny real API call
CLAUDE_PROBE_MODEL = "claude-haiku-4-5-20251001"   # cheapest model for the header probe call
CLAUDE_STUCK_PCT = 99.0     # last known value already (near) full - see below

_state: dict = {"updated": 0, "claude": {"ok": False}, "codex": {"ok": False}}
_lock = threading.Lock()
_claude_cache: dict = {"ts": 0, "data": None}


# ── Codex (local) ────────────────────────────────────────────────────────────
def _find_rate_limits(obj):
    """Recursively search the event for a rate_limits dict (robust against structure changes)."""
    if isinstance(obj, dict):
        rl = obj.get("rate_limits")
        if isinstance(rl, dict):
            return rl
        for v in obj.values():
            r = _find_rate_limits(v)
            if r:
                return r
    return None


def _codex_window(w):
    """(used%, reset_unix) for one window; rolls an already-elapsed reset forward."""
    used = float(w.get("used_percent", 0) or 0)
    reset = int(w.get("resets_at", 0) or 0)
    wm = int(w.get("window_minutes", 0) or 0)
    now = time.time()
    if reset and reset < now and wm:
        used = 0.0                       # window has reset since this data point
        while reset < now:
            reset += wm * 60
    return {"used_pct": round(used, 1), "reset": reset}


def _valid_codex_rate_limits(rl):
    """Only limit_id=='codex' with real primary/secondary windows counts.

    Codex also occasionally logs other limit_id types (e.g. "premium")
    with primary/secondary == null. Those must not overwrite a
    previously seen valid Codex data point.
    """
    return (
        isinstance(rl, dict)
        and rl.get("limit_id") == "codex"
        and isinstance(rl.get("primary"), dict)
        and isinstance(rl.get("secondary"), dict)
    )


def read_codex():
    files = sorted(
        glob.glob(str(CODEX_SESSIONS / "**" / "rollout-*.jsonl"), recursive=True),
        key=os.path.getmtime, reverse=True,
    )
    for fp in files[:8]:
        try:
            last_rl = None
            with open(fp, "r") as fh:
                for line in fh:
                    if '"token_count"' not in line or '"rate_limits"' not in line:
                        continue
                    rl = _find_rate_limits(json.loads(line))
                    if _valid_codex_rate_limits(rl):
                        last_rl = rl   # most recent valid codex record wins
            if last_rl is None:
                continue
            return {
                "ok": True,
                "h5": _codex_window(last_rl["primary"]),
                "week": _codex_window(last_rl["secondary"]),
                "source_age": int(time.time() - os.path.getmtime(fp)),
            }
        except Exception:
            continue
    return {"ok": False, "error": "no Codex rate_limits found (have you used Codex yet?)"}


# Codex only logs rate_limits on a *successful* call. If the 5h window is
# actually exhausted, no fresh local data point ever arrives again - without
# this correction the display would stay stuck at the last known value
# (e.g. 99%) for the rest of the window instead of showing 100%.
CODEX_STUCK_PCT = 99.0     # last known value already (near) full
CODEX_STUCK_AFTER_S = 180  # ...and no new data point for a few poll cycles
CODEX_BACKOFF_S = 1800     # then only recheck every 30 min instead of every minute

_codex_backoff = {"until": 0.0, "cached": None}


def read_codex_with_backoff():
    """read_codex(), with presumption+backoff for the "limit actually full" case."""
    now = time.time()
    if now < _codex_backoff["until"]:
        return _codex_backoff["cached"]

    result = read_codex()

    if (result.get("ok") and result["h5"]["used_pct"] >= CODEX_STUCK_PCT
            and result.get("source_age", 0) >= CODEX_STUCK_AFTER_S):
        reset_at = result["h5"]["reset"] or (now + CODEX_BACKOFF_S)
        result = dict(result, h5=dict(result["h5"], used_pct=100.0), presumed_exhausted=True)
        _codex_backoff["until"] = min(now + CODEX_BACKOFF_S, reset_at)

    _codex_backoff["cached"] = result
    return result


# ── Claude (live, read-only) ─────────────────────────────────────────────────
def read_claude():
    try:
        creds = json.loads(CLAUDE_CREDS.read_text())["claudeAiOauth"]
    except Exception as e:
        return {"ok": False, "error": f"credentials not readable: {e}"}
    token = creds.get("accessToken")
    exp = creds.get("expiresAt", 0)
    if not token:
        return {"ok": False, "error": "no accessToken"}
    if exp and exp / 1000 < time.time():
        return {"ok": False, "error": "token expired - run Claude Code once"}

    body = json.dumps({
        "model": CLAUDE_PROBE_MODEL,
        "max_tokens": 1,
        "messages": [{"role": "user", "content": "hi"}],
    }).encode()
    req = urllib.request.Request(
        "https://api.anthropic.com/v1/messages",
        data=body,
        headers={
            "authorization": f"Bearer {token}",
            "anthropic-version": "2023-06-01",
            "anthropic-beta": "oauth-2025-04-20",
            "content-type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=25) as r:
            h = {k.lower(): v for k, v in r.headers.items()}
    except urllib.error.HTTPError as e:                # 429 etc. still carry the headers
        h = {k.lower(): v for k, v in (e.headers or {}).items()}
    except Exception as e:
        return {"ok": False, "error": f"API call failed: {e}"}

    if "anthropic-ratelimit-unified-5h-utilization" not in h:
        return {"ok": False, "error": "no unified-rate-limit headers in the response"}

    def f(key):
        try:
            return float(h.get(key, 0))
        except Exception:
            return 0.0

    return {
        "ok": True,
        "h5": {"used_pct": round(f("anthropic-ratelimit-unified-5h-utilization") * 100, 1),
               "reset": int(f("anthropic-ratelimit-unified-5h-reset"))},
        "week": {"used_pct": round(f("anthropic-ratelimit-unified-7d-utilization") * 100, 1),
                 "reset": int(f("anthropic-ratelimit-unified-7d-reset"))},
        "status": h.get("anthropic-ratelimit-unified-status", ""),
    }


def claude_fallback_on_probe_failure(cached_data, error):
    """Used when the live probe call fails (network, 429, token expired, ...).
    The probe call makes a real request on every poll - if it fails while the
    last known value was already (near) full, that's a strong signal for
    "actually exhausted by now", not just a network hiccup. Otherwise the
    display would stay stuck at the old (too low) value until the next
    successful poll - the same bug as Codex's, just triggered by a failed
    rather than a missing call.
    """
    if cached_data is None:
        return {"ok": False, "error": error}

    claude = dict(cached_data, stale=True, error=error)
    if claude["h5"]["used_pct"] >= CLAUDE_STUCK_PCT:
        claude = dict(claude, h5=dict(claude["h5"], used_pct=100.0), presumed_exhausted=True)
    if claude["week"]["used_pct"] >= CLAUDE_STUCK_PCT:
        claude = dict(claude, week=dict(claude["week"], used_pct=100.0), presumed_exhausted=True)
    return claude


# ── Refresher ────────────────────────────────────────────────────────────────
def refresh_loop():
    last_claude = 0.0
    poll_cost_pct: float | None = None
    while True:
        now = time.time()
        codex = read_codex_with_backoff()

        if _claude_cache["data"] is None or now - last_claude >= CLAUDE_POLL_SECONDS:
            prev_week_pct = _claude_cache["data"]["week"]["used_pct"] if _claude_cache["data"] else None
            cl = read_claude()
            last_claude = now
            if cl.get("ok"):
                if prev_week_pct is not None:
                    delta = cl["week"]["used_pct"] - prev_week_pct
                    if delta > 0:
                        poll_cost_pct = round(delta, 5)
                _claude_cache["data"] = cl
                _claude_cache["ts"] = now
                claude = cl
            else:
                print(f"[claude] probe failed: {cl.get('error')}", flush=True)
                claude = claude_fallback_on_probe_failure(_claude_cache["data"], cl.get("error"))
        else:
            claude = dict(_claude_cache["data"], stale=False)

        with _lock:
            _state.clear()
            _state.update({
                "updated": int(now),
                "claude": claude,
                "claude_age": int(now - _claude_cache["ts"]) if _claude_cache["ts"] else None,
                "poll_cost_week_pct": poll_cost_pct,
                "codex": codex,
            })
        time.sleep(CODEX_POLL_SECONDS)


# ── HTTP server ──────────────────────────────────────────────────────────────
INDEX = Path(__file__).parent / "index.html"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/state.json"):
            with _lock:
                payload = json.dumps(_state).encode()
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("access-control-allow-origin", "*")
            self.send_header("cache-control", "no-store")
            self.end_headers()
            self.wfile.write(payload)
        else:
            try:
                payload = INDEX.read_bytes()
                ctype = "text/html; charset=utf-8"
            except Exception:
                payload = b"index.html missing"
                ctype = "text/plain; charset=utf-8"
            self.send_response(200)
            self.send_header("content-type", ctype)
            self.end_headers()
            self.wfile.write(payload)


def main():
    threading.Thread(target=refresh_loop, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"Usage dashboard running on http://localhost:{PORT}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
