# Iris — AI Activity Visualizer

Iris shows what the agents on this Pi are doing. There are two implementations:

| Implementation | Target | Entry point | Notes |
|---|---|---|---|
| **C framebuffer** (production) | Pi LCD, `/dev/fb0`, 800×480 RGB565 | `iris_fb` (built from `iris_fb.c`) | ~60 fps absolute-clock pacing, touch input, no X11 |
| **Python terminal** (older) | Any terminal | `iris.py` | ANSI animation; `--framebuffer` is a slow fallback |

Both read the activity state file (`/tmp/iris_state`) and the session file
(`/tmp/iris_sessions.json`), and both are driven by the Hermes `iris` plugin.

## C framebuffer version — Pi LCD

`iris_fb.c` renders a see-through holographic "digital organism"
(`iris_organism.h`): nested shells of circuit traces around a spinning core.
Traces are born, live and fade; each heartbeat sends a ripple outward; data
pulses travel along traces. Activity (0.0–1.0) scales speed, trace count and
brightness: deep blue at rest, bright electric cyan-blue when busy, maroon while
night shift runs.

### What is on the screen

- **Center / right**: the organism orb, with one small orb per open Hermes
  session orbiting it (`orbs.enabled`).
- **Top**: clock and date.
- **Left column**: the cron queue (scroll by dragging, tap a row for its
  detail overlay) and the **SERVICES** widget (see below). The column is compact
  by default and can be expanded.
- **Top-right**: **RUN NIGHT SHIFT** button — fires the night-shift cron job in
  a detached child.
- **Bottom panel**: agent panel (live Claude Code / Hermes activity), the
  portfolio card, and the **NIGHT** pill that toggles manual night-shift mode
  (shared with the dashboard via `/tmp/iris_ns_manual`; the flag expires on its
  own after the night window).
- **Night-shift log**: while night shift is on, it takes over the left column
  (NOW / NEXT / DONE lanes from `/tmp/iris_ns_view.txt`, or the step log).

### Build and run

```bash
make                    # builds ./iris_fb  (make clean && make to rebuild)
./iris_fb [state-file]  # default state file: /tmp/iris_state
```

The state file accepts `thinking`, `idle`, or a float `0.0–1.0` (the plugin
writes a float at ~10 Hz). Only one renderer may own the screen: `iris_fb`
takes a lock on `/tmp/iris_fb.lock`, and a second copy waits as a standby.
`IRIS_TOUCH_DEV` overrides the touchscreen input device.

### Snapshot preview (does not touch the screen)

```bash
IRIS_SNAPSHOT=/tmp/iris.raw IRIS_SNAPSHOT_FRAMES=120 ./iris_fb
scripts/lcd-shot --from /tmp/iris.raw out.png
```

Renders ~2 s off-screen (no framebuffer, no lock, no touch) and writes the last
frame as raw RGB565 800×480. Safe to run while the live renderer owns the screen.

### Install as a service

```bash
sudo ./install.sh       # system service iris.service (takes over tty1) — the one used on the Pi
./install-user.sh       # alternative: user service (no sudo; needs loginctl enable-linger)
iris-state thinking     # both install iris-state; it writes /tmp/iris_state
iris-state idle
```

Run only one of the two services; two renderers on the same framebuffer flicker.

## Layout is owned by the dashboard (http://<pi>:8080)

Widget positions and visibility are **not** hardcoded. `iris_fb` reads
`~/.config/iris/layout.conf` (one `key=value` per line) and re-reads it at the
1 Hz stats cadence, so a change made in the dashboard's **LCD — Iris screen**
card lands on the panel within a second — no restart, no rebuild. Touch zones
are derived from the same values, so a widget that moves takes its tap target
with it, and a widget that is switched off stops responding to taps. The
night-shift log panel follows the queue column (`queue.x` / `queue.y`) and
stops just above the bottom panel (`panel.y`).

| key | what it moves |
|---|---|
| `clock.enabled` / `.x` / `.y` | big clock + date (x is the CENTER) |
| `queue.enabled` / `.x` / `.y` / `.rows` | left cron-queue column, rows visible |
| `services.enabled` | SERVICES widget in the left column |
| `panel.enabled` / `.y` | bottom agent panel (the night pill rides with it) |
| `portfolio.enabled` / `.x` | portfolio card inside the panel (left edge) |
| `orbs.enabled` | per-session orbs around the sphere |
| `nightbtn.enabled` | top-right RUN NIGHT SHIFT button |

