#!/usr/bin/env python3
"""Watch Hermes sessions and emit active session data for Iris."""

import json
import os
import sqlite3
import time
from pathlib import Path


STATE_DB = Path.home() / ".hermes" / "state.db"
OUTPUT_FILE = Path("/tmp/iris_sessions.json")
AGENTS_FILE = Path("/tmp/iris_agents.txt")


# Source -> purpose mapping
# u = working for Humdan (telegram, api_server = Apple Watch, cli)
# s = working for itself (cron, kanban/night-shift workers)
SOURCE_PURPOSE = {
    'telegram': 'u',
    'api_server': 'u',
    'cli': 'u',
    'cron': 's',
    'kanban': 's',
}


def get_active_sessions():
    """Fetch currently active sessions from Hermes state.db."""
    if not STATE_DB.exists():
        return []

    conn = sqlite3.connect(STATE_DB)
    conn.row_factory = sqlite3.Row
    cursor = conn.cursor()

    cursor.execute("""
        SELECT id, source, display_name, started_at, last_activity_at,
               profile_name, cwd, git_branch
        FROM sessions
        WHERE ended_at IS NULL OR ended_at = 0
        ORDER BY last_activity_at DESC NULLS LAST, started_at DESC
    """)

    sessions = []
    now = time.time()
    for row in cursor.fetchall():
        session = dict(row)
        # Compute idle time
        last_active = session.get('last_activity_at') or session.get('started_at') or now
        session['idle_seconds'] = now - last_active
        # Filter out stale sessions (idle > 1 hour) - Hermes doesn't close them properly
        if session['idle_seconds'] > 3600:
            continue
        # Short label for display
        if session['source'] == 'telegram':
            session['label'] = session.get('display_name') or 'Telegram'
            session['color'] = 'magenta'
        elif session['source'] == 'cron':
            session['label'] = 'Cron'
            session['color'] = 'amber'
        else:
            # CLI session
            cwd = session.get('cwd') or ''
            branch = session.get('git_branch') or ''
            if branch:
                session['label'] = f"🌿 {branch}"
            elif cwd:
                name = os.path.basename(cwd)
                session['label'] = f"📁 {name}"
            else:
                session['label'] = 'CLI'
            session['color'] = 'cyan'
        sessions.append(session)

    conn.close()
    return sessions


def compute_activity(idle_seconds):
    """Compute activity 0..1 from idle seconds. Active < 5s = 1.0, decays to 0 at 120s."""
    if idle_seconds < 5:
        return 1.0
    if idle_seconds > AGENT_WINDOW_S:
        return 0.0
    # Exponential decay from 5s to 120s
    return max(0.0, min(1.0, 2.71828 ** (-(idle_seconds - 5) / 40.0)))


# An agent counts as working while it produced output in the last 2 minutes.
AGENT_WINDOW_S = 120
CLAUDE_PROJECTS = Path.home() / ".claude" / "projects"


def hermes_agents(now):
    """Hermes sessions that wrote a message recently.

    sessions.last_activity_at is only set when a session starts, so it can't
    tell a working session from an idle one; the newest message timestamp can.
    Ended sessions still count for the window, so a short kanban worker shows up.
    """
    if not STATE_DB.exists():
        return []
    try:
        conn = sqlite3.connect(f"file:{STATE_DB}?mode=ro", uri=True, timeout=1)
        rows = conn.execute("""
            SELECT s.id, s.source,
                   (SELECT MAX(m.timestamp) FROM messages m WHERE m.session_id = s.id) AS last_msg
            FROM sessions s
            WHERE s.started_at > ? AND (s.ended_at IS NULL OR s.ended_at = 0 OR s.ended_at > ?)
        """, (now - 86400, now - AGENT_WINDOW_S)).fetchall()
        conn.close()
    except sqlite3.Error:
        return []
    agents = []
    for sid, source, last_msg in rows:
        if not last_msg or now - last_msg > AGENT_WINDOW_S:
            continue
        agents.append(('h', sid, last_msg, SOURCE_PURPOSE.get(source, 'u')))
    return agents


def claude_agents(now):
    """Claude Code sessions: each transcript (~/.claude/projects/*/<id>.jsonl)
    is appended to on every message and tool call, so its mtime is the
    session's last activity. Claude Code works for Humdan: purpose 'u'."""
    agents = []
    try:
        for f in CLAUDE_PROJECTS.glob("*/*.jsonl"):
            mtime = f.stat().st_mtime
            if now - mtime <= AGENT_WINDOW_S:
                agents.append(('c', f.stem, mtime, 'u'))
    except OSError:
        pass
    return agents


def write_agents_file(now):
    """Write /tmp/iris_agents.txt with 6-field lines: A|kind|id|last_active|activity|purpose"""
    agents = claude_agents(now) + hermes_agents(now)
    agents.sort(key=lambda a: a[2], reverse=True)   # most recent first: the renderer shows 8
    lines = []
    for kind, sid, last_active, purpose in agents:
        activity = compute_activity(now - last_active)
        if activity <= 0:
            continue
        lines.append(f"A|{kind}|{sid}|{last_active:.0f}|{activity:.3f}|{purpose}")

    # Atomic write
    tmp = AGENTS_FILE.with_name(f'.{AGENTS_FILE.name}.{os.getpid()}.tmp')
    tmp.write_text('\n'.join(lines) + ('\n' if lines else ''))
    tmp.replace(AGENTS_FILE)
    return lines


def main():
    """Run once and write output, or daemon mode with --watch."""
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--watch', action='store_true', help='Run as daemon, update every 2s')
    parser.add_argument('--interval', type=float, default=2.0, help='Update interval in seconds')
    parser.add_argument('--output', default=str(OUTPUT_FILE), help='Output JSON file')
    args = parser.parse_args()

    output_path = Path(args.output)

    def write_sessions():
        sessions = get_active_sessions()
        data = {
            'timestamp': time.time(),
            'count': len(sessions),
            'sessions': sessions
        }
        # Atomic write for sessions JSON
        tmp = output_path.with_name(f'.{output_path.name}.{os.getpid()}.tmp')
        tmp.write_text(json.dumps(data))
        tmp.replace(output_path)
        print(f"[{time.strftime('%H:%M:%S')}] Active sessions: {len(sessions)}")
        for s in sessions:
            print(f"  {s['label']} ({s['source']}) idle {s['idle_seconds']:.0f}s")

        # Also write agents file for the renderer
        for line in write_agents_file(time.time()):
            print(f"  agent {line}")

    if args.watch:
        print(f"Watching Hermes sessions -> {output_path}")
        while True:
            write_sessions()
            time.sleep(args.interval)
    else:
        write_sessions()


if __name__ == '__main__':
    main()