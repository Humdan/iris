#!/usr/bin/env python3
"""
fbsnap.py — dump /dev/fb0 (RGB565 800x480) to PPM/PNG

Usage: fbsnap.py [out.png]
"""

import struct
import subprocess
import sys
from pathlib import Path
import tempfile
import os

def rgb565_to_rgb888(px):
    """Convert 16-bit RGB565 to (r, g, b)"""
    r = ((px >> 11) & 0x1f) << 3
    g = ((px >> 5) & 0x3f) << 2
    b = (px & 0x1f) << 3
    return (r, g, b)

def main():
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('/tmp/iris_snap.png')
    
    # Read framebuffer
    try:
        with open('/dev/fb0', 'rb') as fb:
            data = fb.read(800 * 480 * 2)
    except Exception as e:
        print(f"error reading /dev/fb0: {e}", file=sys.stderr)
        sys.exit(1)
    
    # Unpack RGB565 pixels
    pixels = struct.unpack(f'<{800*480}H', data)
    
    # Write PPM
    with tempfile.NamedTemporaryFile(suffix='.ppm', delete=False) as tmp:
        tmp.write(b'P6\n800 480\n255\n')
        for px in pixels:
            r, g, b = rgb565_to_rgb888(px)
            tmp.write(bytes([r, g, b]))
        tmp_path = tmp.name
    
    try:
        # Convert to PNG
        subprocess.run(['convert', tmp_path, str(out_path)], check=True, 
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print(f"saved {out_path}", file=sys.stderr)
    except FileNotFoundError:
        print("error: 'convert' (ImageMagick) not found. saving as PPM instead.", file=sys.stderr)
        import shutil
        shutil.move(tmp_path, str(out_path.with_suffix('.ppm')))
        print(f"saved {out_path.with_suffix('.ppm')}", file=sys.stderr)
        return
    finally:
        try:
            os.unlink(tmp_path)
        except:
            pass

if __name__ == '__main__':
    main()
