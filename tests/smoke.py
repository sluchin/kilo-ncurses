#!/usr/bin/env python3
"""Drive kilo-ncurses through a pseudo terminal and check what it writes."""

import fcntl
import os
import pty
import select
import signal
import struct
import tempfile
import termios
import time
import unittest

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "kilo-ncurses")

CTRL_Q, CTRL_S, CTRL_F = b"\x11", b"\x13", b"\x06"
# keypad mode: xterm sends application-mode cursor keys (ESC O x)
UP, DOWN, RIGHT, LEFT = b"\x1bOA", b"\x1bOB", b"\x1bOC", b"\x1bOD"
HOME, END, DELETE = b"\x1bOH", b"\x1bOF", b"\x1b[3~"
ENTER, BACKSPACE = b"\r", b"\x7f"


class Session:
    def __init__(self, *args, rows=24, cols=80):
        env = dict(os.environ, TERM="xterm-256color", LC_ALL="C.UTF-8")
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execve(BIN, [BIN, *args], env)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.out = b""
        self.drain()

    def drain(self, quiet=0.25, limit=3.0):
        end = time.time() + limit
        last = time.time()
        while time.time() < end and time.time() - last < quiet:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try:
                    data = os.read(self.fd, 65536)
                except OSError:
                    return
                if not data:
                    return
                self.out += data
                last = time.time()

    def send(self, *chunks):
        for c in chunks:
            os.write(self.fd, c if isinstance(c, bytes) else c.encode())
            self.drain(quiet=0.08)

    def finish(self, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                return os.waitstatus_to_exitcode(status)
            self.drain(quiet=0.05, limit=0.1)
        os.kill(self.pid, 9)
        os.waitpid(self.pid, 0)
        raise AssertionError("editor did not exit")


def tmpfile(content=None, suffix=".txt"):
    fd, path = tempfile.mkstemp(suffix=suffix)
    os.close(fd)
    if content is None:
        os.unlink(path)
    else:
        with open(path, "wb") as f:
            f.write(content)
    return path


def read(path):
    with open(path, "rb") as f:
        return f.read()


class SmokeTest(unittest.TestCase):
    def test_new_file_save_as(self):
        path = tmpfile()
        s = Session()
        s.send("hello", ENTER, "world", CTRL_S)
        s.send(path.encode(), ENTER)
        s.send(CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"hello\nworld\n")
        os.unlink(path)

    def test_edit_existing_file(self):
        path = tmpfile(b"abc\ndef\n")
        s = Session(path)
        s.send(DOWN, HOME, BACKSPACE)  # join the two lines
        s.send(END, "!", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"abcdef!\n")
        os.unlink(path)

    def test_delete_and_split(self):
        path = tmpfile(b"abcd\n")
        s = Session(path)
        s.send(RIGHT, RIGHT, ENTER)  # ab / cd
        s.send(DELETE, CTRL_S, CTRL_Q)  # delete "c"
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"ab\nd\n")
        os.unlink(path)

    def test_utf8_cursor_and_delete(self):
        path = tmpfile("日本語\n".encode())
        s = Session(path)
        s.send(RIGHT, BACKSPACE)  # removes the first whole character
        s.send(CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), "本語\n".encode())
        os.unlink(path)

    def test_utf8_input(self):
        path = tmpfile(b"\n")
        s = Session(path)
        s.send("こんにちは", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), "こんにちは\n".encode())
        os.unlink(path)

    def test_incremental_search(self):
        path = tmpfile(b"one\ntwo foo\nthree foo\n")
        s = Session(path)
        s.send(CTRL_F, "foo", ENTER)  # first match: line 2
        s.send("X", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"one\ntwo Xfoo\nthree foo\n")
        os.unlink(path)

    def test_search_next_with_arrow_and_cancel(self):
        path = tmpfile(b"foo\nbar foo\n")
        s = Session(path)
        s.send(CTRL_F, "foo", DOWN, ENTER)  # arrow moves to the next match
        s.send("X", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"foo\nbar Xfoo\n")
        s2 = Session(path)
        s2.send(CTRL_F, "bar", "\x1b")  # Esc restores the cursor
        s2.send("Y", CTRL_S, CTRL_Q)
        self.assertEqual(s2.finish(), 0)
        self.assertEqual(read(path), b"Yfoo\nbar Xfoo\n")
        os.unlink(path)

    def test_quit_requires_confirmation_when_modified(self):
        path = tmpfile(b"x\n")
        s = Session(path)
        s.send("a", CTRL_Q)
        s.send(CTRL_Q)
        self.assertIn(b"unsaved changes", s.out)
        s.send(CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"x\n")  # nothing was saved
        os.unlink(path)

    def test_vertical_move_keeps_column(self):
        path = tmpfile(b"abcdef\nab\nabcdef\n")
        s = Session(path)
        s.send(END, DOWN, DOWN, "!", CTRL_S, CTRL_Q)  # column 6 survives the short line
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"abcdef\nab\nabcdef!\n")
        os.unlink(path)

    def test_syntax_highlighting_emits_color(self):
        path = tmpfile(b'int main() { return 0; } // note\n', suffix=".c")
        s = Session(path)
        self.assertRegex(s.out.decode("utf-8", "replace"), r"\x1b\[[0-9;]*3[0-7]m")
        s.send(CTRL_Q)
        self.assertEqual(s.finish(), 0)
        os.unlink(path)

    def test_block_comment_spans_lines(self):
        path = tmpfile(b"/* a\nb */ int x;\n", suffix=".c")
        s = Session(path)
        # the comment colour (cyan) must stay on until the closing marker on line 2
        self.assertRegex(s.out, rb"\x1b\[36m/\* a(?:(?!\x1b\[39m).)*b \*/\x1b\[39m")
        s.send(CTRL_Q)
        self.assertEqual(s.finish(), 0)
        os.unlink(path)

    def test_tiny_terminal_does_not_crash(self):
        path = tmpfile(b"a long line that does not fit the screen\n" * 5)
        s = Session(path, rows=3, cols=10)
        s.send(DOWN, END, "z", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertTrue(read(path).startswith(b"a long line"))
        os.unlink(path)

    def test_resize_while_running(self):
        path = tmpfile(b"x\n")
        s = Session(path)
        fcntl.ioctl(s.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 10, 30, 0, 0))
        os.kill(s.pid, signal.SIGWINCH)
        s.drain()
        s.send("y", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"yx\n")
        os.unlink(path)

    def test_save_preserves_mode(self):
        path = tmpfile(b"x\n")
        os.chmod(path, 0o600)
        s = Session(path)
        s.send("y", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)
        os.unlink(path)


if __name__ == "__main__":
    if not os.path.exists(BIN):
        raise SystemExit("build first: make")
    unittest.main(verbosity=2)
