"""Iris plugin — drives the Iris LCD visualizer from live Hermes activity.

Maintains a decaying activity level in [0, 1]:
  * an LLM request in flight holds a baseline (model is "thinking")
  * every tool call, streamed text chunk and API round-trip adds a kick
  * with nothing happening the level decays to 0 within a few seconds
A background thread writes the level to the Iris state file at ~10 Hz.
iris_fb reads it and scales firing rate, drift speed and brightness to match.
"""

from __future__ import annotations

import os
import threading
import time

_STATE_FILE = os.environ.get("IRIS_STATE_FILE", "/tmp/iris_state")

_lock = threading.Lock()
_level = 0.0          # decaying activity 0..1
_inflight = 0         # LLM requests currently in flight
_last_written = -1.0
_writer_started = False

# Tunables
_DECAY_PER_S = 0.6          # how fast the level falls when idle
_BASELINE_INFLIGHT = 0.45   # floor while the model is generating
_KICK_TOOL = 0.35
_KICK_API = 0.25
_KICK_DELTA = 0.05


def _bump(amount: float) -> None:
    global _level
    with _lock:
        _level = min(1.0, _level + amount)


def _write(level: float) -> None:
    global _last_written
    if abs(level - _last_written) < 0.01:
        return
    tmp = _STATE_FILE + ".tmp"
    try:
        with open(tmp, "w") as f:
            f.write(f"{level:.3f}\n")
        os.replace(tmp, _STATE_FILE)
        _last_written = level
    except OSError:
        pass


def _writer() -> None:
    global _level
    last = time.monotonic()
    while True:
        time.sleep(0.1)
        now = time.monotonic()
        dt, last = now - last, now
        with _lock:
            _level = max(0.0, _level - _DECAY_PER_S * dt)
            if _inflight > 0:
                _level = max(_level, _BASELINE_INFLIGHT)
            lvl = _level
        _write(lvl)


def _ensure_writer() -> None:
    global _writer_started
    with _lock:
        if _writer_started:
            return
        _writer_started = True
    threading.Thread(target=_writer, name="iris-writer", daemon=True).start()


# ---- hook callbacks (all observers; return values ignored) ----

def _on_pre_api_request(**_kw) -> None:
    global _inflight
    _ensure_writer()
    with _lock:
        _inflight += 1
    _bump(_KICK_API)


def _on_post_api_request(**_kw) -> None:
    global _inflight
    with _lock:
        _inflight = max(0, _inflight - 1)


def _on_api_request_error(**_kw) -> None:
    _on_post_api_request()


def _on_stream_delta(**_kw) -> None:
    _bump(_KICK_DELTA)


def _on_pre_tool_call(**_kw) -> None:
    _ensure_writer()
    _bump(_KICK_TOOL)


def _on_post_tool_call(**_kw) -> None:
    _bump(_KICK_TOOL * 0.5)


def _on_session_start(**_kw) -> None:
    _ensure_writer()
    _bump(0.3)


def _on_session_end(**_kw) -> None:
    global _inflight, _level
    with _lock:
        _inflight = 0
        _level = 0.0
    _write(0.0)


def register(ctx) -> None:
    ctx.register_hook("pre_api_request", _on_pre_api_request)
    ctx.register_hook("post_api_request", _on_post_api_request)
    ctx.register_hook("api_request_error", _on_api_request_error)
    ctx.register_hook("on_stream_delta", _on_stream_delta)
    ctx.register_hook("pre_tool_call", _on_pre_tool_call)
    ctx.register_hook("post_tool_call", _on_post_tool_call)
    ctx.register_hook("on_session_start", _on_session_start)
    ctx.register_hook("on_session_end", _on_session_end)
    _ensure_writer()
