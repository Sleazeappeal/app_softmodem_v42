#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Softmodem test endpoint: stands in for the BBS on the softmodem's TCP side.

Point a test extension's Softmodem() at it, e.g.
  Softmodem(127.0.0.1,2399,v(V22bis)ld(8)s(1)cbt(-16))
Input is
line-based (CR or LF). Two kinds of user share the same port:

Machine (modemtest.py):
  PING                     -> PONG
  SEND <kind> <len> <seed> -> "DATA <len> <crc>\r\n" followed by <len> payload bytes
  RECV <kind> <len> <seed> -> "READY\r\n", then reads <len> bytes and answers
                              "RESULT OK <crc>" or "RESULT BAD first=<off> diff=<n> got=<len>"
  QUIT                     -> BYE, close

Human (terminal program on a retro PC): any other line is a menu choice,
an empty line shows the menu. File transfers use lrzsz (sz/rz) directly on
the socket. Test files are regenerated at start-up in ~/smtest-files.

usage: smtest_server.py [port]   (serves connections one after another until killed)
"""
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

from smtest_common import payload, crc, first_diff

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 2399
CMD = re.compile(rb"(PING|QUIT|SEND|RECV)(?: (\w+) (\d+) (\d+))?")
IDLE = 150          # seconds without input before the call is dropped
XFER_TIMEOUT = 300  # cap for one sz/rz run

FILEDIR = os.path.expanduser("~/smtest-files")
# 8.3 names; 8 KB each so a full download + upload fits in one call at 1200 bps
FILES = {
    "SMTEXT.TXT": ("text", 8192, 101),
    "SMBIN.BIN": ("binary", 8192, 102),
    "SMZERO.BIN": ("zeros", 8192, 103),
    "SMMIXED.BIN": ("mixed", 8192, 104),
}

MENU = (
    "\r\n"
    "SMTEST - softmodem test server\r\n"
    "Press a digit (no ENTER needed); Q + ENTER hangs up.\r\n"
    "\r\n"
    "  1  Text screen (known content, ends with byte count and CRC-32)\r\n"
    "  2  ANSI colour test screen\r\n"
    "  3  Echo mode (everything you type comes back; ESC leaves)\r\n"
    "  4  Download all test files - ZMODEM\r\n"
    "  5  Download all test files - YMODEM batch\r\n"
    "  6  Download one test file  - 1K XMODEM\r\n"
    "  7  Upload the test files back - ZMODEM (server verifies them)\r\n"
    "  8  List test files (size, CRC-32)\r\n"
    "  Q  Hang up\r\n"
    "\r\n"
    "Choice: "
).encode()

BANNER = b"\r\nSMTEST softmodem test server. Press ENTER for the menu.\r\n"


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


def write_files():
    os.makedirs(FILEDIR, exist_ok=True)
    for name, (kind, n, seed) in FILES.items():
        with open(os.path.join(FILEDIR, name), "wb") as f:
            f.write(payload(kind, n, seed))


def text_screen():
    lines = [f"Line {i:02d}: The quick brown fox jumps over the lazy dog. 0123456789 ABCDEFGH"
             for i in range(1, 21)]
    body = ("\r\n".join(lines) + "\r\n").encode()
    return b"\r\n" + body + f"--- end: {len(body)} bytes, CRC-32 {crc(body)} ---\r\n".encode()


def ansi_screen():
    out = ["\x1b[0m\x1b[2J\x1b[H", "ANSI colour test\r\n\r\n"]
    for bold in ("", "1;"):
        out.append("".join(f"\x1b[{bold}{30 + c}m Fg{c} " for c in range(8)) + "\x1b[0m\r\n")
    out.append("".join(f"\x1b[{40 + c}m Bg{c} " for c in range(8)) + "\x1b[0m\r\n\r\n")
    # CP437 box drawing
    out.append("\xc9" + "\xcd" * 30 + "\xbb\r\n")
    out.append("\xba" + " CP437 box: \xb0\xb1\xb2\xdb  \xda\xc4\xbf\xc0\xd9  ".ljust(30) + "\xba\r\n")
    out.append("\xc8" + "\xcd" * 30 + "\xbc\r\n")
    out.append("\x1b[0m\r\n")
    return "".join(out).encode("latin-1")


def run_lrzsz(conn, cmd, cwd=None):
    """Run sz/rz with the socket as stdin/stdout. Returns (exit code, seconds)."""
    conn.setblocking(True)
    log(f"run: {' '.join(cmd)}")
    t = time.time()
    try:
        r = subprocess.run(cmd, stdin=conn.fileno(), stdout=conn.fileno(),
                           stderr=subprocess.PIPE, cwd=cwd, timeout=XFER_TIMEOUT)
        rc, err = r.returncode, r.stderr.decode("latin-1", "replace")
    except subprocess.TimeoutExpired:
        rc, err = -1, f"timeout after {XFER_TIMEOUT}s"
    secs = time.time() - t
    for line in err.splitlines()[-6:]:
        if line.strip():
            log(f"  {cmd[0]}: {line.strip()}")
    log(f"  exit {rc} after {secs:.1f}s")
    conn.settimeout(IDLE)
    return rc, secs


def verify_upload(dirname):
    """Compare uploaded files against the expected test files."""
    report = []
    got = {n.upper(): n for n in os.listdir(dirname)}
    for name, (kind, n, seed) in FILES.items():
        if name not in got:
            report.append(f"  {name:12s} not sent")
            continue
        data = open(os.path.join(dirname, got.pop(name)), "rb").read()
        expected = payload(kind, n, seed)
        if data == expected:
            report.append(f"  {name:12s} OK   {len(data)} bytes, CRC-32 {crc(data)}")
        else:
            first, diff = first_diff(expected, data)
            report.append(f"  {name:12s} BAD  {len(data)} bytes, first difference at {first}, "
                          f"{diff} bytes differ")
    for extra in got.values():
        report.append(f"  {extra:12s} (not a test file, ignored)")
    return report


class Session:
    def __init__(self, conn):
        self.conn = conn
        self.pending = b""

    def read_line(self):
        """Read one input line (used for sub-prompts). None if the call is gone."""
        while not re.search(rb"[\r\n]", self.pending):
            chunk = self.conn.recv(256)
            if not chunk:
                return None
            self.pending += chunk
        line, self.pending = re.split(rb"\r\n|\r|\n", self.pending, maxsplit=1)
        return line.decode("latin-1", "replace")

    def recv_exact(self, n, timeout):
        """n bytes, taking any already-buffered input first."""
        buf, self.pending = bytearray(self.pending[:n]), self.pending[n:]
        self.conn.settimeout(timeout)
        try:
            while len(buf) < n:
                chunk = self.conn.recv(n - len(buf))
                if not chunk:
                    break
                buf += chunk
        except socket.timeout:
            pass
        self.conn.settimeout(IDLE)
        return bytes(buf)

    def machine(self, c):
        """One modemtest.py command. Returns False to end the session."""
        conn = self.conn
        cmd = c.group(1).decode()
        if cmd == "PING":
            conn.sendall(b"PONG\r\n")
            log("PING")
        elif cmd == "QUIT":
            conn.sendall(b"BYE\r\n")
            log("QUIT")
            return False
        elif c.group(2) is not None:
            kind, n, seed = c.group(2).decode(), int(c.group(3)), int(c.group(4))
            data = payload(kind, n, seed)
            if cmd == "SEND":
                conn.sendall(f"DATA {n} {crc(data)}\r\n".encode() + data)
                log(f"SEND {kind} {n}")
            else:
                conn.sendall(b"READY\r\n")
                got = self.recv_exact(n, 15 + n / 100)
                if got == data:
                    conn.sendall(f"RESULT OK {crc(got)}\r\n".encode())
                    log(f"RECV {kind} {n} OK")
                else:
                    first, diff = first_diff(data, got)
                    conn.sendall(f"RESULT BAD first={first} diff={diff} got={len(got)}\r\n".encode())
                    log(f"RECV {kind} {n} BAD first={first} diff={diff} got={len(got)}")
        return True

    def human(self, choice):
        """One menu choice. Returns False to hang up."""
        conn = self.conn
        log(f"menu: {choice!r}")
        if choice == "":
            conn.sendall(MENU)
        elif choice == "1":
            conn.sendall(text_screen() + b"\r\nChoice: ")
        elif choice == "2":
            conn.sendall(ansi_screen() + b"Choice: ")
        elif choice == "3":
            conn.sendall(b"\r\nEcho mode - press ESC to leave.\r\n")
            n = 0
            data, self.pending = self.pending, b""
            while True:
                if not data:
                    data = conn.recv(1024)
                    if not data:
                        return False
                esc = data.find(b"\x1b")
                if esc >= 0:
                    conn.sendall(data[:esc])
                    n += esc
                    break
                conn.sendall(data)
                n += len(data)
                data = b""
            log(f"echo: {n} bytes")
            conn.sendall(f"\r\nLeft echo mode ({n} bytes echoed).\r\n".encode() + MENU)
        elif choice in ("4", "5"):
            proto = "ZMODEM" if choice == "4" else "YMODEM"
            conn.sendall(f"\r\nStarting {proto} download of {len(FILES)} files. Start receiving "
                         f"in your terminal now if it does not start by itself.\r\n".encode())
            time.sleep(1)
            cmd = ["sz", "-b"] + (["--ymodem"] if choice == "5" else []) + list(FILES)
            rc, secs = run_lrzsz(conn, cmd, cwd=FILEDIR)
            time.sleep(1)
            conn.sendall(f"\r\n{proto} download {'finished' if rc == 0 else 'FAILED (exit %d)' % rc} "
                         f"in {secs:.0f}s.\r\n".encode() + MENU)
        elif choice == "6":
            names = list(FILES)
            conn.sendall(b"\r\n" + b"".join(f"  {i + 1}  {n}\r\n".encode() for i, n in enumerate(names))
                         + b"File number: ")
            sel = self.read_line()
            if sel is None:
                return False
            sel = sel.strip()
            if not sel.isdigit() or not 1 <= int(sel) <= len(names):
                conn.sendall(b"\r\nNo such file.\r\n" + MENU)
                return True
            name = names[int(sel) - 1]
            conn.sendall(f"\r\nStart 1K XMODEM receive of {name} in your terminal now.\r\n".encode())
            rc, secs = run_lrzsz(conn, ["sz", "--xmodem", "-k", "-b", name], cwd=FILEDIR)
            time.sleep(1)
            conn.sendall(f"\r\n1K XMODEM {name} {'finished' if rc == 0 else 'FAILED (exit %d)' % rc} "
                         f"in {secs:.0f}s.\r\n".encode() + MENU)
        elif choice == "7":
            # HyperTerminal sends one file per "Send File"; repeat 7 for each file
            conn.sendall(b"\r\nReady for ZMODEM upload. Send one or more of the test files "
                         b"(SMTEXT.TXT, SMBIN.BIN, SMZERO.BIN, SMMIXED.BIN) now.\r\n")
            with tempfile.TemporaryDirectory(prefix="smtest-up-") as d:
                rc, secs = run_lrzsz(conn, ["rz", "-b", "-y"], cwd=d)
                report = verify_upload(d)
            for line in report:
                log(f"upload: {line.strip()}")
            time.sleep(1)
            conn.sendall(f"\r\nZMODEM upload {'finished' if rc == 0 else 'FAILED (exit %d)' % rc} "
                         f"in {secs:.0f}s. Verification:\r\n".encode()
                         + "\r\n".join(report).encode() + b"\r\n" + MENU)
        elif choice == "8":
            lines = [f"  {name:12s} {n:6d} bytes  CRC-32 {crc(payload(kind, n, seed))}  ({kind})"
                     for name, (kind, n, seed) in FILES.items()]
            conn.sendall(b"\r\n" + "\r\n".join(lines).encode() + b"\r\n" + MENU)
        elif choice.upper() == "Q":
            conn.sendall(b"\r\nBye.\r\n")
            return False
        else:
            conn.sendall(b"\r\nUnknown choice.\r\n" + MENU)
        return True

    def run(self):
        conn = self.conn
        conn.settimeout(IDLE)
        conn.sendall(BANNER)
        while True:
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                log(f"idle for {IDLE}s, hanging up")
                conn.sendall(f"\r\nNo input for {IDLE}s, hanging up.\r\n".encode())
                return
            if not chunk:
                log("connection closed by softmodem")
                return
            self.pending += chunk
            # a lone menu digit acts at once, without ENTER (machine commands
            # always start with a letter, so this cannot catch them)
            if re.fullmatch(rb"[1-8]", self.pending):
                choice, self.pending = self.pending.decode(), b""
                if not self.human(choice):
                    return
                continue
            while True:
                m = re.search(rb"\r\n|\r|\n", self.pending)
                if not m:
                    self.pending = self.pending[-512:]
                    break
                line, self.pending = self.pending[:m.start()], self.pending[m.end():]
                c = CMD.search(line)
                ok = self.machine(c) if c else self.human(line.decode("latin-1", "replace").strip())
                if not ok:
                    return


def main():
    write_files()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(1)
    log(f"listening on 127.0.0.1:{PORT}, test files in {FILEDIR}")
    while True:
        conn, addr = srv.accept()
        log(f"connection from {addr}")
        try:
            Session(conn).run()
        except (OSError, ValueError) as e:
            log(f"connection error: {e}")
        finally:
            conn.close()
            log("connection done")


if __name__ == "__main__":
    main()