Delete the file to fall back to the built-in defaults. Values are clamped both
in `server.py` and in `layout_clamp()` so nothing can be pushed off-screen.
Adding a field means: `LAYOUT_FIELDS` in `iris_layout.h`, a default in
`layout_defaults()`, and the matching entry in `server.py`'s `LCD_SCHEMA` — the
dashboard UI is generated from that schema, so no JS change is needed.

## SERVICES widget and heartbeat

`iris_heartbeat.py` (user unit `iris-heartbeat.service`) probes each service on
an interval and rewrites `/tmp/iris_services`; `iris_services.h` only reads that
file, so the render loop never blocks on the network. Only dynamic services are
listed; the network itself is the NET dot in the header. Managed websites come
from `~/.hermes/managed-sites.json`, and apps from the `svc` port registry.

## Session orbs

`session_watcher.py` (user unit `session-watcher.service`) polls
`~/.hermes/state.db` every 2 s and writes the open sessions to
`/tmp/iris_sessions.json`. The most recent session's orb is bigger, brighter and
faster; others are smaller and dimmer, colored by source (magenta Telegram,
amber cron, cyan CLI). A missing or unreadable file just means no orbs.

```bash
python3 session_watcher.py --watch --interval 2
export IRIS_SESSIONS_FILE=/path/to/sessions.json   # override the path
```

## Hermes plugin — live activity → state file

`plugin/` is a Hermes plugin that turns agent activity into a decaying 0.0–1.0
level written to `/tmp/iris_state` at ~10 Hz: every LLM request, tool call and
streamed token kicks it up; silence lets it fall to zero in ~2 s. It also writes
the current tool label to `/tmp/iris_task`, a step log for night-shift runs to
`/tmp/iris_ns_live.log`, and night-shift lanes (`ns_lanes.py`) to
`/tmp/iris_ns_view.txt`. The plugin runs inside the Hermes gateway, so plugin
edits need a gateway restart.

```bash
ln -s ~/dev/iris/plugin ~/.hermes/plugins/iris
hermes plugins enable iris        # takes effect in the next Hermes session
```

Environment overrides: `IRIS_STATE_FILE`, `IRIS_TASK_FILE`, `IRIS_NS_LIVE_LOG`,
`IRIS_NS_STATE`, `IRIS_NS_VIEW`.

## Scripts

| Script | Purpose |
|---|---|
| `scripts/lcd-shot [out.png] [--crop x0,y0,x1,y1] [--from raw]` | Save the LCD (or a raw `IRIS_SNAPSHOT` frame) as a PNG; prints the path. |
| `scripts/fbsnap.py [out.png]` | Dump `/dev/fb0` (RGB565 800×480) to PNG, or PPM without ImageMagick. |
| `scripts/ns-task plan\|start\|done\|show\|clear` | Announce what an autonomous (cron) Hermes run is working on; shown live on the LCD during night shift. |
| `iris-state thinking\|idle` | Write the activity state file. |

## Python terminal version

```bash
pip install -r requirements.txt
python3 iris.py                                        # interactive: SPACE toggles states
python3 iris.py --daemon --state-file /tmp/iris_state  # follow the state file
```

Thinking mode is fast and layered (waves, neural-net pattern, particles); idle
is a slow pulse. Session orbs are overlaid in both modes.

## Layout of the repo

```
iris/
├── Makefile                 # builds iris_fb
├── iris_fb.c                # C framebuffer renderer: main loop, touch thread, compositing
├── iris_organism.h          # the organism orb
├── iris_widgets.h           # font, clock, queue, agent panel, night-shift panel, session orbs
├── iris_layout.h            # layout.conf parsing (dashboard-owned layout)
├── iris_services.h          # SERVICES widget
├── iris_heartbeat.py        # service probes -> /tmp/iris_services
├── session_watcher.py       # Hermes DB -> /tmp/iris_sessions.json
├── iris.service, iris-user.service, iris-heartbeat.service, session-watcher.service
├── install.sh, install-user.sh, iris-state
├── plugin/                  # Hermes plugin (__init__.py, ns_lanes.py, plugin.yaml)
├── scripts/                 # lcd-shot, fbsnap.py, ns-task
├── iris.py, animation_engine.py, states/, utils/   # Python terminal version
└── iris-idle.*, iris-working.*                     # image assets
```

## License

MIT
