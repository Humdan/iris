"""Thinking state animations - fast-paced, dynamic visualizations."""

import json
import math
import os
import time
import random
from collections import deque

from utils.terminal import (
    get_terminal_size, write_raw, set_fg_color, reset_colors,
    move_cursor, clear_to_end
)
from utils.patterns import (
    sine_wave, cosine_wave, lerp, ease_in_out, ease_out_elastic,
    generate_neural_pattern, connection_strength,
    distance, distance_sq, normalize, Palette
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
    """Orbiting orbs representing active Hermes sessions (thinking mode)."""

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
            # Evenly distribute orbs in a circle
            angle = (i * 2 * math.pi / max(len(sessions), 1)) - math.pi / 2
            radius = max_radius * (0.4 + 0.6 * (i % 3) / 2)

            # Color based on session type - brighter in thinking mode
            color_name = session.get('color', 'cyan')
            if color_name == 'magenta':
                color = (255, 150, 220)
            elif color_name == 'amber':
                color = (255, 210, 80)
            else:
                color = (100, 230, 255)

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
        """Render session orbs as orbiting circles with trails and connections."""
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
                
                orbit_speed1 = 0.4 if orb1['is_primary'] else 0.25
                angle1 = orb1['base_angle'] + self.time * orbit_speed1
                x1 = int(orb1['x'] + math.cos(angle1) * orb1['radius'] * 0.2)
                y1 = int(orb1['y'] + math.sin(angle1) * orb1['radius'] * 0.2)
                
                orbit_speed2 = 0.4 if orb2['is_primary'] else 0.25
                angle2 = orb2['base_angle'] + self.time * orbit_speed2
                x2 = int(orb2['x'] + math.cos(angle2) * orb2['radius'] * 0.2)
                y2 = int(orb2['y'] + math.sin(angle2) * orb2['radius'] * 0.2)
                
                dx = x2 - x1
                dy = y2 - y1
                dist = math.sqrt(dx * dx + dy * dy)
                
                if dist > 40:
                    continue
                
                # Connection color - brighter in thinking mode
                r, g, b = orb1['color']
                r, g, b = r / 255.0 * 0.5, g / 255.0 * 0.5, b / 255.0 * 0.5
                
                # Pulsing connection - faster in thinking mode
                conn_pulse = 0.4 + 0.5 * math.sin(self.time * 3.0 + (i + j) * 0.5)
                alpha = conn_pulse * (1.0 - dist / 40.0)
                
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
                            grid[ly][lx] = '∘'

        # Second pass: draw the orbs with trails
        for orb in self.orbs:
            # Faster orbital motion in thinking mode
            orbit_speed = 0.4 if orb['is_primary'] else 0.25
            angle = orb['base_angle'] + self.time * orbit_speed
            x = int(orb['x'] + math.cos(angle) * orb['radius'] * 0.2)
            y = int(orb['y'] + math.sin(angle) * orb['radius'] * 0.2)

            # Pulsing size - more intense in thinking mode
            pulse = 1.0 + 0.5 * math.sin(self.time * 4.0 + orb['pulse_phase'])
            if orb['is_primary']:
                pulse *= 1.5

            # Draw orb with trail effect
            r_px = max(1, int(3 * pulse))
            r, g, b = orb['color']

            for dy in range(-r_px, r_px + 1):
                for dx in range(-r_px, r_px + 1):
                    px, py = x + dx, y + dy
                    if 0 <= px < self.width and 0 <= py < self.height:
                        dist = math.sqrt(dx * dx + dy * dy)
                        if dist <= r_px:
                            intensity = 1.0 - (dist / r_px)
                            # Brighter, more energetic chars in thinking mode
                            if orb['is_primary'] and intensity > 0.6:
                                char = '●'
                            elif intensity > 0.6:
                                char = '◉'
                            elif intensity > 0.3:
                                char = '◎'
                            else:
                                char = '∘'
                            grid[py][px] = char
                            # Additive color blending
                            cr, cg, cb = color_grid[py][px]
                            cr = min(255, cr + int(r * intensity))
                            cg = min(255, cg + int(g * intensity))
                            cb = min(255, cb + int(b * intensity))
                            color_grid[py][px] = (cr, cg, cb)

            # Draw activity particles for recently active orbs (more in thinking mode)
            if orb['idle'] < 60:
                for _ in range(3):
                    if random.random() < 0.5:
                        part_angle = random.random() * 2 * math.pi
                        part_dist = r_px + random.random() * 5
                        part_x = int(x + math.cos(part_angle) * part_dist)
                        part_y = int(y + math.sin(part_angle) * part_dist)
                        if 0 <= part_x < self.width and 0 <= part_y < self.height:
                            if grid[part_y][part_x] == ' ':
                                grid[part_y][part_x] = '∘'
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
                                intensity = 0.2 * (1.0 - abs(dist - glow_r))
                                cr, cg, cb = color_grid[gy][gx]
                                cr = min(255, cr + int(r * intensity))
                                cg = min(255, cg + int(g * intensity))
                                cb = min(255, cb + int(b * intensity))
                                color_grid[gy][gx] = (cr, cg, cb)
                                if grid[gy][gx] == ' ':
                                    grid[gy][gx] = '∘'

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


class WaveAnimation:
    """Animated wave pattern that propagates across the terminal."""

    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.time = 0.0

    def update(self, delta_time):
        self.time += delta_time

    def render(self):
        """Render wave pattern as a grid of characters."""
        lines = []
        palette = Palette.THINKING
        palette_len = len(palette)

        for y in range(self.height):
            line = ""
            for x in range(self.width):
                # Calculate wave influence from multiple sources
                waves = [
                    sine_wave(self.time * 3.0 + x * 0.1 + y * 0.05, 2.0, 0.5),
                    cosine_wave(self.time * 2.5 + x * 0.15 + y * 0.08, 1.5, 0.4),
                    sine_wave(self.time * 4.0 + (x + y) * 0.07, 1.0, 0.3),
                ]

                # Combine waves
                combined = sum(waves) / len(waves)
                intensity = normalize(combined, -1, 1)

                # Map intensity to color
                if intensity > 0.1:
                    color_idx = int(intensity * (palette_len - 1))
                    r, g, b = palette[color_idx]

                    # Use different characters based on intensity
                    intensity_normalized = intensity
                    if intensity_normalized > 0.7:
                        char = '█'
                    elif intensity_normalized > 0.4:
                        char = '▓'
                    elif intensity_normalized > 0.2:
                        char = '▒'
                    else:
                        char = '░'

                    line += char
                else:
                    line += ' '
            lines.append(line)

        return '\n'.join(lines)


class NeuralNetworkAnimation:
    """Neural network-inspired animation with nodes and connections."""

    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.time = 0.0
        self.nodes = []
        self._init_nodes()

    def _init_nodes(self):
        """Initialize network nodes in a grid pattern."""
        cols = 8
        rows = 6
        margin_x = self.width * 0.1
        margin_y = self.height * 0.2

        for row in range(rows):
            for col in range(cols):
                x = margin_x + (self.width - 2 * margin_x) * col / (cols - 1)
                y = margin_y + (self.height - 2 * margin_y) * row / (rows - 1)

                # Add some perturbation
                x += math.sin(row + col) * 2
                y += math.cos(row * 2 + col) * 2

                self.nodes.append({
                    'x': x,
                    'y': y,
                    'phase': (row + col) * 0.5,
                    'frequency': 0.8 + (row * col) * 0.1 % 1.0
                })

    def update(self, delta_time):
        self.time += delta_time

    def render(self):
        """Render the neural network visualization."""
        grid = [[' ' for _ in range(self.width)] for _ in range(self.height)]

        palette = Palette.THINKING
        palette_len = len(palette)
        pulse_palette = Palette.PULSE_THINKING

        # Draw connections
        for i, node1 in enumerate(self.nodes):
            for j, node2 in enumerate(self.nodes):
                if i >= j:
                    continue

                dist = distance(node1['x'], node1['y'], node2['x'], node2['y'])
                if dist > self.width * 0.2:  # Only connect nearby nodes
                    continue

                # Calculate connection strength with wave effect
                strength = connection_strength(
                    (node1['x'], node1['y']),
                    (node2['x'], node2['y']),
                    self.time
                )

                if strength > 0.3:
                    # Draw line between nodes
                    steps = int(dist)
                    for step in range(steps):
                        t = step / max(steps - 1, 1)
                        x = int(lerp(node1['x'], node2['x'], t))
                        y = int(lerp(node1['y'], node2['y'], t))

                        if 0 <= x < self.width and 0 <= y < self.height:
                            if grid[y][x] == ' ':
                                intensity = normalize(strength, 0.3, 1.0)
                                color_idx = min(int(intensity * (palette_len - 1)), palette_len - 1)
                                r, g, b = pulse_palette[color_idx]

                                # Use different chars for connection strength
                                if intensity > 0.7:
                                    grid[y][x] = '·'
                                elif intensity > 0.5:
                                    grid[y][x] = '∘'
                                else:
                                    grid[y][x] = '·'

        # Draw nodes
        for node in self.nodes:
            x, y = int(node['x']), int(node['y'])
            if 0 <= x < self.width and 0 <= y < self.height:
                # Node pulsing effect
                pulse = abs(sine_wave(self.time * 5.0 + node['phase'], node['frequency'], 1.0))
                intensity = (pulse + 1) / 2  # Normalize to 0-1

                if intensity > 0.8:
                    char = '●'
                elif intensity > 0.5:
                    char = '◐'
                elif intensity > 0.3:
                    char = '◑'
                else:
                    char = '○'

                if 0 <= y < self.height and 0 <= x < self.width:
                    grid[y][x] = char

        return '\n'.join(''.join(row) for row in grid)


class ParticleSystem:
    """Particle system for energetic "thinking" effects."""

    def __init__(self, width, height, particle_count=30):
        self.width = width
        self.height = height
        self.time = 0.0
        self.particles = []

        import random
        random.seed(42)  # Fixed seed for reproducibility

        for i in range(particle_count):
            self.particles.append({
                'x': random.uniform(0, width),
                'y': random.uniform(0, height),
                'vx': random.uniform(-0.5, 0.5),
                'vy': random.uniform(-0.3, 0.3),
                'life': random.uniform(1.0, 3.0),
                'max_life': random.uniform(1.0, 3.0),
                'idx': i
            })

    def reset_particle(self, p):
        """Reset a particle to a new random position."""
        import random
        idx = p['idx']
        random.seed(idx * 1099511628237 + 42)
        p['x'] = random.uniform(0, self.width)
        p['y'] = random.uniform(0, self.height)
        p['vx'] = random.uniform(-0.5, 0.5)
        p['vy'] = random.uniform(-0.3, 0.3)
        p['life'] = random.uniform(1.0, 3.0)
        p['max_life'] = random.uniform(1.0, 3.0)

    def update(self, delta_time):
        self.time += delta_time

        for p in self.particles:
            p['x'] += p['vx'] * (1 + self.time * 0.01)
            p['y'] += p['vy'] * (0.8 + self.time * 0.005)
            p['life'] -= delta_time

            if p['life'] <= 0 or p['x'] < 0 or p['x'] >= self.width or p['y'] < 0 or p['y'] >= self.height:
                self.reset_particle(p)

    def render(self):
        """Render particles to a grid."""
        grid = [[' ' for _ in range(self.width)] for _ in range(self.height)]

        pulse_palette = Palette.PULSE_THINKING

        for p in self.particles:
            x, y = int(p['x']), int(p['y'])
            if 0 <= x < self.width and 0 <= y < self.height:
                life_ratio = p['life'] / p['max_life']
                char_idx = min(int(life_ratio * 3), 2)
                char = '◦' if char_idx == 0 else ('∘' if char_idx == 1 else '•')
                grid[y][x] = char

        return '\n'.join(''.join(row) for row in grid)


class ThinkingState:
    """Manages the thinking state with multiple animation layers."""

    def __init__(self):
        self.width, self.height = get_terminal_size()
        # Adjust height for status line
        self.height = self.height - 2

        self.time = 0.0
        self.frame_count = 0

        # Initialize animation layers
        self.waves = WaveAnimation(self.width, self.height)
        self.neural_net = NeuralNetworkAnimation(self.width, self.height)
        self.particles = ParticleSystem(self.width, self.height, particle_count=40)
        self.session_orbs = SessionOrbsAnimation(self.width, self.height)

    def update(self, delta_time):
        """Update all animation layers."""
        self.time += delta_time
        self.frame_count += 1

        # Update each layer
        self.waves.update(delta_time * 1.5)
        self.neural_net.update(delta_time * 0.8)
        self.particles.update(delta_time)
        self.session_orbs.update(delta_time)

        # Update width/height in case terminal was resized
        new_w, new_h = get_terminal_size()
        self.width, self.height = new_w, new_h - 2

    def render(self):
        """Render the complete thinking state animation."""
        # Combine layers
        layers = []

        # Layer 1: Wave background (subtler)
        wave_render = self.waves.render()

        # Layer 2: Neural network (more prominent)
        neural_render = self.neural_net.render()

        # Layer 3: Particles
        particle_render = self.particles.render()

        # Layer 4: Session orbs (highest priority)
        orbs_render = self.session_orbs.render()

        # Merge layers by overlaying characters
        output_lines = []
        palette = Palette.THINKING
        palette_len = len(palette)

        # Split each layer into lines and pad to full width
        wave_lines = [line.ljust(self.width) for line in wave_render.split('\n')]
        neural_lines = [line.ljust(self.width) for line in neural_render.split('\n')]
        particle_lines = [line.ljust(self.width) for line in particle_render.split('\n')]
        orbs_lines = [line.ljust(self.width) for line in orbs_render.split('\n')]

        for y in range(self.height):
            line = ""
            for x in range(self.width):
                # Get characters from each layer
                wave_char = wave_lines[y][x] if y < len(wave_lines) else ' '
                neural_char = neural_lines[y][x] if y < len(neural_lines) else ' '
                particle_char = particle_lines[y][x] if y < len(particle_lines) else ' '
                orbs_char = orbs_lines[y][x] if y < len(orbs_lines) else ' '

                # Priority: orbs > particles > neural > waves
                if orbs_char != ' ':
                    char = orbs_char
                    r, g, b = palette[0]
                elif particle_char != ' ':
                    char = particle_char
                    r, g, b = palette[0]
                elif neural_char != ' ':
                    char = neural_char
                    r, g, b = palette[1]
                elif wave_char != ' ':
                    char = wave_char
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
        status = f" [ THINKING ]{session_info}• {elapsed:.1f}s • frame {self.frame_count} "
        status_line = status.center(self.width, '─')

        output_lines.append('')
        output_lines.append(status_line)

        return '\n'.join(output_lines)

    @property
    def frame_delay(self):
        """Frame delay in seconds for thinking mode."""
        return 0.05  # 20 FPS