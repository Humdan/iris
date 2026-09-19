"""Live "what is autonomous Hermes working on" state for the Iris LCD.

A *lane* is one running autonomous session: a cron job (e.g. Night shift,
TokenTimes backfill) or a kanban worker (a night-shift card, an EV task).
Lanes are opened/updated/closed automatically by the Iris plugin hooks; inside
a lane the agent can announce named tasks with the `ns-task` CLI:

    ns-task plan "Compact sessions" "Commit WIP" "Stage proposals"
    ns-task start "Compact sessions"      # finishes the previous active task
    ns-task done

State:  /tmp/iris_ns_state.json   (JSON, source of truth, flock-guarded)
View:   /tmp/iris_ns_view.txt     (flat lines the C renderer parses)
    L|<name>|<started_epoch>|<0 other, 1 night-shift coordinator, 2 night-shift worker>
    S|<step_epoch>|<latest step text>
    T|<a=active d=done q=queued>|<started_epoch>|<ended_epoch>|<title>
    N|<open night-shift cards>              (tonight's backlog, from the kanban board)
    Q|<title>                               queued night card
    D|<title>                               finished night card (newest first)
"""
from __future__ import annotations

import fcntl
import json
import os
import time

STATE = os.environ.get("IRIS_NS_STATE", "/tmp/iris_ns_state.json")
VIEW = os.environ.get("IRIS_NS_VIEW", "/tmp/iris_ns_view.txt")
LOCK = STATE + ".lock"
JOBS = os.path.expanduser("~/.hermes/cron/jobs.json")
NS_JOB_ID = "b7a26b64f480"
# night-shift coordinator jobs: kickoff, supervisor, report
NS_JOB_IDS = {NS_JOB_ID, "111af46795b0", "39a5995dc824"}
STALE_S = 45 * 60          # a lane with no activity for this long is dropped
MAX_TASKS = 24


def _job_name(job_id: str) -> str:
    try:
        with open(JOBS) as f:
            d = json.load(f)
        for j in d.get("jobs", d) if isinstance(d, dict) else d:
            if j.get("id") == job_id:
                return j.get("name") or job_id
    except (OSError, ValueError):
        pass
    return job_id


KANBAN_DB = os.path.expanduser("~/.hermes/kanban.db")


def _kanban_task(task_id: str) -> dict | None:
    import sqlite3
    try:
        con = sqlite3.connect(f"file:{KANBAN_DB}?mode=ro", uri=True, timeout=2)
        row = con.execute("select title, tenant from tasks where id = ?", (task_id,)).fetchone()
        con.close()
        return {"title": row[0], "tenant": row[1] or ""} if row else None
    except Exception:
        return None


def _identity(session_id: str) -> dict | None:
    """Who is this session? cron job or kanban worker; None = not tracked."""
    job_id = cron_job_id(session_id)
    if job_id:
        return {"kind": "cron", "job_id": job_id, "name": _job_name(job_id),
                "flag": 1 if job_id in NS_JOB_IDS else 0}
    task_id = os.environ.get("HERMES_KANBAN_TASK")
    if task_id:
        t = _kanban_task(task_id) or {"title": task_id, "tenant": ""}
        return {"kind": "kanban", "task_id": task_id, "name": t["title"],
                "flag": 2 if t["tenant"].startswith("night-") else 0}
    return None


def _night_board() -> tuple[int, list[str], list[str]]:
    """(open card count, queued titles, finished titles newest first) for tonight."""
    import sqlite3
    from datetime import datetime
    tenant = "night-" + datetime.now().strftime("%Y-%m-%d")
    try:
        con = sqlite3.connect(f"file:{KANBAN_DB}?mode=ro", uri=True, timeout=2)
        rows = con.execute("select title, status from tasks where tenant = ? "
                           "order by coalesce(completed_at, created_at) desc", (tenant,)).fetchall()
        con.close()
    except Exception:
        return 0, [], []
    open_ = sum(1 for _, st in rows if st in ("ready", "todo", "running", "triage"))
    queued = [t for t, st in reversed(rows) if st in ("ready", "todo", "triage")]
    done = [t for t, st in rows if st in ("done", "review")]
    return open_, queued, done


def cron_job_id(session_id: str) -> str | None:
    """cron_<jobid>_<date>_<time> -> jobid; None for non-cron sessions."""
    if not session_id or not session_id.startswith("cron_"):
        return None
    parts = session_id.split("_")
    return parts[1] if len(parts) > 1 else None


