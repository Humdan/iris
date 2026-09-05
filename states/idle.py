"""Idle state animations - slow-paced, subtle visualizations."""

import math

from utils.terminal import (
    get_terminal_size,
)
from utils.patterns import (
    sine_wave, cosine_wave, lerp, ease_in_out,
    distance, normalize, Palette
)


class SlowPulseAnimation:
    """A gentle, pulsing wave that moves slowly across the terminal."""
    
    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.time = 0.0
        self.center_x = width / 2
        self.center_y = height / 2
    
    def update(self, delta_time):
        self.time += delta_time
    
    def render(self):
        """Render slow pulse animation."""
        lines = []
        palette = Palette.IDLE
        palette_len = len(palette)
        
        for y in range(self.height):
            line = ""
            for x in range(self.width):
                # Calculate distance from center
                dx = x - self.center_x
                dy = y - self.center_y
                dist = math.sqrt(dx * dx + dy * dy)
                
                # Slow moving radial wave
                wave = sine_wave(dist * 0.2 - self.time * 0.3, 0.5, 0.3)
                
                intensity = normalize(wave + 0.5, 0, 1)
                
                if intensity > 0.15:
                    color_idx = min(int(intensity * (palette_len - 1)), palette_len - 1)
                    r, g, b = palette[color_idx]
                    
                    if intensity > 0.8:
                        char = '▓'
                    elif intensity > 0.5:
                        char = '▒'
                    elif intensity > 0.3:
                        char = '░'
                    else:
                        char = '·'
                    
                    line += char
                else:
                    line += ' '
            lines.append(line)
        
        return '\n'.join(lines)


class BreathingAnimation:
    """A breathing-like pulse animation centered in the terminal."""
    
    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.time = 0.0
        self.center_x = width / 2
        self.center_y = height / 2
    
    def update(self, delta_time):
        self.time += delta_time
    
    def render(self):
        """Render breathing animation."""
        grid = [[' ' for _ in range(self.width)] for _ in range(self.height)]
        
        # Slow breathing cycle (one breath every 6 seconds)
        breath_cycle = sine_wave(self.time * 0.3, 1.0, 1.0)
        breath_intensity = (breath_cycle + 1) / 2  # Normalize to 0-1
        
        palette = Palette.IDLE
        palette_len = len(palette)
        
        # Draw expanding/constricting circle
        max_radius = min(self.width, self.height) * 0.3
        radius = max_radius * (0.5 + breath_intensity * 0.5)
        
        # Draw circle outline
        circle_thickness = 2.0
        for y in range(self.height):
            for x in range(self.width):
                dist = distance(x, y, self.center_x, self.center_y)
                edge_dist = abs(dist - radius)
                
                if edge_dist < circle_thickness / 2:
                    intensity = 1 - (edge_dist / (circle_thickness / 2))
                    color_idx = min(int(intensity * (palette_len - 1)), palette_len - 1)
                    
                    if intensity > 0.6:
                        grid[y][x] = '○'
                    elif intensity > 0.3:
                        grid[y][x] = '·'
                    else:
                        grid[y][x] = ' '
        
        return '\n'.join(''.join(row) for row in grid)


class GentleFlowAnimation:
    """Subtle horizontal flow pattern."""
    
    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.time = 0.0
    
    def update(self, delta_time):
        self.time += delta_time
    
    def render(self):
        """Render gentle flow animation."""
        lines = []
        palette = Palette.IDLE
        palette_len = len(palette)
        
        for y in range(self.height):
            line = ""
            for x in range(self.width):
                # Slow horizontal sine wave
                wave = sine_wave(self.time * 0.5 + x * 0.05 + y * 0.03, 1.0, 0.3)
                intensity = normalize(wave + 0.5, 0, 1)
                
                if intensity > 0.2:
                    color_idx = min(int(intensity * (palette_len - 1)), palette_len - 1)
                    
                    if intensity > 0.8:
                        char = '═'
                    elif intensity > 0.5:
                        char = '─'
                    elif intensity > 0.3:
                        char = '╌'
                    else:
                        char = '·'
                    
                    line += char
                else:
                    line += ' '
            lines.append(line)
        
        return '\n'.join(lines)


class IdleState:
    """Manages the idle state with subtle animations."""
    
    def __init__(self):
        self.width, self.height = get_terminal_size()
        self.height = self.height - 2
        
        self.time = 0.0
        self.frame_count = 0
        
        # Initialize animation layers
        self.pulse = SlowPulseAnimation(self.width, self.height)
        self.breathing = BreathingAnimation(self.width, self.height)
        self.flow = GentleFlowAnimation(self.width, self.height)
    
    def update(self, delta_time):
        """Update all animation layers."""
        self.time += delta_time
        self.frame_count += 1
        
        self.pulse.update(delta_time * 0.3)
        self.breathing.update(delta_time * 0.3)
        self.flow.update(delta_time * 0.3)
        
        # Update dimensions in case of resize
        new_w, new_h = get_terminal_size()
        self.width, self.height = new_w, new_h - 2
    
    def render(self):
        """Render the complete idle state animation."""
        # Get all layer renders
        pulse_render = self.pulse.render()
        breathing_render = self.breathing.render()
        flow_render = self.flow.render()
        
        # Merge layers
        output_lines = []
        palette = Palette.IDLE
        palette_len = len(palette)
        
        # Split each layer into lines and pad to full width
        pulse_lines = [line.ljust(self.width) for line in pulse_render.split('\n')]
        breathing_lines = [line.ljust(self.width) for line in breathing_render.split('\n')]
        flow_lines = [line.ljust(self.width) for line in flow_render.split('\n')]
        
        for y in range(self.height):
            line = ""
            for x in range(self.width):
                p_char = pulse_lines[y][x] if y < len(pulse_lines) else ' '
                b_char = breathing_lines[y][x] if y < len(breathing_lines) else ' '
                f_char = flow_lines[y][x] if y < len(flow_lines) else ' '
                
                # Priority: breathing > pulse > flow
                if b_char != ' ':
                    char = b_char
                    r, g, b = palette[0]
                elif p_char != ' ':
                    char = p_char
                    r, g, b = palette[1]
                elif f_char != ' ':
                    char = f_char
                    r, g, b = palette[2]
                else:
                    char = ' '
                    r, g, b = (0, 0, 0)
                
                line += char
            
            output_lines.append(line)
        
        # Add status line
        elapsed = self.time
        status = f" [ IDLE ] • {elapsed:.1f}s • frame {self.frame_count} "
        status_line = status.center(self.width, '─')
        
        output_lines.append('')
        output_lines.append(status_line)
        
        return '\n'.join(output_lines)
    
    @property
    def frame_delay(self):
        """Frame delay in seconds for idle mode."""
        return 0.2  # 5 FPS
