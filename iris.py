#!/usr/bin/env python3
"""Iris - Terminal Animation for AI Activity Visualization

A terminal-based animation system that visualizes AI agent activity states.
Displays dynamic patterns when "thinking" and subtle animations when "idle."

Usage:
    python3 iris.py                  # Run with interactive mode (toggle with spacebar)
    python3 iris.py --daemon         # Run as daemon, monitor state file
    python3 iris.py --daemon --state-file /tmp/iris_state

External control (when using --daemon):
    echo "thinking" > /tmp/iris_state
    echo "idle" > /tmp/iris_state
"""

import argparse
import sys
import os

# Ensure we can import local modules
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from animation_engine import run_iris, AnimationEngine
from utils.terminal import is_terminal


def main():
    parser = argparse.ArgumentParser(
        description="Iris - Terminal animation for AI activity visualization",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
States:
  thinking  - Fast-paced, dynamic animations (waves, neural networks, particles)
  idle      - Slow, gentle animations (pulses, breathing, flows)

Control:
  Press SPACE to toggle states in interactive mode.
  Press Q to quit.
  In daemon mode, write 'thinking' or 'idle' to the state file to change states.
        """
    )
    
    parser.add_argument(
        '--daemon',
        action='store_true',
        help='Run as daemon monitoring a state file'
    )
    
    parser.add_argument(
        '--state-file',
        default='/tmp/iris_state',
        help='Path to the state file (default: /tmp/iris_state)'
    )
    
    parser.add_argument(
        '--initial-state',
        choices=['idle', 'thinking'],
        default='idle',
        help='Initial animation state (default: idle)'
    )
    
    parser.add_argument(
        '--framebuffer',
        action='store_true',
        help='Render directly to /dev/fb0 (Pi LCD, no X11 needed)'
    )

    args = parser.parse_args()
    render_mode = 'framebuffer' if args.framebuffer else 'terminal'
    
    if not is_terminal() and not args.framebuffer:
        print("Warning: Output is not a terminal. Animation may not render correctly.")
        print("Run this in a terminal for best experience.")
    
    if args.daemon:
        print(f"Starting Iris in daemon mode. State file: {args.state_file}")
        print(f"Control with: echo 'thinking' > {args.state_file}")
        print(f"Or:           echo 'idle' > {args.state_file}")
        print("Press Ctrl+C to stop.")
        
        # Initialize state file
        with open(args.state_file, 'w') as f:
            f.write(args.initial_state)
        
        run_iris(state_file=args.state_file, render_mode=render_mode)
    else:
        print("Iris - Terminal AI Activity Visualizer")
        print(f"Initial state: {args.initial_state}")
        print("Controls:")
        print("  SPACE - Toggle between idle and thinking states")
        print("  Q     - Quit")
        print("  Ctrl+C - Quit")
        print()
        print("Starting animation...")
        
        engine = AnimationEngine(render_mode=render_mode)
        engine.set_state(args.initial_state)
        engine.run()
    
    print("\nIris stopped. Goodbye!")


if __name__ == '__main__':
    main()
