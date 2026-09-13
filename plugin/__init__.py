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
_TASK_FILE = os.environ.get("IRIS_TASK_FILE", "/tmp/iris_task")
# Live step log tailed by the iris console during night-shift runs. Each tool
# call appends one timestamped, human-readable line here; the renderer shows the
# tail. Gated to night-shift sessions so normal daytime work isn't dumped to the
# LCD. Session start truncates it; session end leaves the final trace visible.
_LIVE_LOG = os.environ.get("IRIS_NS_LIVE_LOG", "/tmp/iris_ns_live.log")
_LIVE_MAX_LINES = 200   # ring-trim so the file never grows unbounded

# Map raw tool names -> short friendly labels shown on the LCD ("what's being
# worked on"). Unknown tools are upper-cased and truncated by the renderer.
_TASK_LABELS = {
    "terminal": "SHELL",
    "execute_code": "CODE",
    "browser_exec": "BROWSER",
    "web_search": "SEARCH",
    "web_extract": "FETCH",
    "read_file": "READ",
    "write_file": "WRITE",
    "patch": "EDIT",
    "search_files": "GREP",
    "delegate_task": "AGENTS",
    "vision_analyze": "VISION",
    "image_generate": "IMAGE",
    "text_to_speech": "SPEAK",
    "clarify": "ASKING",
    "memory": "MEMORY",
    "skill_view": "SKILL",
    "skill_manage": "SKILL",
}


def _write_task(label: str) -> None:
    """Write the current task label (or empty to clear). Atomic, best-effort."""
    tmp = _TASK_FILE + ".tmp"
    try:
        with open(tmp, "w") as f:
            f.write((label or "")[:16] + "\n")
        os.replace(tmp, _TASK_FILE)
    except OSError:
        pass


# --- night-shift live step log -------------------------------------------
# Only sessions belonging to the Night shift cron job stream their steps to the
# LCD. We detect that by the job id appearing in HERMES_SESSION_ID (cron names
# sessions cron_<jobid>_<ts>) or an explicit IRIS_NS_LIVE=1 override.
_NS_JOB_ID = "b7a26b64f480"
_is_ns_session = None   # cached per-process detection


def _ns_session_active() -> bool:
    global _is_ns_session
    if _is_ns_session is not None:
        return _is_ns_session
    if os.environ.get("IRIS_NS_LIVE") == "1":
        _is_ns_session = True
        return True
    sid = os.environ.get("HERMES_SESSION_ID", "") or os.environ.get("HERMES_SESSION", "")
    _is_ns_session = _NS_JOB_ID in sid
    return _is_ns_session


def _live_reset() -> None:
    """Truncate the live log at the start of a night-shift session."""
    if not _ns_session_active():
        return
    try:
        with open(_LIVE_LOG, "w") as f:
            f.write("")
    except OSError:
        pass


def _live_append(line: str) -> None:
    """Append one timestamped step line, ring-trimming to _LIVE_MAX_LINES."""
    if not _ns_session_active():
        return
    stamp = time.strftime("%H:%M:%S")
    entry = f"{stamp} {line}".rstrip()[:120]
    try:
        # append then trim if the file grew past the cap (cheap, best-effort)
        with open(_LIVE_LOG, "a") as f:
            f.write(entry + "\n")
        with open(_LIVE_LOG) as f:
            lines = f.readlines()
        if len(lines) > _LIVE_MAX_LINES:
            with open(_LIVE_LOG, "w") as f:
                f.writelines(lines[-_LIVE_MAX_LINES:])
    except OSError:
        pass


def _arg_hint(tool_name: str, args: dict) -> str:
    """Compact, readable hint about what a tool call is doing, for the LCD."""
    if not isinstance(args, dict):
        return ""
    def g(*keys):
        for k in keys:
            v = args.get(k)
            if isinstance(v, str) and v.strip():
                return v.strip().replace("\n", " ")
        return ""
    if tool_name == "terminal":
        return g("command")
    if tool_name in ("read_file", "write_file", "patch"):
        return g("path", "file_path")
    if tool_name in ("web_search",):
        return g("query")
    if tool_name in ("web_extract",):
        u = args.get("urls")
        return (u[0] if isinstance(u, list) and u else g("url"))
    if tool_name == "search_files":
        return g("pattern")
    if tool_name in ("skill_view", "skill_manage"):
        return g("name")
    if tool_name == "delegate_task":
        t = args.get("tasks")
        if isinstance(t, list) and t and isinstance(t[0], dict):
            return str(t[0].get("goal", ""))[:80]
    return g("prompt", "content", "text")


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


def _on_pre_tool_call(tool_name: str = "", args=None, **_kw) -> None:
    _ensure_writer()
    _bump(_KICK_TOOL)
    label = _TASK_LABELS.get(tool_name, (tool_name or "").upper())
    _write_task(label)
    # Stream a readable step line to the night-shift live log (no-op otherwise).
    hint = _arg_hint(tool_name, args if isinstance(args, dict) else {})
    _live_append(f"{label}: {hint}" if hint else label)


def _on_post_tool_call(**_kw) -> None:
    _bump(_KICK_TOOL * 0.5)


def _on_session_start(**_kw) -> None:
    _ensure_writer()
    _bump(0.3)
    _live_reset()
    _live_append("night shift started")


def _on_session_end(**_kw) -> None:
    global _inflight, _level
    with _lock:
        _inflight = 0
        _level = 0.0
    _write(0.0)
    _write_task("")
    # Always leave a closing line so even a no-op / [SILENT] run shows it ran.
    # Keep the mtime fresh for ~a few more seconds of "just finished" banner.
    _live_append("night shift finished")


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
