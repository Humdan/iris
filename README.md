# Iris - Terminal Animation for AI Activity Visualization

A terminal-based animation system that visualizes AI agent activity states through
terminal animations. It displays dynamic patterns when "thinking" and subtle,
slower animations when "idle."

## Features

- **Thinking Mode**: Fast-paced, dynamic visualizations that indicate active processing
- **Idle Mode**: Gentle, slow animations that show the system is waiting
- **Neural Network Visualization**: Wave-based patterns inspired by neural activity
- **Activity-Based Transitions**: Automatic state switching based on activity levels

## Installation

```bash
git clone https://github.com/Humdan/iris.git
cd iris
pip install -r requirements.txt
```

## Usage

```bash
# Run with default settings
python3 iris.py

# Run as a background daemon that reads state from a file
python3 iris.py --daemon --state-file /tmp/iris_state

# Signal activity (writing "thinking" sets active mode, anything else is idle)
echo "thinking" > /tmp/iris_state
echo "idle" > /tmp/iris_state
```

## Session Orbs — one marble per open Hermes session

`session_watcher.py` polls `~/.hermes/state.db` every 2 seconds and writes the
currently open sessions to `/tmp/iris_sessions.json`. Iris renders one orb for
each open session:

- **Most recent / active session** — bigger, brighter, and in a distinct color
  (magenta for Telegram, amber for cron, cyan for CLI). It pulses and orbits
  slightly faster so your eye goes straight to it.
- **Other open sessions** — smaller, dimmer orbs in their source color, orbiting
  more slowly.
- **Status line** — shows the live session count in both idle and thinking modes.

```bash
# Run the watcher (or let the systemd service do it)
python3 session_watcher.py --watch --interval 2

# Override the data path if you want to test with your own JSON
export IRIS_SESSIONS_FILE=/path/to/sessions.json
```

Orbs are overlaid on top of the idle and thinking animations, so they stay
visible regardless of the activity state. If the watcher file is missing or
unreadable, Iris silently falls back to no orbs.

## Pi LCD / boot service

`iris_fb.c` is a smooth per-pixel renderer for the framebuffer (`/dev/fb0`):
a neural-network / knowledge-graph of glowing nodes on black. Idle: slow drift,
dim edges, the occasional lazy pulse. Thinking: pulses race along edges, nodes
flare, the graph rewires. Multithreaded C, 30 fps on a Pi 4 at 800x480, no X11.

```bash
make                    # builds ./iris_fb
./iris_fb               # run on the LCD (state file /tmp/iris_state)
./install-user.sh       # build + enable as a user service (no sudo; needs linger)
sudo ./install.sh       # or: system service that takes over tty1
iris-state thinking     # switch animation
iris-state idle
```

The Python `--framebuffer` mode is kept as a fallback but is blocky and slow.

The service takes over tty1, starts idle, and restarts on failure.

## Layout is owned by the dashboard (http://<pi>:8080)

Where the LCD widgets sit, and which ones are drawn at all, is **not** hardcoded any more.
`iris_fb` reads `~/.config/iris/layout.conf` (one `key=value` per line) and re-reads it at the
1 Hz stats cadence, so a change made in the dashboard's **LCD — Iris screen** card lands on the
real panel within a second — no restart, no rebuild. Touch zones are derived from the same
values, so a widget that moves takes its tap target with it, and a widget that is switched off
stops responding to taps.

| key | what it moves |
|---|---|
| `clock.enabled` / `.x` / `.y` | big clock + date (x is the CENTER) |
| `queue.enabled` / `.x` / `.y` / `.rows` | left cron-queue column, rows visible |
| `panel.enabled` / `.y` | bottom agent panel (the night pill rides with it) |
| `portfolio.enabled` / `.x` | portfolio card inside the panel (left edge) |
| `orbs.enabled` | per-session orbs around the sphere |
| `nightbtn.enabled` | top-right RUN NIGHT SHIFT button |

Delete the file to fall back to the built-in defaults, which are exactly the old hardcoded
layout. Values are clamped both in `server.py` and in `layout_clamp()` so nothing can be pushed
off-screen. Adding a field means: `LAYOUT_FIELDS` in `iris_layout.h`, a default in
`layout_defaults()`, and the matching entry in `server.py`'s `LCD_SCHEMA` — the dashboard UI is
generated from that schema, so no JS change is needed.

## Driving it from Hermes Agent (live "processor" mode)

`plugin/` is a Hermes plugin that turns real agent activity into a decaying
0.0-1.0 level written to `/tmp/iris_state` at 10 Hz: every LLM request, tool
call and streamed token kicks it up; silence lets it fall to zero in ~2 s.
`iris_fb` scales firing rate, drift and brightness continuously with it.

```bash
ln -s ~/dev/iris/plugin ~/.hermes/plugins/iris
hermes plugins enable iris        # takes effect in the next Hermes session
```

You can also drive the file by hand: `echo 0.7 > /tmp/iris_state`, or the words
`thinking` / `idle`.

## States

The animation has two primary states:

### Thinking (Active)
- Faster frame rate (50ms)
- Dynamic wave propagation
- Multiple concurrent pattern layers
- Pulsing center with expanding rings

### Idle
- Slower frame rate (200ms)
- Single gentle pulse
- Subtle background pattern
- Smooth, calming motion

## Architecture

```
iris/
├── README.md
├── requirements.txt
├── iris.py                 # Main entry point
├── animation_engine.py     # Core animation logic
├── session_watcher.py      # Polls Hermes DB, emits /tmp/iris_sessions.json
├── states/
│   ├── thinking.py         # Thinking mode animations + session orbs
│   └── idle.py             # Idle mode animations + session orbs
└── utils/
    ├── terminal.py         # Terminal control utilities
    ├── patterns.py         # Shared animation patterns
    └── framebuffer.py      # Framebuffer color palette
```

## License

MIT
