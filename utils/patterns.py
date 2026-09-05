"""Shared animation patterns and visual elements."""

import math
from .terminal import get_terminal_size


def lerp(a, b, t):
    """Linear interpolation between a and b."""
    return a + (b - a) * t


def ease_in_out(t):
    """Smooth easing function for natural motion."""
    return 0.5 * (1 - math.cos(math.pi * t))


def ease_out_elastic(t):
    """Elastic easing for bouncy effects."""
    if t <= 0 or t >= 1:
        return t
    return math.pow(2, -10 * t) * math.sin((t - 0.075) * (2 * math.pi) / 0.3) + 1


def ease_in_out_cubic(t):
    """Cubic easing for smooth transitions."""
    return 4 * t * t * t if t < 1 else 1 - pow(-2 * t + 2, 3) / 2


def sine_wave(t, frequency=1.0, amplitude=1.0, phase=0.0):
    """Generate a sine wave value."""
    return math.sin(t * frequency + phase) * amplitude


def cosine_wave(t, frequency=1.0, amplitude=1.0, phase=0.0):
    """Generate a cosine wave value."""
    return math.cos(t * frequency + phase) * amplitude


def distance(x1, y1, x2, y2):
    """Calculate Euclidean distance between two points."""
    return math.sqrt((x2 - x1) ** 2 + (y2 - y1) ** 2)


def distance_sq(x1, y1, x2, y2):
    """Calculate squared distance (avoids sqrt)."""
    return (x2 - x1) ** 2 + (y2 - y1) ** 2


def normalize(value, min_val, max_val):
    """Normalize a value to 0-1 range."""
    if max_val == min_val:
        return 0.5
    return max(0, min(1, (value - min_val) / (max_val - min_val)))


# Braille patterns for fine-grained terminal graphics
# Using Unicode braille characters to achieve sub-character resolution
class BrailleCanvas:
    """Canvas that uses braille characters for higher resolution rendering."""
    
    BRAILLE_BASE = 0x2800
    
    def __init__(self, width=40, height=20):
        self.width = width
        self.height = height
        # Each braille character represents 2x4 pixel grid
        self.canvas_width = math.ceil(width / 2)
        self.canvas_height = math.ceil(height / 4)
        self.pixels = [[0 for _ in range(self.canvas_width)] for _ in range(self.canvas_height)]
    
    def set_pixel(self, x, y, value):
        """Set a pixel value (0 or 1)."""
        if 0 <= x < self.width and 0 <= y < self.height:
            # Map pixel coordinates to braille array coordinates
            col = x // 2
            row = y // 4
            if 0 <= col < self.canvas_width and 0 <= row < self.canvas_height:
                self.pixels[row][col] |= value
    
    def get_char(self, row, col):
        """Get the braille character for a given cell."""
        if 0 <= row < self.canvas_height and 0 <= col < self.canvas_width:
            pixel_value = self.pixels[row][col]
            return chr(self.BRAILLE_BASE + pixel_value)
        return ' '
    
    def render(self):
        """Render the canvas to a string."""
        lines = []
        for row in range(self.canvas_height):
            line = ''.join(self.get_char(row, col) for col in range(self.canvas_width))
            lines.append(line)
        return '\n'.join(lines)


# Neural network-inspired patterns
def generate_neural_pattern(width, height, t, node_count=5):
    """Generate a pattern resembling neural network activity."""
    nodes = []
    for i in range(node_count):
        angle = (i * 2 * math.pi / node_count) + t * 0.3
        radius = min(width, height) * 0.25
        x = width / 2 + math.cos(angle) * radius
        y = height / 2 + math.sin(angle) * radius
        nodes.append((x, y))
    
    return nodes


def connection_strength(node1, node2, time_offset=0):
    """Calculate connection strength between two nodes."""
    distance_val = distance(node1[0], node1[1], node2[0], node2[1])
    # Pulsating connection based on distance and time
    return abs(sine_wave(distance_val * 0.1 - time_offset, frequency=0.5, amplitude=0.5))


# Color palettes
class Palette:
    """Color palettes for different states."""
    
    # Thinking mode - vibrant blues and cyans
    THINKING = [
        (52, 229, 238),   # Cyan
        (52, 204, 238),   # Light blue
        (52, 178, 238),   # Sky blue
        (52, 153, 238),   # Cornflower
        (52, 127, 238),   # Deeper blue
    ]
    
    # Idle mode - muted blues
    IDLE = [
        (72, 189, 216),   # Muted cyan
        (62, 179, 206),   # Soft blue
        (52, 169, 196),   # Calm blue
    ]
    
    # Pulse colors
    PULSE_THINKING = [
        (100, 230, 255),
        (80, 220, 250),
        (60, 210, 245),
    ]
    
    PULSE_IDLE = [
        (82, 196, 219),
        (72, 186, 209),
        (62, 176, 199),
    ]
