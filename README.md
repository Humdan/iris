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

## Driving it from Hermes Agent (live "processor" mode)

`plugin/` is a Hermes plugin that turns real agent activity into a decaying
0.0-1.0 level written to `/tmp/iris_state` at 10 Hz: every LLM request, tool
call and streamed token kicks it up; silence lets it fall to zero in ~2 s.
`iris_fb` scales firing rate, drift and brightness continuously with it.

```bash
ln -s ~/iris/plugin ~/.hermes/plugins/iris
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
├── iris.py              # Main entry point
├── animation_engine.py  # Core animation logic
├── states/
│   ├── thinking.py      # Thinking mode animations
│   └── idle.py          # Idle mode animations
└── utils/
    ├── terminal.py      # Terminal control utilities
    └── patterns.py      # Shared animation patterns
```

## License

MIT
