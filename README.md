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

Iris can draw straight to the framebuffer (`/dev/fb0`), so it works on a
headless Pi with an LCD and no X11:

```bash
python3 iris.py --daemon --framebuffer     # draw to the LCD
sudo ./install.sh                           # install + enable systemd service
iris-state thinking                         # switch animation
iris-state idle
```

The service takes over tty1, starts in idle, and restarts on failure.
Hook an agent to it by calling `iris-state thinking` when work starts and
`iris-state idle` when it finishes.

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
