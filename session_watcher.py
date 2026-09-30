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
    if idle_seconds > 120:
        return 0.0
    # Exponential decay from 5s to 120s
    return max(0.0, min(1.0, 2.71828 ** (-(idle_seconds - 5) / 40.0)))


def get_agent_kind(source):
    """Map source to agent kind for iris_organism.h: 'c' for Claude, 'h' for Hermes."""
    # For now, treat all Hermes sources as 'h' (orange comets)
    # Claude Code sessions would need a hook to appear here
    return 'h'


def write_agents_file(sessions):
    """Write /tmp/iris_agents.txt with 6-field lines: A|kind|id|last_active|activity|purpose"""
    lines = []
    now = time.time()
    for s in sessions:
        last_active = s.get('last_activity_at') or s.get('started_at') or now
        activity = compute_activity(s['idle_seconds'])
        if activity <= 0:
            continue
        kind = get_agent_kind(s['source'])
        purpose = SOURCE_PURPOSE.get(s['source'], 'u')  # default to 'u' for unknown
        # Old 5-field format (backward compatible): A|kind|id|last_active|activity
        # New 6-field format: A|kind|id|last_active|activity|purpose
        line = f"A|{kind}|{s['id']}|{last_active:.0f}|{activity:.3f}|{purpose}"
        lines.append(line)

    # Atomic write
    tmp = AGENTS_FILE.with_name(f'.{AGENTS_FILE.name}.{os.getpid()}.tmp')
    tmp.write_text('\n'.join(lines) + ('\n' if lines else ''))
    tmp.replace(AGENTS_FILE)


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
        write_agents_file(sessions)

    if args.watch:
        print(f"Watching Hermes sessions -> {output_path}")
        while True:
            write_sessions()
            time.sleep(args.interval)
    else:
        write_sessions()


if __name__ == '__main__':
    main()