#!/usr/bin/env python3
"""kilo-ncurses を疑似端末(pty)で実際に動かし, 出力とファイルの内容を確認する."""

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

# KILO_BIN で, 動かす実行ファイルを変えられる (make coverage は, 計測用のビルドを使う)
BIN = os.path.abspath(
    os.environ.get(
        "KILO_BIN", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "kilo-ncurses")
    )
)

CTRL_Q, CTRL_S, CTRL_F, CTRL_A, CTRL_E = b"\x11", b"\x13", b"\x06", b"\x01", b"\x05"
# keypad モードでは xterm はアプリケーションモードのカーソルキー(ESC O x)を送る
UP, DOWN, RIGHT, LEFT = b"\x1bOA", b"\x1bOB", b"\x1bOC", b"\x1bOD"
HOME, END, DELETE = b"\x1bOH", b"\x1bOF", b"\x1b[3~"
PGUP, PGDN, F1, KP_ENTER = b"\x1b[5~", b"\x1b[6~", b"\x1bOP", b"\x1bOM"
ENTER, BACKSPACE = b"\r", b"\x7f"


# make coverage が作る, 関数を失敗させるライブラリ(tests/fault.c). なければ, 失敗のテストは飛ばす
FAULT_LIB = os.environ.get("KILO_FAULT_LIB")


