"""Idle state animations - slow-paced, subtle visualizations."""

import json
import math
import os
import time
import random

from utils.terminal import (
    get_terminal_size,
)
from utils.patterns import (
    sine_wave, cosine_wave, lerp, ease_in_out,
    distance, normalize, Palette
)


SESSIONS_FILE = os.environ.get('IRIS_SESSIONS_FILE', '/tmp/iris_sessions.json')


def load_sessions():
    """Load active sessions from the watcher's JSON file."""
    try:
        if os.path.exists(SESSIONS_FILE):
            with open(SESSIONS_FILE, 'r') as f:
                data = json.load(f)
                return data.get('sessions', [])
    except Exception:
        pass
    return []


class SessionOrbsAnimation:
    """Orbiting orbs representing active Hermes sessions."""

    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.time = 0.0
        self.orbs = []
        self._init_orbs()

    def _init_orbs(self):
        """Initialize orbs based on active sessions."""
        sessions = load_sessions()
        self.orbs = []

        center_x = self.width / 2
        center_y = self.height / 2
        max_radius = min(self.width, self.height) * 0.35

        for i, session in enumerate(sessions):
            # Evenly distribute orbs in a circle with varying radii
            angle = (i * 2 * math.pi / max(len(sessions), 1)) - math.pi / 2
            radius = max_radius * (0.4 + 0.6 * (i % 3) / 2)  # Vary radii slightly

            # Color based on session type
            color_name = session.get('color', 'cyan')
            if color_name == 'magenta':
                color = (255, 100, 200)
            elif color_name == 'amber':
                color = (255, 180, 50)
            else:
                color = (50, 200, 255)

            # Most recent session gets the "bright" orb
            is_primary = (i == 0)

            self.orbs.append({
                'x': center_x + math.cos(angle) * radius,
                'y': center_y + math.sin(angle) * radius,
                'base_angle': angle,
                'radius': radius,
                'orbit_radius': radius,
                'orbit_angle': angle,
                'color': color,
                'is_primary': is_primary,
                'phase': i * 0.5,
                'pulse_phase': i * 0.7,
                'label': session.get('label', '?')[:12],
                'short_label': session.get('label', '?')[:7],
                'idle': session.get('idle_seconds', 0),
                'source': session.get('source', 'cli'),
            })

    def update(self, delta_time):
        self.time += delta_time
        # Refresh orbs periodically
        if int(self.time) % 5 == 0 and int(self.time - delta_time) % 5 != 0:
            self._init_orbs()

    def render(self):
        """Render session orbs as orbiting circles with connections."""
        if not self.orbs:
            return '\n' * self.height

        grid = [[' ' for _ in range(self.width)] for _ in range(self.height)]
        color_grid = [[(0, 0, 0) for _ in range(self.width)] for _ in range(self.height)]

        # First pass: draw connections between orbs of the same source
        for i in range(len(self.orbs)):
            for j in range(i + 1, len(self.orbs)):
                orb1 = self.orbs[i]
                orb2 = self.orbs[j]
                
                # Only connect orbs of the same source type
                if orb1['source'] != orb2['source']:
                    continue
                
                orbit_speed1 = 0.15 if orb1['is_primary'] else 0.08
                angle1 = orb1['base_angle'] + self.time * orbit_speed1
                x1 = int(orb1['x'] + math.cos(angle1) * orb1['radius'] * 0.15)
                y1 = int(orb1['y'] + math.sin(angle1) * orb1['radius'] * 0.15)
                
                orbit_speed2 = 0.15 if orb2['is_primary'] else 0.08
                angle2 = orb2['base_angle'] + self.time * orbit_speed2
                x2 = int(orb2['x'] + math.cos(angle2) * orb2['radius'] * 0.15)
                y2 = int(orb2['y'] + math.sin(angle2) * orb2['radius'] * 0.15)
                
                dx = x2 - x1
                dy = y2 - y1
                dist = math.sqrt(dx * dx + dy * dy)
                
                if dist > 30:
                    continue
                
                # Connection color
                r, g, b = orb1['color']
                r, g, b = r / 255.0 * 0.3, g / 255.0 * 0.3, b / 255.0 * 0.3
                
                # Pulsing connection
                conn_pulse = 0.3 + 0.4 * math.sin(self.time * 1.5 + (i + j) * 0.5)
                alpha = conn_pulse * (1.0 - dist / 30.0)
                
                steps = int(dist)
                for step in range(steps + 1):
                    t = step / max(steps, 1)
                    lx = int(x1 + dx * t)
                    ly = int(y1 + dy * t)
                    if 0 <= lx < self.width and 0 <= ly < self.height:
                        cr, cg, cb = color_grid[ly][lx]
                        cr = min(255, cr + int(r * alpha * 255))
                        cg = min(255, cg + int(g * alpha * 255))
                        cb = min(255, cb + int(b * alpha * 255))
                        color_grid[ly][lx] = (cr, cg, cb)
                        if grid[ly][lx] == ' ':
                            grid[ly][lx] = '·'

        # Second pass: draw the orbs
        for orb in self.orbs:
            # Orbital motion
            orbit_speed = 0.15 if orb['is_primary'] else 0.08
            angle = orb['base_angle'] + self.time * orbit_speed
            x = int(orb['x'] + math.cos(angle) * orb['radius'] * 0.15)
            y = int(orb['y'] + math.sin(angle) * orb['radius'] * 0.15)

            # Pulsing size
            pulse = 1.0 + 0.3 * math.sin(self.time * 2.5 + orb['pulse_phase'])
            if orb['is_primary']:
                pulse *= 1.3

            # Draw orb as a small circle
            r_px = max(1, int(3 * pulse))
            r, g, b = orb['color']

            for dy in range(-r_px, r_px + 1):
                for dx in range(-r_px, r_px + 1):
                    px, py = x + dx, y + dy
                    if 0 <= px < self.width and 0 <= py < self.height:
                        dist = math.sqrt(dx * dx + dy * dy)
                        if dist <= r_px:
                            intensity = 1.0 - (dist / r_px)
                            if orb['is_primary'] and intensity > 0.7:
                                char = '●'
                            elif intensity > 0.7:
                                char = '○'
                            elif intensity > 0.4:
                                char = '◐'
                            else:
                                char = '·'
                            grid[py][px] = char
                            # Additive color blending
                            cr, cg, cb = color_grid[py][px]
                            cr = min(255, cr + int(r * intensity))
                            cg = min(255, cg + int(g * intensity))
                            cb = min(255, cb + int(b * intensity))
                            color_grid[py][px] = (cr, cg, cb)

            # Draw activity particles for recently active orbs
            if orb['idle'] < 60:
                for _ in range(2):
                    if random.random() < 0.3:
                        part_angle = random.random() * 2 * math.pi
                        part_dist = r_px + random.random() * 3
                        part_x = int(x + math.cos(part_angle) * part_dist)
                        part_y = int(y + math.sin(part_angle) * part_dist)
                        if 0 <= part_x < self.width and 0 <= part_y < self.height:
                            if grid[part_y][part_x] == ' ':
                                grid[part_y][part_x] = '·'
                                cr, cg, cb = color_grid[part_y][part_x]
                                color_grid[part_y][part_x] = (
                                    min(255, cr + r // 2),
                                    min(255, cg + g // 2),
                                    min(255, cb + b // 2)
                                )

            # Primary orb gets a subtle glow ring
            if orb['is_primary']:
                glow_r = r_px + 2
                for gy in range(y - glow_r, y + glow_r + 1):
                    for gx in range(x - glow_r, x + glow_r + 1):
                        if 0 <= gx < self.width and 0 <= gy < self.height:
                            dist = math.sqrt((gx - x) ** 2 + (gy - y) ** 2)
                            if glow_r - 1 <= dist <= glow_r:
                                intensity = 0.15 * (1.0 - abs(dist - glow_r))
                                cr, cg, cb = color_grid[gy][gx]
                                cr = min(255, cr + int(r * intensity))
                                cg = min(255, cg + int(g * intensity))
                                cb = min(255, cb + int(b * intensity))
                                color_grid[gy][gx] = (cr, cg, cb)
                                if grid[gy][gx] == ' ':
                                    grid[gy][gx] = '·'

            # Draw label for ALL orbs
            if orb['short_label']:
                label_x = x + r_px + 2
                label_y = y
                label_w = len(orb['short_label'])
                
                # Ensure label stays on screen
                if label_x + label_w >= self.width:
                    label_x = x - r_px - label_w - 2
                if label_y < 0:
                    label_y = 0
                if label_y >= self.height:
                    label_y = self.height - 1

                # Draw label background
                for dy in range(-1, 2):
                    for dx in range(-1, label_w + 1):
                        bg_x, bg_y = label_x + dx, label_y + dy
                        if 0 <= bg_x < self.width and 0 <= bg_y < self.height:
                            if grid[bg_y][bg_x] != ' ':
                                cr, cg, cb = color_grid[bg_y][bg_x]
                                color_grid[bg_y][bg_x] = (cr // 3, cg // 3, cb // 3)
                
                for i, ch in enumerate(orb['short_label']):
                    lx, ly = label_x + i, label_y
                    if 0 <= lx < self.width and 0 <= ly < self.height:
                        grid[ly][lx] = ch
                        color_grid[ly][lx] = (r, g, b)

                # Draw idle indicator
                if not orb['is_primary'] and orb['idle'] > 60:
                    dot_x = label_x + label_w + 1
                    dot_y = label_y
                    if 0 <= dot_x < self.width and 0 <= dot_y < self.height:
                        idle_color = (255, 100, 50) if orb['idle'] > 300 else (255, 200, 50)
                        grid[dot_y][dot_x] = '●'
                        color_grid[dot_y][dot_x] = idle_color

        # Convert to ANSI colored output
        lines = []
        for y in range(self.height):
            line = ""
            for x in range(self.width):
                ch = grid[y][x]
                if ch != ' ':
                    r, g, b = color_grid[y][x]
                    line += f"\033[38;2;{r};{g};{b}m{ch}\033[0m"
                else:
                    line += ' '
            lines.append(line)

        return '\n'.join(lines)


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
        self.session_orbs = SessionOrbsAnimation(self.width, self.height)

    def update(self, delta_time):
        """Update all animation layers."""
        self.time += delta_time
        self.frame_count += 1

        self.pulse.update(delta_time * 0.3)
        self.breathing.update(delta_time * 0.3)
        self.flow.update(delta_time * 0.3)
        self.session_orbs.update(delta_time)

        # Update dimensions in case of resize
        new_w, new_h = get_terminal_size()
        self.width, self.height = new_w, new_h - 2

    def render(self):
        """Render the complete idle state animation."""
        # Get all layer renders
        pulse_render = self.pulse.render()
        breathing_render = self.breathing.render()
        flow_render = self.flow.render()
        orbs_render = self.session_orbs.render()

        # Merge layers
        output_lines = []
        palette = Palette.IDLE
        palette_len = len(palette)

        # Split each layer into lines and pad to full width
        pulse_lines = [line.ljust(self.width) for line in pulse_render.split('\n')]
        breathing_lines = [line.ljust(self.width) for line in breathing_render.split('\n')]
        flow_lines = [line.ljust(self.width) for line in flow_render.split('\n')]
        orbs_lines = [line.ljust(self.width) for line in orbs_render.split('\n')]

        for y in range(self.height):
            line = ""
            for x in range(self.width):
                p_char = pulse_lines[y][x] if y < len(pulse_lines) else ' '
                b_char = breathing_lines[y][x] if y < len(breathing_lines) else ' '
                f_char = flow_lines[y][x] if y < len(flow_lines) else ' '
                o_char = orbs_lines[y][x] if y < len(orbs_lines) else ' '

                # Priority: orbs > breathing > pulse > flow
                if o_char != ' ':
                    char = o_char
                    r, g, b = palette[0]
                elif b_char != ' ':
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

        # Add status line with session count
        sessions = load_sessions()
        session_info = f" {len(sessions)} session{'s' if len(sessions) != 1 else ''} "
        elapsed = self.time
        status = f" [ IDLE ]{session_info}• {elapsed:.1f}s • frame {self.frame_count} "
        status_line = status.center(self.width, '─')

        output_lines.append('')
        output_lines.append(status_line)

        return '\n'.join(output_lines)

    @property
    def frame_delay(self):
        """Frame delay in seconds for idle mode."""
        return 0.2  # 5 FPS