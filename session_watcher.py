#!/usr/bin/env python3
"""Watch Hermes sessions and emit active session data for Iris."""

import json
import os
import sqlite3
import time
from pathlib import Path


STATE_DB = Path.home() / ".hermes" / "state.db"
OUTPUT_FILE = Path("/tmp/iris_sessions.json")


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
        # Atomic write
        tmp = output_path.with_suffix('.tmp')
        tmp.write_text(json.dumps(data))
        tmp.replace(output_path)
        print(f"[{time.strftime('%H:%M:%S')}] Active sessions: {len(sessions)}")
        for s in sessions:
            print(f"  {s['label']} ({s['source']}) idle {s['idle_seconds']:.0f}s")
    
    if args.watch:
        print(f"Watching Hermes sessions -> {output_path}")
        while True:
            write_sessions()
            time.sleep(args.interval)
    else:
        write_sessions()


if __name__ == '__main__':
    main()