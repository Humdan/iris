# Terminal control and ANSI utilities
import os
import sys
import termios
import tty
import signal
from contextlib import contextmanager

# ANSI escape codes
class ANSI:
    RESET = "\033[0m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    ITALIC = "\033[3m"
    UNDERLINE = "\033[4m"
    BLINK = "\033[5m"
    
    # Colors
    BLACK = "\033[30m"
    RED = "\033[31m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    BLUE = "\033[34m"
    MAGENTA = "\033[35m"
    CYAN = "\033[36m"
    WHITE = "\033[37m"
    
    # Bright colors
    BRIGHT_BLACK = "\033[90m"
    BRIGHT_RED = "\033[91m"
    BRIGHT_GREEN = "\033[92m"
    BRIGHT_YELLOW = "\033[93m"
    BRIGHT_BLUE = "\033[94m"
    BRIGHT_MAGENTA = "\033[95m"
    BRIGHT_CYAN = "\033[96m"
    BRIGHT_WHITE = "\033[97m"
    
    # Background colors
    BG_BLACK = "\033[40m"
    BG_RED = "\033[41m"
    BG_GREEN = "\033[42m"
    BG_YELLOW = "\033[43m"
    BG_BLUE = "\033[44m"
    BG_MAGENTA = "\033[45m"
    BG_CYAN = "\033[46m"
    BG_WHITE = "\033[47m"
    
    # Bright backgrounds
    BG_BRIGHT_BLACK = "\033[100m"
    BG_BRIGHT_RED = "\033[101m"
    BG_BRIGHT_GREEN = "\033[102m"
    BG_BRIGHT_YELLOW = "\033[103m"
    BG_BRIGHT_BLUE = "\033[104m"
    BG_BRIGHT_MAGENTA = "\033[105m"
    BG_BRIGHT_CYAN = "\033[106m"
    BG_BRIGHT_WHITE = "\0e[107m"


def get_terminal_size():
    """Get terminal dimensions."""
    try:
        size = os.get_terminal_size()
        return size.columns, size.lines
    except (OSError, AttributeError):
        # Fallback for non-TTY environments
        return 80, 24


def clear_screen():
    """Clear the terminal screen."""
    sys.stdout.write("\033[2J\033[H")
    sys.stdout.flush()


def hide_cursor():
    """Hide the terminal cursor."""
    sys.stdout.write("\033[?25l")
    sys.stdout.flush()


def show_cursor():
    """Show the terminal cursor."""
    sys.stdout.write("\033[?25h")
    sys.stdout.flush()


def move_cursor(row, col):
    """Move cursor to specific position (1-indexed)."""
    sys.stdout.write(f"\033[{row};{col}H")
    sys.stdout.flush()


def save_cursor():
    """Save cursor position."""
    sys.stdout.write("\033[s")
    sys.stdout.flush()


def restore_cursor():
    """Restore cursor position."""
    sys.stdout.write("\033[u")
    sys.stdout.flush()


def set_fg_color(r, g, b):
    """Set foreground color using RGB (24-bit)."""
    sys.stdout.write(f"\033[38;2;{r};{g};{b}m")
    sys.stdout.flush()


def set_bg_color(r, g, b):
    """Set background color using RGB (24-bit)."""
    sys.stdout.write(f"\033[48;2;{r};{g};{b}m")
    sys.stdout.flush()


def reset_colors():
    """Reset all color settings."""
    sys.stdout.write(ANSI.RESET)
    sys.stdout.flush()


@contextmanager
def terminal_context():
    """Context manager for terminal settings."""
    old_settings = None
    try:
        old_settings = termios.tcgetattr(sys.stdin)
        tty.cbreak(sys.stdin)
        hide_cursor()
        signal.signal(signal.SIGWINCH, lambda s, f: None)  # Ignore resize interrupts
        yield
    except termios.error:
        # Handle non-terminal environments gracefully
        yield
    finally:
        if old_settings:
            try:
                termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old_settings)
            except termios.error:
                pass
        show_cursor()
        reset_colors()


def is_terminal():
    """Check if output is a terminal."""
    return sys.stdout.isatty()


def write_raw(text):
    """Write raw text to stdout without newline."""
    sys.stdout.write(text)
    sys.stdout.flush()


def clear_line():
    """Clear the current line."""
    sys.stdout.write("\033[2K")
    sys.stdout.flush()


def clear_to_end():
    """Clear from cursor to end of screen."""
    sys.stdout.write("\033[0J")
    sys.stdout.flush()