class Session:
    def __init__(
        self, *args, rows=24, cols=80, term="xterm-256color", lang="C.UTF-8", fault=None, cwd=None
    ):
        env = dict(os.environ, TERM=term, LC_ALL=lang)
        if fault:
            env.update(LD_PRELOAD=FAULT_LIB, KILO_FAULT=fault)
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            if cwd:
                os.chdir(cwd)
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
        s.send(DOWN, HOME, BACKSPACE)  # 2 行を連結する
        s.send(END, "!", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"abcdef!\n")
        os.unlink(path)

    def test_delete_and_split(self):
        path = tmpfile(b"abcd\n")
        s = Session(path)
        s.send(RIGHT, RIGHT, ENTER)  # ab / cd
        s.send(DELETE, CTRL_S, CTRL_Q)  # "c" を削除する
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"ab\nd\n")
        os.unlink(path)

    def test_utf8_cursor_and_delete(self):
        path = tmpfile("日本語\n".encode())
        s = Session(path)
        s.send(RIGHT, BACKSPACE)  # 先頭の 1 文字(全体)を削除する
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
        s.send(CTRL_F, "foo", ENTER)  # 最初の一致: 2 行目
        s.send("X", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"one\ntwo Xfoo\nthree foo\n")
        os.unlink(path)

    def test_search_next_with_arrow_and_cancel(self):
        path = tmpfile(b"foo\nbar foo\n")
        s = Session(path)
        s.send(CTRL_F, "foo", DOWN, ENTER)  # 矢印キーで次の一致へ移る
        s.send("X", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"foo\nbar Xfoo\n")
        s2 = Session(path)
        s2.send(CTRL_F, "bar", "\x1b")  # Esc でカーソルが元に戻る
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
        self.assertEqual(read(path), b"x\n")  # 何も保存されていない
        os.unlink(path)

    def test_vertical_move_keeps_column(self):
        path = tmpfile(b"abcdef\nab\nabcdef\n")
        s = Session(path)
        s.send(END, DOWN, DOWN, "!", CTRL_S, CTRL_Q)  # 短い行を通っても 6 桁目が保たれる
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
        # コメント色(シアン)は, 2 行目の閉じ記号まで続かなければならない
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


    # 以下は, カバレッジ(make coverage)のために, 残りの操作と失敗の道筋を通すテスト.

    def test_usage_with_too_many_arguments(self):
        s = Session("a", "b")
        self.assertEqual(s.finish(), 1)
        self.assertIn(b"Usage:", s.out)

    def test_welcome_screen_and_message_timeout(self):
        s = Session()
        self.assertIn(b"version", s.out)
        # メッセージは 5 秒で消える(500 ms ごとに再描画する)
        time.sleep(5.6)
        s.drain()
        s.send(CTRL_Q)
        self.assertEqual(s.finish(), 0)
        # 狭い端末では, ウェルカム表示を出さない
        s2 = Session(cols=20)
        self.assertNotIn(b"version", s2.out)
        s2.send(CTRL_Q)
        self.assertEqual(s2.finish(), 0)

    def test_empty_file_and_crlf(self):
        empty = tmpfile(b"")
        s = Session(empty)
        s.send("a", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(empty), b"a\n")
        os.unlink(empty)
        crlf = tmpfile(b"a\r\nb\r\n")
        s = Session(crlf)
        s.send(CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(crlf), b"a\nb\n")
        os.unlink(crlf)

    def test_many_lines_scroll_and_page_keys(self):
        path = tmpfile(b"".join(b"line %d\n" % i for i in range(100)))
        s = Session(path, rows=10)
        s.send(PGDN, PGDN, DOWN, PGUP, PGUP, PGUP, UP)
        s.send(PGDN * 20, "z", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertTrue(read(path).endswith(b"zline 99\n"))
        os.unlink(path)

    def test_horizontal_scroll_and_wide_chars_at_the_edges(self):
        path = tmpfile(("x" + "日本語" * 20 + "\n").encode())
        s = Session(path, cols=20)
        s.send(END, HOME, END, LEFT * 3)
        s.send(CTRL_A, CTRL_E, "!", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertTrue(read(path).endswith("語!\n".encode()))
        os.unlink(path)

    def test_control_and_invalid_bytes_are_drawn(self):
        # タブ, 制御文字, DEL, 不正な UTF-8, 途中で切れた UTF-8, 表示できない文字(U+0080),
        # 継続バイトだけの列
        path = tmpfile(b"\ta\x01b\x7fc\xffd\xc2\x80e\x80\x80\x80\x80\x80\n\xe3\x81\n")
        s = Session(path)
        self.assertIn(b"^A", s.out)
        s.send(END, BACKSPACE, BACKSPACE, DOWN, END, BACKSPACE, CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        # 継続バイトは, 前の文字と合わせて 4 バイトまでを 1 文字として消す
        self.assertEqual(read(path), b"\ta\x01b\x7fc\xffd\xc2\x80\n\n")
        os.unlink(path)

    def test_cursor_moves_across_lines(self):
        path = tmpfile(b"ab\ncd\n")
        s = Session(path)
        s.send(LEFT, RIGHT, RIGHT, RIGHT, LEFT, RIGHT * 5, "!")
        s.send(UP, UP, DOWN * 5, BACKSPACE * 10, DELETE * 10)
        s.send(CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"\n")
        os.unlink(path)

    def test_other_keys(self):
        path = tmpfile(b"ab\n")
        s = Session(path)
        # Ctrl-L(描き直し), F1(何もしない), Esc, Ctrl-C, Ctrl-B(何もしない), Ctrl-H(後退),
        # テンキーの Enter, タブ, Ctrl-J(改行)
        s.send(b"\x0c", F1, b"\x1b", b"\x03", b"\x02", END, b"\x08", KP_ENTER, b"\t", b"\n")
        s.send(CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"a\n\t\n\n")
        os.unlink(path)

    def test_quit_counts_down(self):
        s = Session()
        s.send("a", CTRL_Q, CTRL_Q)
        self.assertIn(b"2 more times", s.out)
        s.send(CTRL_Q)
        self.assertEqual(s.finish(), 0)

    def test_save_as_cancel_and_prompt_editing(self):
        path = tmpfile(suffix=".scm")
        s = Session()
        s.send("(x)", CTRL_S, "\x1b")  # 取り消す
        self.assertIn(b"cancelled", s.out)
        # 空の Enter は無視する. 矢印キーと後退と Delete で直す
        s.send(CTRL_S, ENTER, LEFT, "zz", BACKSPACE, DELETE, BACKSPACE)
        time.sleep(0.7)  # 入力を待つ間に, 時間切れ(ERR)で描き直す
        s.drain()
        s.send(path.encode(), ENTER, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"(x)\n")
        os.unlink(path)

    def test_prompt_limits_and_grows_the_input(self):
        s = Session(cols=40)
        # 入力は QUERY_MAX バイトまで(長いプロンプトでは, カーソルは右端に置く)
        s.send(CTRL_F, "q" * 300, ENTER)
        s.send(CTRL_Q)
        self.assertEqual(s.finish(), 0)

    def test_search_backward_wrap_and_not_found(self):
        path = tmpfile(b"foo foo\nbar\nfoo\n")
        s = Session(path)
        s.send(DOWN, CTRL_F, "foo", UP, UP, LEFT, RIGHT, RIGHT, F1)
        s.send(BACKSPACE, BACKSPACE, BACKSPACE, "zzz")  # 見つからない
        s.send(BACKSPACE * 3, "bar", ENTER)
        s.send("X", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"foo foo\nXbar\nfoo\n")
        os.unlink(path)

    def test_search_finds_the_match_before_the_cursor_on_the_same_line(self):
        path = tmpfile(b"foo bar\n")
        s = Session(path)
        s.send(END, CTRL_F, "foo", ENTER, "X", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"Xfoo bar\n")
        os.unlink(path)

    def test_highlight_c(self):
        src = (
            b"  #include <x.h>\n"
            b"/* a */ int b = 1.5 + .5 + x1; char c = '\\'';\n"
            b"const char *s = \"a\\\"b\"; foo(); /* open\n"
            b"still */ return 0;\n"
            b"\"unterminated \\\n"
            b"x.y\n"
        )
        path = tmpfile(src, suffix=".c")
        s = Session(path)
        # ブロックコメントを開いて閉じ, 下の行を塗り直す
        s.send(DOWN * 3, HOME, "/*", ENTER, "*/", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        os.unlink(path)

    def test_highlight_scheme(self):
        src = (
            b'(define (f x) #|block|# (let* ((c #\\a) (d #\\space) (u #\\\xce\xbb)) (car "s" 1 #t)))\n'
            b"; c\n#|\nopen|#\n"
            # 行末で終わる文字リテラル
            b"#\\\n#\\\xce\xbb\n#\\space\n"
        )
        path = tmpfile(src, suffix=".scm")
        s = Session(path)
        s.send(CTRL_Q)
        self.assertEqual(s.finish(), 0)
        os.unlink(path)

    def test_highlight_c_edge_cases(self):
        src = (
            b"int a\x00b; int _x = 1_000; int \xe5\xa4\x89 = 2;\n"  # NUL, _, UTF-8 の識別子
            b"   \n\n"  # 空白だけの行と, 空の行
            b"x = .y + 1.;\n"
            b"y = .\n"
        )
        path = tmpfile(src, suffix=".c")
        s = Session(path)
        s.send(CTRL_Q)
        self.assertEqual(s.finish(), 0)
        os.unlink(path)

    def test_opening_a_comment_repaints_the_lines_below(self):
        path = tmpfile(b"a\nb\nc\n", suffix=".c")
        s = Session(path)
        s.send("/*", DOWN, DOWN, END, "*/", UP, UP, HOME, DELETE, DELETE)
        s.send(CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"a\nb\nc*/\n")
        os.unlink(path)

    def test_long_lines_grow_and_delete_joins_the_next_line(self):
        path = tmpfile(b"ab\ncd\n")
        s = Session(path)
        s.send(END, "x" * 20, DELETE, CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"ab" + b"x" * 20 + b"cd\n")
        os.unlink(path)

    def test_save_as_relative_name(self):
        d = tempfile.mkdtemp()
        s = Session(cwd=d)
        s.send("x", CTRL_S, "rel.c", ENTER, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(os.path.join(d, "rel.c")), b"x\n")
        os.unlink(os.path.join(d, "rel.c"))
        os.rmdir(d)

    def test_prompt_keys(self):
        path = tmpfile(b"".join(b"line %d\n" % i for i in range(100)))
        s = Session(path, rows=10)
        # Ctrl-H で消す, Ctrl-B は入力しない, Ctrl-J で確定する. 遠くの一致は, 画面の中央に置く
        s.send(CTRL_F, "line 8x", b"\x08", b"\x02", "0", b"\n", "!", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertIn(b"\n!line 80\n", read(path))
        os.unlink(path)

    def test_backspace_as_del_on_vt100(self):
        # vt100 の後退キーは ^H なので, DEL (0x7f) は, そのままの文字として届く
        path = tmpfile(b"ab\n")
        s = Session(path, term="vt100")
        s.send("x", b"\x7f", CTRL_F, "a", b"\x7f", "b", ENTER, CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"ab\n")
        os.unlink(path)

    def test_highlight_without_color(self):
        path = tmpfile(b"int x; // c\nchar *s = \"s\"; 1\n", suffix=".c")
        s = Session(path, term="vt100")
        s.send(CTRL_F, "x", ENTER, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        os.unlink(path)

    def test_plain_file_without_extension(self):
        path = tmpfile(b"int x;\n", suffix="")
        s = Session(path)
        s.send(CTRL_Q)
        self.assertEqual(s.finish(), 0)
        os.unlink(path)

    def test_input_that_cannot_be_encoded(self):
        # UTF-8 にできない文字(é を失敗させる)は, 入力しない
        path, s = self.fault_session("wcrtomb:233", b"\n")
        s.send("é", CTRL_F, "é", "\x1b", "a", CTRL_S, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        self.assertEqual(read(path), b"a\n")
        os.unlink(path)

    def test_open_unreadable_file_fails(self):
        if os.geteuid() == 0:
            self.skipTest("root can read any file")
        path = tmpfile(b"x\n")
        os.chmod(path, 0)
        s = Session(path)
        self.assertEqual(s.finish(), 1)
        self.assertIn(b"Permission denied", s.out)
        os.unlink(path)

    def test_open_directory_and_save_fails(self):
        d = tempfile.mkdtemp()
        s = Session(d)
        s.send("x", CTRL_S)  # ディレクトリを, ファイルで置き換えられない
        self.assertIn(b"Can't save!", s.out)
        s.send(CTRL_Q, CTRL_Q, CTRL_Q)
        self.assertEqual(s.finish(), 0)
        for name in os.listdir(d):
            os.unlink(os.path.join(d, name))
        os.rmdir(d)

    def test_save_into_missing_directory_and_too_long_path(self):
        missing = os.path.join(tempfile.gettempdir(), "kilo-no-such-dir", "a.txt")
        # PATH_MAX に近い長さ(一時ファイルの名前が収まらない)
        long_path = "/tmp/kilo-no-such-dir" + ("/" + "d" * 200) * 20 + "/" + "f" * 50
        self.assertEqual(len(long_path), 4092)
        for path in (missing, long_path):
            s = Session(path)
            s.send("x", CTRL_S)
            self.assertIn(b"Can't save!", s.out)
            s.send(CTRL_Q, CTRL_Q, CTRL_Q)
            self.assertEqual(s.finish(), 0)

    def fault_session(self, fault, content=b"x\n"):
        if not FAULT_LIB:
            self.skipTest("needs tests/fault.c (make coverage)")
        path = tmpfile(content)
        return path, Session(path, fault=fault)

    def test_save_failures_keep_the_file(self):
        for fault in ("fdopen", "fwrite", "fputc", "fflush", "fsync", "fchmod", "fclose"):
            with self.subTest(fault=fault):
                path, s = self.fault_session(fault)
                s.send("y", CTRL_S)
                self.assertIn(b"Can't save!", s.out)
                s.send(CTRL_Q, CTRL_Q, CTRL_Q)
                self.assertEqual(s.finish(), 0)
                self.assertEqual(read(path), b"x\n")
                d = os.path.dirname(path)
                base = os.path.basename(path)
                self.assertEqual([n for n in os.listdir(d) if n.startswith(base + ".")], [])
                os.unlink(path)

    def test_out_of_memory(self):
        # ファイル名を写せない
        path, s = self.fault_session("strdup")
        self.assertEqual(s.finish(), 1)
        self.assertIn(b"out of memory", s.out)
        os.unlink(path)
        # 長い行の領域(1 MiB)を確保できない
        path, s = self.fault_session("realloc:1048576", b"a" * 600000 + b"\n")
        self.assertEqual(s.finish(), 1)
        self.assertIn(b"out of memory", s.out)
        os.unlink(path)


if __name__ == "__main__":
    if not os.path.exists(BIN):
        raise SystemExit("build first: make")
    unittest.main(verbosity=2)
