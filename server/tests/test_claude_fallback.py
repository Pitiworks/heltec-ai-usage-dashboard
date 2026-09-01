"""Regressionstest: schlaegt der Claude-Live-Probe fehl, waehrend der letzte
bekannte Stand schon (fast) voll war, muss das als vermutlich aufgebraucht (100%)
gemeldet werden - statt den alten (zu niedrigen) Wert unveraendert weiterzuservieren."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import usage_dashboard as ud  # noqa: E402


def _cached(h5_pct, week_pct):
    return {
        "ok": True,
        "h5": {"used_pct": h5_pct, "reset": 1788200000},
        "week": {"used_pct": week_pct, "reset": 1788500000},
        "status": "allowed",
    }


def test_no_prior_data_reports_not_ok():
    result = ud.claude_fallback_on_probe_failure(None, "API-Call fehlgeschlagen: timeout")

    assert result["ok"] is False


def test_probe_failure_with_low_cached_value_stays_unchanged():
    cached = _cached(23.0, 40.0)

    result = ud.claude_fallback_on_probe_failure(cached, "API-Call fehlgeschlagen: timeout")

    assert result["ok"] is True
    assert result["stale"] is True
    assert result["h5"]["used_pct"] == 23.0
    assert result["week"]["used_pct"] == 40.0
    assert "presumed_exhausted" not in result


def test_probe_failure_with_near_full_h5_reports_100pct():
    cached = _cached(99.0, 40.0)

    result = ud.claude_fallback_on_probe_failure(cached, "API-Call fehlgeschlagen: 429")

    assert result["h5"]["used_pct"] == 100.0
    assert result["week"]["used_pct"] == 40.0
    assert result["presumed_exhausted"] is True


def test_probe_failure_with_near_full_week_reports_100pct():
    cached = _cached(50.0, 99.5)

    result = ud.claude_fallback_on_probe_failure(cached, "API-Call fehlgeschlagen: 429")

    assert result["h5"]["used_pct"] == 50.0
    assert result["week"]["used_pct"] == 100.0
    assert result["presumed_exhausted"] is True


def test_probe_failure_leaves_reset_timestamps_untouched():
    cached = _cached(99.0, 99.0)

    result = ud.claude_fallback_on_probe_failure(cached, "API-Call fehlgeschlagen: 429")

    assert result["h5"]["reset"] == 1788200000
    assert result["week"]["reset"] == 1788500000
