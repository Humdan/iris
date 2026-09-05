"""Framebuffer renderer for direct LCD output without X11.

Each text cell of the animation becomes a colored block on the LCD.
Frames are composed in memory and blitted with a single write().
"""

import struct

from utils.patterns import Palette


def _sysfs_int(path, default):
    try:
        return int(open(path).read().strip().split(',')[0])
    except Exception:
        return default


def rgb_to_565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


class FramebufferRenderer:
    """Direct framebuffer renderer for the Pi LCD screen."""

    def __init__(self, fb_device='/dev/fb0', char_width=8, char_height=16):
        self.fb_device = fb_device
        self.fb = None
        self.char_width = char_width
        self.char_height = char_height
        self.width = self.height = self.bpp = 0
        self.cols = self.rows = 0
        self._pixel_cache = {}
        self._init_palette()

    def _init_palette(self):
        th, idl = Palette.THINKING, Palette.IDLE
        self.palette = {
            ' ': (0, 0, 0),
            '░': (20, 60, 80),
            '▒': (30, 100, 130),
            '▓': (40, 140, 170),
            '█': th[0],
            '·': (35, 120, 150),
            '∘': (60, 160, 190),
            '◦': (70, 170, 200),
            '•': th[1],
            '○': idl[0],
            '◑': th[3],
            '◐': th[2],
            '●': (200, 255, 255),
            '─': (30, 90, 110),
            '═': (40, 110, 130),
            '╌': (25, 75, 95),
        }
        self.default_color = th[2]

    def open(self):
        try:
            self.fb = open(self.fb_device, 'r+b', buffering=0)
            size = open('/sys/class/graphics/fb0/virtual_size').read().split(',')
            self.width, self.height = int(size[0]), int(size[1])
            self.bpp = _sysfs_int('/sys/class/graphics/fb0/bits_per_pixel', 16)
            self.cols = self.width // self.char_width
            self.rows = self.height // self.char_height
            self.line_length = _sysfs_int('/sys/class/graphics/fb0/stride', self.width * self.bpp // 8)
            return True
        except Exception as e:
            print(f"Failed to open framebuffer: {e}")
            return False

    def close(self):
        if self.fb:
            try:
                self.clear()
            except Exception:
                pass
            self.fb.close()
            self.fb = None

    def _pixel_bytes(self, rgb):
        pb = self._pixel_cache.get(rgb)
        if pb is None:
            r, g, b = rgb
            if self.bpp == 16:
                pb = struct.pack('<H', rgb_to_565(r, g, b))
            else:
                pb = struct.pack('<I', (0xFF << 24) | (r << 16) | (g << 8) | b)
            self._pixel_cache[rgb] = pb
        return pb

    def clear(self):
        if self.fb:
            self.fb.seek(0)
            self.fb.write(b'\x00' * (self.line_length * self.height))

    def render_text_frame(self, frame_text):
        """Compose the whole frame in memory, then blit once."""
        if not self.fb:
            return
        bpp_bytes = self.bpp // 8
        black_px = self._pixel_bytes((0, 0, 0))
        pad = self.line_length - self.width * bpp_bytes
        out = bytearray()
        lines = frame_text.split('\n')

        for row in range(self.rows):
            line = lines[row] if row < len(lines) else ''
            cells = []
            for col in range(self.cols):
                ch = line[col] if col < len(line) else ' '
                rgb = self.palette.get(ch, self.default_color)
                cells.append(self._pixel_bytes(rgb) * self.char_width)
            scan = b''.join(cells)
            # pad to full width in case cols*char_width < width
            scan += black_px * (self.width - self.cols * self.char_width)
            scan += b'\x00' * pad
            out += scan * self.char_height

        # remaining rows below text area
        remaining = self.height - self.rows * self.char_height
        if remaining > 0:
            out += b'\x00' * (self.line_length * remaining)

        self.fb.seek(0)
        self.fb.write(bytes(out))

    def get_terminal_size(self):
        return (self.cols, self.rows)
