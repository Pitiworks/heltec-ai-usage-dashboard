"""Regressionstest: ein trailing limit_id!="codex"-Eintrag mit primary/secondary==null
darf die zuletzt gesehenen echten Codex-Werte nicht auf 0%/reset 0 überschreiben."""
import importlib
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import usage_dashboard as ud  # noqa: E402


def _event(rate_limits):
    return {"type": "event", "payload": {"token_count": True, "rate_limits": rate_limits}}


def _write_session(tmp_path, lines):
    fp = tmp_path / "rollout-2026-08-28T00-00-00-test.jsonl"
    fp.write_text("\n".join(json.dumps(_event(rl)) for rl in lines) + "\n")
    return fp


def test_trailing_premium_null_does_not_zero_out_codex(tmp_path, monkeypatch):
    future = int(time.time()) + 3600
    codex_rl = {
        "limit_id": "codex",
        "primary": {"used_percent": 99.0, "window_minutes": 300, "resets_at": future},
        "secondary": {"used_percent": 62.0, "window_minutes": 10080, "resets_at": future + 1000},
    }
    premium_rl = {"limit_id": "premium", "primary": None, "secondary": None}

    _write_session(tmp_path, [codex_rl, premium_rl])
    monkeypatch.setattr(ud, "CODEX_SESSIONS", tmp_path)

    result = ud.read_codex()

    assert result["ok"] is True
    assert result["h5"]["used_pct"] == 99.0
    assert result["week"]["used_pct"] == 62.0


def test_no_valid_codex_entry_reports_not_ok(tmp_path, monkeypatch):
    premium_rl = {"limit_id": "premium", "primary": None, "secondary": None}
    _write_session(tmp_path, [premium_rl])
    monkeypatch.setattr(ud, "CODEX_SESSIONS", tmp_path)

    result = ud.read_codex()

    assert result["ok"] is False


def test_valid_codex_valid_entry_wins_over_earlier_one(tmp_path, monkeypatch):
    future = int(time.time()) + 3600
    older = {
        "limit_id": "codex",
        "primary": {"used_percent": 10.0, "window_minutes": 300, "resets_at": future},
        "secondary": {"used_percent": 20.0, "window_minutes": 10080, "resets_at": future + 1000},
    }
    newer = {
        "limit_id": "codex",
        "primary": {"used_percent": 55.0, "window_minutes": 300, "resets_at": future},
        "secondary": {"used_percent": 66.0, "window_minutes": 10080, "resets_at": future + 1000},
    }
    _write_session(tmp_path, [older, newer])
    monkeypatch.setattr(ud, "CODEX_SESSIONS", tmp_path)

    result = ud.read_codex()

    assert result["h5"]["used_pct"] == 55.0
    assert result["week"]["used_pct"] == 66.0


def test_backoff_assumes_100pct_once_stuck_near_full(tmp_path, monkeypatch):
    """If Codex is truly rate-limited, it never logs a fresh data point again -
    a near-full reading that stops updating for a while must be reported as
    100%, not stay stuck at its last logged value forever."""
    future = int(time.time()) + 3600
    codex_rl = {
        "limit_id": "codex",
        "primary": {"used_percent": 99.0, "window_minutes": 300, "resets_at": future},
        "secondary": {"used_percent": 62.0, "window_minutes": 10080, "resets_at": future + 1000},
    }
    fp = _write_session(tmp_path, [codex_rl])
    stale_mtime = time.time() - ud.CODEX_STUCK_AFTER_S - 10
    os.utime(fp, (stale_mtime, stale_mtime))
    monkeypatch.setattr(ud, "CODEX_SESSIONS", tmp_path)
    monkeypatch.setattr(ud, "_codex_backoff", {"until": 0.0, "cached": None})

    result = ud.read_codex_with_backoff()

    assert result["h5"]["used_pct"] == 100.0
    assert result["presumed_exhausted"] is True
    assert ud._codex_backoff["until"] > time.time()


def test_backoff_leaves_fresh_near_full_reading_untouched(tmp_path, monkeypatch):
    """A near-full reading that's still fresh (recently logged) is real data,
    not a stuck one - must be passed through unchanged."""
    future = int(time.time()) + 3600
    codex_rl = {
        "limit_id": "codex",
        "primary": {"used_percent": 99.0, "window_minutes": 300, "resets_at": future},
        "secondary": {"used_percent": 62.0, "window_minutes": 10080, "resets_at": future + 1000},
    }
    _write_session(tmp_path, [codex_rl])
    monkeypatch.setattr(ud, "CODEX_SESSIONS", tmp_path)
    monkeypatch.setattr(ud, "_codex_backoff", {"until": 0.0, "cached": None})

    result = ud.read_codex_with_backoff()

    assert result["h5"]["used_pct"] == 99.0
    assert "presumed_exhausted" not in result
