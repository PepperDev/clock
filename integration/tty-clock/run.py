#!/usr/bin/env python3
"""Integration test: clock output validation across TTY/non-TTY and once/loop modes.

Compares EXPECTED vs ACTUAL values for clock size, position (X,Y), and sidebar X.
"""

import os
import pty
import subprocess
import sys
import time
import select
import struct
import fcntl
import termios
import signal
import re

CLOCK_BIN = os.environ.get('CLOCK_BIN', '../../bin/clock')
_rc = 0



CLOCK_FACE_COLS = 27  # ascii clock: 6 digits × 3 + 2 colons × 1 + 7 spaces
DOT_ROWS = 5

# Font bitmap: digit_data[digit][col] bits map to pixel-rows 4..0 (bit 0 → pixel-row 4)
DIGIT_DATA = [
    [0x1F, 0x11, 0x1F], [0x00, 0x00, 0x1F], [0x17, 0x15, 0x1D],
    [0x15, 0x15, 0x1F], [0x1C, 0x04, 0x1F], [0x1D, 0x15, 0x17],
    [0x1F, 0x15, 0x17], [0x10, 0x10, 0x1F], [0x1F, 0x15, 0x1F],
    [0x1C, 0x14, 0x1F],
]

def fill_digit(grid, digit, pos):
    """Fill grid[pos+col][4-row] for the given digit (0-9) or colon (-1)."""
    if 0 <= digit <= 9:
        for col in range(3):
            bits = DIGIT_DATA[digit][col]
            for row in range(5):
                grid[pos + col][4 - row] = (bits >> row) & 1
    else:  # colon
        grid[pos][1] = 1  # pixel-row 3
        grid[pos][3] = 1  # pixel-row 1

