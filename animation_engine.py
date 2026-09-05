"""Core animation engine - manages states and rendering loop."""

import sys
import os
import time
import threading
import importlib.util

# Add project root to path
project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, project_root)

from utils.terminal import (
    terminal_context, clear_screen, hide_cursor, show_cursor, reset_colors,
    write_raw, get_terminal_size, move_cursor, clear_to_end, set_fg_color
)
from utils.patterns import Palette

from states.thinking import ThinkingState
from states.idle import IdleState


class AnimationEngine:
    """Main animation engine that manages state transitions and rendering."""
    
    def __init__(self):
        self.state = "idle"  # Start in idle state
        self.running = True
        self.last_state_change = time.time()
        self.idle_state = IdleState()
        self.thinking_state = ThinkingState()
        self.current_animation = self.idle_state
        self.start_time = time.time()
        self.activity_log = []  # Track state changes for logging
        
    def set_state(self, state_name):
        """Change the current animation state."""
        if state_name not in ("idle", "thinking"):
            raise ValueError(f"Unknown state: {state_name}")
        
        if state_name != self.state:
            self.state = state_name
            self.last_state_change = time.time()
            
            # Switch to the appropriate animation state
            if state_name == "thinking":
                self.current_animation = self.thinking_state
            else:
                self.current_animation = self.idle_state
            
            # Log the state change
            self.activity_log.append({
                'time': time.time() - self.start_time,
                'from': self.current_animation.__class__.__name__,
                'to': state_name,
                'elapsed': time.time() - self.last_state_change
            })
        
    def get_current_animation(self):
        """Get the current animation instance."""
        return self.current_animation
    
    def render_frame(self):
        """Render a single frame of the current animation."""
        animation = self.get_current_animation()
        return animation.render()
    
    def update(self, delta_time):
        """Update the current animation."""
        animation = self.get_current_animation()
        animation.update(delta_time)
    
    @property
    def frame_delay(self):
        """Get the frame delay for the current state."""
        return self.get_current_animation().frame_delay
    
    def run(self):
        """Run the main animation loop."""
        try:
            with terminal_context():
                clear_screen()
                
                last_time = time.time()
                
                while self.running:
                    current_time = time.time()
                    delta_time = current_time - last_time
                    last_time = current_time
                    
                    # Check if terminal was resized
                    new_w, new_h = get_terminal_size()
                    
                    # Update animation
                    self.update(delta_time)
                    
                    # Render
                    move_cursor(1, 1)
                    frame = self.render_frame()
                    write_raw(frame)
                    clear_to_end()
                    
                    # Wait for next frame
                    time.sleep(self.frame_delay)
                    
                    # Check for keyboard input (non-blocking)
                    self._handle_input()
                    
        except KeyboardInterrupt:
            pass
        finally:
            show_cursor()
            reset_colors()
            clear_screen()
    
    def _handle_input(self):
        """Handle keyboard input (non-blocking)."""
        # Check for keyboard input without blocking
        import select
        if select.select([sys.stdin], [], [], 0) == ([], [], []):
            return
        
        # Read the key
        try:
            key = sys.stdin.read(1)
            if key == 'q' or key == 'Q':
                self.running = False
            elif key == ' ':
                # Toggle state on spacebar
                if self.state == "idle":
                    self.set_state("thinking")
                else:
                    self.set_state("idle")
        except Exception:
            pass


def run_iris(state_file=None):
    """Run the Iris animation engine.
    
    Args:
        state_file: Optional path to a file that can be used to control
                   the animation state from external processes.
    """
    engine = AnimationEngine()
    
    if state_file:
        # Start a thread to monitor state file
        def monitor_state_file():
            last_modified = 0
            last_content = ""
            while engine.running:
                try:
                    if os.path.exists(state_file):
                        mtime = os.path.getmtime(state_file)
                        if mtime != last_modified:
                            last_modified = mtime
                            with open(state_file, 'r') as f:
                                content = f.read().strip().lower()
                                if content != last_content:
                                    last_content = content
                                    if content == "thinking":
                                        engine.set_state("thinking")
                                    elif content == "idle":
                                        engine.set_state("idle")
                except Exception:
                    pass
                time.sleep(0.1)
        
        state_thread = threading.Thread(target=monitor_state_file, daemon=True)
        state_thread.start()
    
    engine.run()
