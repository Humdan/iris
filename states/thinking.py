"""Thinking state animations - fast-paced, dynamic visualizations."""

import math
import time
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
    
    def update(self, delta_time):
        """Update all animation layers."""
        self.time += delta_time
        self.frame_count += 1
        
        # Update each layer
        self.waves.update(delta_time * 1.5)
        self.neural_net.update(delta_time * 0.8)
        self.particles.update(delta_time)
        
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
        
        # Layer 3: Particles (highest layer)
        particle_render = self.particles.render()
        
        # Merge layers by overlaying characters
        output_lines = []
        palette = Palette.THINKING
        palette_len = len(palette)
        
        # Split each layer into lines and pad to full width
        wave_lines = [line.ljust(self.width) for line in wave_render.split('\n')]
        neural_lines = [line.ljust(self.width) for line in neural_render.split('\n')]
        particle_lines = [line.ljust(self.width) for line in particle_render.split('\n')]
        
        for y in range(self.height):
            line = ""
            for x in range(self.width):
                # Get characters from each layer
                wave_char = wave_lines[y][x] if y < len(wave_lines) else ' '
                neural_char = neural_lines[y][x] if y < len(neural_lines) else ' '
                particle_char = particle_lines[y][x] if y < len(particle_lines) else ' '
                
                # Priority: particles > neural > waves
                if particle_char != ' ':
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
        
        # Add status line
        elapsed = self.time
        status = f" [ THINKING ] • {elapsed:.1f}s • frame {self.frame_count} "
        status_line = status.center(self.width, '─')
        
        output_lines.append('')
        output_lines.append(status_line)
        
        return '\n'.join(output_lines)
    
    @property
    def frame_delay(self):
        """Frame delay in seconds for thinking mode."""
        return 0.05  # 20 FPS