class _Locked:
    def __enter__(self):
        self.fd = open(LOCK, "a+")
        fcntl.flock(self.fd, fcntl.LOCK_EX)
        try:
            with open(STATE) as f:
                self.state = json.load(f)
        except (OSError, ValueError):
            self.state = {}
        self.state.setdefault("lanes", {})
        return self.state

    def __exit__(self, *exc):
        now = time.time()
        lanes = self.state["lanes"]
        for sid in [s for s, l in lanes.items() if now - l.get("touched", 0) > STALE_S]:
            del lanes[sid]
        _atomic_write(STATE, json.dumps(self.state))
        _atomic_write(VIEW, _render(self.state))
        fcntl.flock(self.fd, fcntl.LOCK_UN)
        self.fd.close()
        return False


def _atomic_write(path: str, text: str) -> None:
    tmp = f"{path}.{os.getpid()}.tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def _clean(s: str) -> str:
    return " ".join(str(s).replace("|", "/").split())[:160]


def _render(state: dict) -> str:
    lanes = sorted(state["lanes"].values(),
                   key=lambda l: ({1: 0, 2: 1}.get(l.get("flag", 0), 2), l.get("started", 0)))
    out = []
    for l in lanes:
        out.append(f"L|{_clean(l['name'])}|{int(l['started'])}|{int(l.get('flag', 0))}")
        if l.get("step"):
            out.append(f"S|{int(l.get('step_at', 0))}|{_clean(l['step'])}")
        for t in l.get("tasks", []):
            out.append(f"T|{t['status'][0]}|{int(t.get('started') or 0)}|{int(t.get('ended') or 0)}|{_clean(t['title'])}")
    open_, queued, done = _night_board()
    if open_ or queued or done:
        out.append(f"N|{open_}")
        out += [f"Q|{_clean(t)}" for t in queued[:8]]
        out += [f"D|{_clean(t)}" for t in done[:8]]
    return "\n".join(out) + ("\n" if out else "")


# ---------------------------------------------------------------- lanes (hooks)
def open_lane(session_id: str) -> None:
    ident = _identity(session_id)
    if not ident:
        return
    with _Locked() as st:
        now = time.time()
        st["lanes"][session_id] = {**ident, "started": now, "touched": now,
                                   "step": "", "step_at": 0, "tasks": []}


def step(session_id: str, text: str) -> None:
    ident = _identity(session_id)
    if not ident:
        return
    with _Locked() as st:
        lane = st["lanes"].get(session_id)
        if lane is None:            # lane opened before the plugin loaded: adopt it
            lane = st["lanes"][session_id] = {**ident, "started": time.time(), "tasks": []}
        lane["step"], lane["step_at"] = text, time.time()
        lane["touched"] = lane["step_at"]


def close_lane(session_id: str) -> None:
    if not _identity(session_id):
        return
    with _Locked() as st:
        st["lanes"].pop(session_id, None)


# ---------------------------------------------------------------- tasks (ns-task)
def _pick_lane(st: dict, session_id: str | None) -> dict | None:
    lanes = st["lanes"]
    if session_id and session_id in lanes:
        return lanes[session_id]
    ns = [l for l in lanes.values() if l.get("flag") == 1]
    pool = ns or list(lanes.values())
    return max(pool, key=lambda l: l.get("touched", 0)) if pool else None


def task_cmd(action: str, titles: list[str], session_id: str | None, parallel: bool = False) -> str:
    with _Locked() as st:
        lane = _pick_lane(st, session_id)
        if lane is None:
            return "no running cron lane (ns-task only works inside a scheduled run)"
        tasks = lane.setdefault("tasks", [])
        now = time.time()
        lane["touched"] = now
        find = lambda t: next((x for x in tasks if x["title"].lower() == t.lower()), None)
        if action == "plan":
            for t in titles:
                if not find(t):
                    tasks.append({"title": t, "status": "queued", "started": 0, "ended": 0})
        elif action == "start":
            if not parallel:
                for x in tasks:
                    if x["status"] == "active":
                        x["status"], x["ended"] = "done", now
            for t in titles:
                x = find(t)
                if x:
                    x["status"], x["started"], x["ended"] = "active", now, 0
                else:
                    tasks.append({"title": t, "status": "active", "started": now, "ended": 0})
        elif action == "done":
            active = [x for x in tasks if x["status"] == "active"]
            targets = [x for x in active if any(t.lower() in x["title"].lower() for t in titles)] if titles else active[-1:]
            for x in targets:
                x["status"], x["ended"] = "done", now
        elif action == "clear":
            tasks.clear()
        del tasks[:-MAX_TASKS]
        return f"{lane['name']}: " + ", ".join(f"{x['title']} [{x['status']}]" for x in tasks if x["status"] != "done") \
            if any(x["status"] != "done" for x in tasks) else f"{lane['name']}: no open tasks"