def build_grid(h, m, s):
    """Return 27×5 grid (col×pixel_row) for the given time."""
    g = [[0] * 5 for _ in range(27)]
    fill_digit(g, h // 10, 0)
    fill_digit(g, h % 10, 4)
    fill_digit(g, -1, 8)
    fill_digit(g, m // 10, 10)
    fill_digit(g, m % 10, 14)
    fill_digit(g, -1, 18)
    fill_digit(g, s // 10, 20)
    fill_digit(g, s % 10, 24)
    return g

def glyph(u, l):
    """0=space, 1=▀, 2=█, 3=▄ (matches draw_col ordering)."""
    if l and u: return 2
    if u: return 1
    if l: return 3
    return 0

def face_span(h, m, s, sz):
    """Return clock face grid width at given scale (always CLOCK_FACE_COLS × sz)."""
    return CLOCK_FACE_COLS * sz

def say(msg):
    print(msg, flush=True)

def fail(msg):
    global _rc
    print(f"  FAIL: {msg}")
    _rc = 1

def esc_free(data):
    return b'\033' not in data

def has_blocks(data):
    return any(ch in data for ch in (b'\xe2\x96\x80', b'\xe2\x96\x84', b'\xe2\x96\x88'))

def has_reset(data):
    return b'\033c' in data

def has_cursor_hide(data):
    return b'\033[?25l' in data

def has_cursor_show(data):
    return b'\033[?25h' in data

def cursor_fwd_offsets(data):
    return sorted(set(int(m.group(1)) for m in re.finditer(rb'\033\[(\d+)C', data)))

def cursor_col_abs(data):
    return [int(m.group(1)) for m in re.finditer(rb'\033\[(\d+)G', data)]

def cursor_pos(data):
    """Return (row, col) from first \033[<row>;<col>H sequence, or None."""
    m = re.search(rb'\033\[(\d+);(\d+)H', data)
    return (int(m.group(1)), int(m.group(2))) if m else None

def strip_escapes(data):
    out = []
    i = 0
    while i < len(data):
        if data[i] == 0x1b:
            i += 1
            if i < len(data) and data[i] == ord('['):
                i += 1
                while i < len(data) and data[i] != 0x1b and data[i] not in (ord('C'), ord('H'), ord('l'), ord('h'), ord('m'), ord('J'), ord('G'), ord('K')):
                    i += 1
                if i < len(data) and data[i] != 0x1b:
                    i += 1  # skip CSI terminator
                # else: leave i at embedded \033 for outer loop
            elif i < len(data) and data[i] == ord('c'):
                i += 1
            elif i < len(data) and data[i] == ord('?'):
                i += 1
                while i < len(data) and data[i] != 0x1b and data[i] not in (ord('h'), ord('l')):
                    i += 1
                if i < len(data) and data[i] != 0x1b:
                    i += 1
            else:
                i += 1
        else:
            out.append(data[i])
            i += 1
    return bytes(out)

def rendered_width(data):
    """Clock face width from first line with ▀ or █ (top-half or full blocks).
    
    The colon produces ▄ on the second line but ▀/█ only appear on the first
    text line.  If the leading digit is "1" the first non-space falls at grid
    column 2 (rightmost third of the digit cell); extend left to column 0 to
    account for the two blank columns of the "1" cell.
    """
    for raw_line in data.split(b'\n'):
        if not raw_line:
            continue
        raw_line = raw_line.rstrip(b'\r')
        # Skip sidebar lines (TTY mode) — contain \033[<N>G positioning
        if re.search(rb'\033\[\d+G', raw_line):
            continue
        # Must have ▀ (\xe2\x96\x80) or █ (\xe2\x96\x88), not just ▄
        if b'\xe2\x96\x80' not in raw_line and b'\xe2\x96\x88' not in raw_line:
            continue
        cleaned = strip_escapes(raw_line)
        first = -1
        last = -1
        col = 0
        i = 0
        while i < len(cleaned):
            is_utf8 = bool(cleaned[i] & 0x80)
            if is_utf8:
                i += 1
                while i < len(cleaned) and (cleaned[i] & 0xc0) == 0x80:
                    i += 1
                if first < 0: first = col
                last = col
            elif cleaned[i] != 0x20:
                if first < 0: first = col
                last = col
                i += 1
            else:
                i += 1
            col += 1
        if last >= 0:
            if first > 0:
                first = 0  # leading digit is "1" — full cell starts at col 0
            return last - first + 1
    return 0

# --- Layout computation ---

def calc_info_col(wscol):
    info_w = wscol * 30 // 100
    if info_w < 40:
        info_w = 40
    if info_w > 60:
        info_w = 60
    left_w = wscol - info_w
    return left_w if left_w >= 0 else 0

def expected(wscol, wsrow, widgets, h=None, m=None, s=None):
    """Return (clock_x, clock_y, sidebar_x) for given terminal size and time."""
    if h is None:
        now = time.localtime()
        h, m, s = now.tm_hour, now.tm_min, now.tm_sec
    if widgets:
        avail = calc_info_col(wscol)
    else:
        avail = wscol
    sz = avail // CLOCK_FACE_COLS
    lines = wsrow // 16
    if lines < sz:
        sz = lines
    if sz < 1:
        sz = 1
    clock_width = face_span(h, m, s, sz)
    clock_x = (avail - clock_width) // 2
    if clock_x < 0:
        clock_x = 0
    clock_y = 0  # once mode default; loop mode caller overrides
    if widgets:
        sidebar_x = avail + 1
    else:
        sidebar_x = None
    return clock_x, clock_y, sidebar_x

def expected_loop_y(wscol, wsrow, widgets):
    """Loop mode Y position (0-indexed) from recalc_size formula."""
    if widgets:
        left_w = calc_info_col(wscol)
    else:
        left_w = wscol
    sz = left_w // CLOCK_FACE_COLS
    lines = wsrow // 16
    if lines < sz:
        sz = lines
    if sz < 1:
        sz = 1
    return max(0, (wsrow - (sz * DOT_ROWS + 1) // 2) // 2)

def expected_clock_height(wscol, wsrow, widgets):
    """Clock face height in text rows."""
    if widgets:
        avail = calc_info_col(wscol)
    else:
        avail = wscol
    sz = avail // CLOCK_FACE_COLS
    lines = wsrow // 16
    if lines < sz:
        sz = lines
    if sz < 1:
        sz = 1
    return (sz * DOT_ROWS + 1) // 2

def expected_clock_width(wscol, wsrow, widgets, h=None, m=None, s=None):
    """Expected rendered clock width = face_span(h,m,s,sz)."""
    if h is None:
        now = time.localtime()
        h, m, s = now.tm_hour, now.tm_min, now.tm_sec
    if widgets:
        info_w = max(40, min(60, wscol * 30 // 100))
        left_w = wscol - info_w
        if left_w < 0:
            left_w = 0
    else:
        left_w = wscol
    sz = left_w // CLOCK_FACE_COLS
    lines = wsrow // 16
    if lines < sz:
        sz = lines
    if sz < 1:
        sz = 1
    return face_span(h, m, s, sz)

# --- Runners ---

def run_no_pty(args, timeout=5):
    try:
        r = subprocess.run([CLOCK_BIN] + args,
                           capture_output=True, timeout=timeout)
        return r.stdout
    except subprocess.TimeoutExpired as e:
        return e.stdout

def echo_is_on(slave_path):
    """Return True if ECHO flag is set on the slave terminal."""
    try:
        sfd = os.open(slave_path, os.O_RDWR | os.O_NOCTTY)
        attrs = termios.tcgetattr(sfd)
        on = bool(attrs[3] & termios.ECHO)
        os.close(sfd)
        return on
    except Exception:
        return True  # assume on if can't check

def run_pty(args, rows=0, cols=0, timeout=5, start_line=10):
    master_fd, slave_fd = pty.openpty()
    slave_path = os.ttyname(slave_fd)
    if rows and cols:
        buf = struct.pack('HHHH', rows, cols, 0, 0)
        fcntl.ioctl(slave_fd, termios.TIOCSWINSZ, buf)

    # Position cursor at start_line so once-mode clock (no RIS) starts there
    os.write(slave_fd, f"\033[{start_line};0H".encode())

    proc = subprocess.Popen(
        [CLOCK_BIN] + args,
        stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
        close_fds=True,
        preexec_fn=os.setsid,
    )
    os.close(slave_fd)

    output = b''
    echo_ever_off = not echo_is_on(slave_path)
    deadline = time.time() + timeout
    while time.time() < deadline:
        remaining = max(0, deadline - time.time())
        try:
            r, _, _ = select.select([master_fd], [], [], min(0.05, remaining))
        except:
            break
        if r:
            try:
                data = os.read(master_fd, 65536)
                if not data:
                    break
                output += data
            except:
                break
        if not echo_is_on(slave_path):
            echo_ever_off = True

    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except:
        proc.terminate()
    try:
        proc.wait(timeout=2)
    except:
        proc.kill()
        proc.wait()

    tty_ok = echo_is_on(slave_path)

    os.close(master_fd)
    return output, tty_ok, echo_ever_off


def check_clock_size(label, data, expected_cols):
    w = rendered_width(data)
    if w != expected_cols:
        fail(f"{label}: expected clock size {expected_cols} cols, got {w}")


def check_position(label, data, exp_x, exp_y=None, once=True):
    cfo = cursor_fwd_offsets(data)
    cp = cursor_pos(data)
    if cfo:
        if cfo[0] != exp_x:
            fail(f"{label}: expected clock X offset {exp_x}, got {cfo[0]}")
    if cp:
        if not once:
            if exp_y is not None and cp[0] != exp_y + 1:
                fail(f"{label}: expected clock Y row {exp_y}, got {cp[0] - 1}")
        # once mode Y checks handled in run_test (clock omission + sidebar Y)
    elif not once and exp_y is not None:
        fail(f"{label}: expected clock Y positioning but none found")


def check_sidebar(label, data, exp_sidebar_x):
    cols = cursor_col_abs(data)
    if not cols:
        fail(f"{label}: no sidebar \\033[<N>G sequences found")
    elif cols[0] != exp_sidebar_x:
        fail(f"{label}: expected sidebar X {exp_sidebar_x}, got {cols[0]}")


def check_sidebar_y(label, data, exp_y_start, clock_h=None):
    """Verify sidebar is at row exp_y_start (0-indexed).

    Accepts either fixed CUP (\033[<row>;0H before first \033[<N>G) or relative
    cursor-up (\033[<N>A) by clock_h rows that lands at the same fixed position.
    """
    gs = [(m.start(), int(m.group(1))) for m in re.finditer(rb'\033\[(\d+)G', data)]
    if not gs:
        fail(f"{label}: no sidebar \\033[<N>G sequences")
        return
    first_g_pos = gs[0][0]

    # Fixed CUP positioning
    hs = [(m.start(), int(m.group(1))) for m in re.finditer(rb'\033\[(\d+);0H', data)]
    last_h = None
    for pos, row in hs:
        if pos < first_g_pos:
            last_h = (pos, row)
    if last_h is not None:
        row = last_h[1] - 1
        if row != exp_y_start:
            fail(f"{label}: expected first sidebar Y {exp_y_start}, got {row}")
        return

    # Relative cursor-up positioning
    if clock_h is not None:
        us = [(m.start(), int(m.group(1))) for m in re.finditer(rb'\033\[(\d+)A', data)]
        for pos, n in us:
            if pos < first_g_pos:
                if n == clock_h:
                    return
                fail(f"{label}: expected cursor-up {clock_h}, got {n}")
                return
    fail(f"{label}: no \\033[<row>;0H before first sidebar line")


def run_test(label, once, pty, rows, cols, widgets, timeout=5, start_line=10, mode='ascii'):
    wscol = cols if cols else 80
    wsrow = rows if rows else 24
    args = (['-o'] if once else []) + (['-w', '', mode] if not widgets else [mode])
    prefix = ' -o' if once else ''
    wflag = " -w ''" if not widgets else ''
    say(f"  {label}: clock{prefix}{wflag} {mode}" + (f", pty {rows}x{cols}" if pty else ", no pty"))

    if pty:
        data, echo, echo_ever_off = run_pty(args, rows, cols, timeout=timeout, start_line=start_line)
        pty_header = b'\033[%d;0H' % start_line
        if data.startswith(pty_header):
            data = data[len(pty_header):]
        if mode == 'text':
            if not esc_free(data):
                fail(f"{label}: text mode escape seq in pty")
            if echo_ever_off:
                fail(f"{label}: text mode echo removed during execution")
            if not data.strip():
                fail(f"{label}: no output")
            return
        if once:
            if echo_ever_off:
                fail(f"{label}: echo removed during once mode")
            if not echo:
                fail(f"{label}: ECHO removed")
            if has_reset(data):
                fail(f"{label}: contains \\033c")
            if has_cursor_hide(data):
                fail(f"{label}: contains \\033[?25l")
        else:
            if not echo:
                fail(f"{label}: ECHO removed")
            if not echo_ever_off:
                fail(f"{label}: echo should have been off during loop mode")
            if not has_reset(data):
                fail(f"{label}: missing \\033c")
            if not has_cursor_hide(data):
                fail(f"{label}: missing \\033[?25l")
        if not has_blocks(data):
            fail(f"{label}: no half-block chars")
        if once:
            check_position(label, data, expected(wscol, wsrow, widgets)[0], once=True)
            if widgets:
                hs = list(re.finditer(rb'\033\[(\d+);0H', data))
                if len(hs) > 1:
                    fail(f"{label}: once mode should have at most one \\033[<row>;0H (sidebar), got {len(hs)}")
                clock_h = expected_clock_height(wscol, wsrow, widgets) if pty else None
                if start_line + clock_h <= wsrow:
                    once_exp_y = start_line - 1
                else:
                    once_exp_y = wsrow - clock_h - 1
            else:
                if cursor_pos(data) is not None:
                    fail(f"{label}: once mode clock emitted \\033[<row>;0H when it should not")
        else:
            ex, _, _ = expected(wscol, wsrow, widgets)
            check_position(label, data, ex, exp_y=expected_loop_y(wscol, wsrow, widgets), once=False)
        check_clock_size(label, data, expected_clock_width(wscol, wsrow, widgets))
        if widgets:
            check_sidebar(label, data, expected(wscol, wsrow, widgets)[2])
            if once:
                check_sidebar_y(label, data, once_exp_y, clock_h)
            else:
                check_sidebar_y(label, data, 0)
            if not b'\xf0\x9f\x92\xbb' in data and not b'\xf0\x9f\x97\x84' in data and not b'\xf0\x9f\x9b\x9c' in data:
                fail(f"{label}: no sidebar content")
    else:
        data = run_no_pty(args, timeout=timeout)
        if mode == 'text':
            if not esc_free(data):
                fail(f"{label}: text mode escape seq without pty")
            if not data.strip():
                fail(f"{label}: no output")
            return
        if once:
            if not esc_free(data):
                fail(f"{label}: escape seq without pty")
            if not has_blocks(data):
                fail(f"{label}: no half-block chars")
            check_clock_size(label, data, expected_clock_width(wscol, wsrow, widgets))
        else:
            if not esc_free(data):
                fail(f"{label}: escape seq without pty")
            if not data:
                fail(f"{label}: no output")


def main():
    global _rc
    say("tty-clock integration tests")

    # ====== GROUP 1: once, no-pty ======
    say("\n[Group 1: --once, no pty]")
    run_test("1a", once=True, pty=False, rows=0, cols=0, widgets=False)
    run_test("1b", once=True, pty=False, rows=0, cols=0, widgets=True)

    # ====== GROUP 2: once, pty 0x0 ======
    say("\n[Group 2: --once, pty size 0]")
    run_test("2a", once=True, pty=True, rows=0, cols=0, widgets=False)
    run_test("2b", once=True, pty=True, rows=0, cols=0, widgets=True)

    # ====== GROUP 3: once, pty 24x80, cursor at line 10 ======
    say("\n[Group 3: --once, pty 24x80, cursor at line 10]")
    run_test("3a", once=True, pty=True, rows=24, cols=80, widgets=False)
    run_test("3b", once=True, pty=True, rows=24, cols=80, widgets=True)

    # ====== GROUP 4: once, pty 24x80, cursor at line 23 ======
    say("\n[Group 4: --once, pty 24x80, cursor at line 23]")
    run_test("4a", once=True, pty=True, rows=24, cols=80, widgets=False, start_line=23)
    run_test("4b", once=True, pty=True, rows=24, cols=80, widgets=True, start_line=23)

    # ====== GROUP 5: once, pty 36x120 ======
    say("\n[Group 5: --once, pty 36x120]")
    run_test("5a", once=True, pty=True, rows=36, cols=120, widgets=False)
    run_test("5b", once=True, pty=True, rows=36, cols=120, widgets=True)

    # ====== GROUP 6: --once, pty 50x120 ======
    say("\n[Group 6: --once, pty 50x120]")
    run_test("6a", once=True, pty=True, rows=50, cols=120, widgets=False)
    run_test("6b", once=True, pty=True, rows=50, cols=120, widgets=True)

    # ====== GROUP 7: loop, no-pty ======
    say("\n[Group 7: loop, no pty, 3s]")
    run_test("7a", once=False, pty=False, rows=0, cols=0, widgets=False, timeout=3)
    run_test("7b", once=False, pty=False, rows=0, cols=0, widgets=True, timeout=3)

    # ====== GROUP 8: loop, pty 0x0 ======
    say("\n[Group 8: loop, pty size 0, 3s]")
    run_test("8a", once=False, pty=True, rows=0, cols=0, widgets=False, timeout=3)
    run_test("8b", once=False, pty=True, rows=0, cols=0, widgets=True, timeout=3)

    # ====== GROUP 9: loop, pty 24x80, cursor at line 10 ======
    say("\n[Group 9: loop, pty 24x80, cursor at line 10, 3s]")
    run_test("9a", once=False, pty=True, rows=24, cols=80, widgets=False, timeout=3)
    run_test("9b", once=False, pty=True, rows=24, cols=80, widgets=True, timeout=3)

    # ====== GROUP 10: loop, pty 24x80, cursor at line 23 ======
    say("\n[Group 10: loop, pty 24x80, cursor at line 23, 3s]")
    run_test("10a", once=False, pty=True, rows=24, cols=80, widgets=False, timeout=3, start_line=23)
    run_test("10b", once=False, pty=True, rows=24, cols=80, widgets=True, timeout=3, start_line=23)

    # ====== GROUP 11: loop, pty 36x120 ======
    say("\n[Group 11: loop, pty 36x120, 3s]")
    run_test("11a", once=False, pty=True, rows=36, cols=120, widgets=False, timeout=3)
    run_test("11b", once=False, pty=True, rows=36, cols=120, widgets=True, timeout=3)

    # ====== GROUP 12: loop, pty 50x120 ======
    say("\n[Group 12: loop, pty 50x120, 3s]")
    run_test("12a", once=False, pty=True, rows=50, cols=120, widgets=False, timeout=3)
    run_test("12b", once=False, pty=True, rows=50, cols=120, widgets=True, timeout=3)

    # ====== GROUP 13: text, once, no-pty ======
    say("\n[Group 13: text --once, no pty]")
    run_test("13a", once=True, pty=False, rows=0, cols=0, widgets=False, mode='text')
    run_test("13b", once=True, pty=False, rows=0, cols=0, widgets=True, mode='text')

    # ====== GROUP 14: text, once, pty 0x0 ======
    say("\n[Group 14: text --once, pty size 0]")
    run_test("14a", once=True, pty=True, rows=0, cols=0, widgets=False, mode='text')
    run_test("14b", once=True, pty=True, rows=0, cols=0, widgets=True, mode='text')

    # ====== GROUP 15: text, once, pty 36x120 ======
    say("\n[Group 15: text --once, pty 36x120]")
    run_test("15a", once=True, pty=True, rows=36, cols=120, widgets=False, mode='text')
    run_test("15b", once=True, pty=True, rows=36, cols=120, widgets=True, mode='text')

    # ====== GROUP 16: text, loop, no-pty ======
    say("\n[Group 16: text loop, no pty, 3s]")
    run_test("16a", once=False, pty=False, rows=0, cols=0, widgets=False, timeout=3, mode='text')
    run_test("16b", once=False, pty=False, rows=0, cols=0, widgets=True, timeout=3, mode='text')

    # ====== GROUP 17: text, loop, pty 0x0 ======
    say("\n[Group 17: text loop, pty size 0, 3s]")
    run_test("17a", once=False, pty=True, rows=0, cols=0, widgets=False, timeout=3, mode='text')
    run_test("17b", once=False, pty=True, rows=0, cols=0, widgets=True, timeout=3, mode='text')

    # ====== GROUP 18: text, loop, pty 36x120 ======
    say("\n[Group 18: text loop, pty 36x120, 3s]")
    run_test("18a", once=False, pty=True, rows=36, cols=120, widgets=False, timeout=3, mode='text')
    run_test("18b", once=False, pty=True, rows=36, cols=120, widgets=True, timeout=3, mode='text')

    if _rc == 0:
        say("\nAll tty-clock tests PASS")
    else:
        say(f"\n{_rc} test(s) FAILED")
    return _rc


if __name__ == '__main__':
    try:
        rc = main()
    finally:
        import os as _os
        _os.system('stty sane 2>/dev/null')
    sys.exit(rc)
