#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Modem-side softmodem test: dial the softmodem with a hardware modem on a
serial port, run a transfer suite against smtest_server.py, verify every
byte, hang up, report.

usage: modemtest.py --device /dev/ttyUSB0 --number <ext> [--init AT...]
                    [--suite down:text:8192,up:binary:1024,...]
                    [--json result.json] [--idle 8] [--max-secs 300]

Suite items are <dir>:<kind>:<len>; dir "down" = softmodem -> modem (our encoder,
the modem's decoder), "up" = modem -> softmodem (the modem's encoder, our decoder),
"last" = like down, but the server closes the connection right after the data
(tests the softmodem's call end; must be the last item).

The default --init and the AT&V1 diagnostics parsing suit Rockwell chipsets;
the modem needs DTR ignored (&D0) if the serial cable does not carry it.

Timeouts: a transfer fails after --idle seconds without a new byte; every test
also has a hard cap; the whole call is aborted (and hung up) after --max-secs.
Before every test the line is drained and re-synced with PING/PONG, so one
failing test does not poison the next; if sync fails, the rest is skipped.
"""
import argparse
import fcntl
import json
import os
import re
import select
import signal
import struct
import sys
import termios
import time

from smtest_common import payload, crc, first_diff

DEFAULT_SUITE = ("down:text:8192,down:ascii:2048,down:binary:2048,down:zeros:8192,down:mixed:4096,"
                 "up:text:2048,up:binary:1024,up:mixed:2048")
FINAL = ("CONNECT", "NO CARRIER", "BUSY", "NO DIALTONE", "NO ANSWER", "ERROR")

t0 = time.time()
IDLE = 8
UP_CPS = 108  # upload pacing, set from the carrier rate after CONNECT


class Deadline(Exception):
    pass


def log(msg):
    print(f"{time.time() - t0:7.2f} {msg}", flush=True)


class Port:
    def __init__(self, dev):
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = 0; a[1] = 0; a[3] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL | termios.CRTSCTS
        a[4] = a[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.buf = b""

    def write(self, data):
        """Write everything, waiting for the modem's CTS (kernel flow control).
        Gives up after IDLE seconds without progress."""
        view = memoryview(data)
        last = time.time()
        while view:
            if time.time() - last > IDLE:
                return False
            select.select([], [self.fd], [], 1)
            try:
                n = os.write(self.fd, view)
            except BlockingIOError:
                continue
            if n:
                last = time.time()
            view = view[n:]
        return True

    def fill(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if r:
            try:
                chunk = os.read(self.fd, 4096)
            except BlockingIOError:
                return False
            self.buf += chunk
            return bool(chunk)
        return False

    def wait_for(self, pattern, timeout):
        """Wait until regex pattern (bytes) matches the buffer; consume up to the match end."""
        end = time.time() + timeout
        rx = re.compile(pattern)
        while True:
            m = rx.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            if time.time() > end:
                return None
            self.fill(min(0.2, max(0.0, end - time.time())))

    def read_exact(self, n, timeout):
        """Read n bytes; give up after `timeout` overall or IDLE seconds without data."""
        end = time.time() + timeout
        last = time.time()
        while len(self.buf) < n and time.time() < end and time.time() - last < IDLE:
            if self.fill(0.2):
                last = time.time()
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def drain(self, quiet=1.0, cap=10.0):
        """Discard input until the line has been quiet for `quiet` seconds."""
        end = time.time() + cap
        last = time.time()
        while time.time() - last < quiet and time.time() < end:
            if self.fill(0.2):
                last = time.time()
        dropped, self.buf = len(self.buf), b""
        return dropped

    def command(self, cmd, wait=2.0):
        """AT command in command mode; returns the response text."""
        self.buf = b""
        self.write(cmd.encode() + b"\r")
        m = self.wait_for(rb"\r\n(OK|ERROR)\r\n", wait)
        return (m.string[:m.end()] if m else self.buf).decode("latin-1", "replace")

    def lines(self):
        v = struct.unpack("I", fcntl.ioctl(self.fd, termios.TIOCMGET, struct.pack("I", 0)))[0]
        return " ".join(n for n in ("CTS", "DSR", "CAR") if v & getattr(termios, "TIOCM_" + n)) or "-"


def run_down(p, kind, n, seed):
    data = payload(kind, n, seed)
    t = time.time()
    p.write(f"SEND {kind} {n} {seed}\r".encode())
    m = p.wait_for(rb"DATA (\d+) ([0-9a-f]{8})\r\n", IDLE)
    if not m:
        return {"ok": False, "error": "no DATA header", "garbage": p.buf[:80].decode("latin-1", "replace")}
    t_first = time.time()
    got = p.read_exact(n, 15 + n / 120)
    secs = time.time() - t_first
    res = {"ok": got == data, "bytes": len(got), "secs": round(secs, 2),
           "cps": round(len(got) / secs) if secs > 0 else None, "latency": round(t_first - t, 2)}
    if got != data:
        first, diff = first_diff(data, got)
        res.update(first_diff=first, diff=diff, crc_hdr=m.group(2).decode(), crc_got=crc(got))
    return res


def paced_write(p, data, cps):
    """Some modems (a Rockwell RCV56) never drop CTS and silently discard
    what overflows their ~2.5 KB buffer. So feed them no faster than the line
    can carry."""
    t = time.time()
    for i in range(0, len(data), 128):
        if not p.write(data[i:i + 128]):
            return False
        ahead = t + (i + 128) / cps - time.time()
        if ahead > 0:
            time.sleep(ahead)
    return True


def run_up(p, kind, n, seed):
    data = payload(kind, n, seed)
    p.write(f"RECV {kind} {n} {seed}\r".encode())
    if not p.wait_for(rb"READY\r\n", IDLE):
        return {"ok": False, "error": "no READY", "garbage": p.buf[:80].decode("latin-1", "replace")}
    t = time.time()
    if not paced_write(p, data, UP_CPS):
        return {"ok": False, "error": "write stalled (CTS)", "secs": round(time.time() - t, 2)}
    m = p.wait_for(rb"RESULT (OK|BAD)([^\r\n]*)\r\n", 20 + n / 100)
    secs = time.time() - t
    if not m:
        return {"ok": False, "error": "no RESULT", "secs": round(secs, 2)}
    return {"ok": m.group(1) == b"OK", "bytes": n, "secs": round(secs, 2),
            "cps": round(n / secs) if secs > 0 else None,
            "detail": m.group(2).decode("latin-1").strip()}


def run_last(p, kind, n, seed):
    """SEND and QUIT in one go: the server writes the payload and BYE and
    closes at once, so the softmodem must deliver everything after the TCP
    close (softmodem_drain()) and then end the call. Must be the last test."""
    data = payload(kind, n, seed)
    t = time.time()
    p.write(f"SEND {kind} {n} {seed}\rQUIT\r".encode())
    if not p.wait_for(rb"DATA (\d+) ([0-9a-f]{8})\r\n", IDLE):
        return {"ok": False, "error": "no DATA header"}
    got = p.read_exact(n, 15 + n / 120)
    bye = p.wait_for(rb"BYE\r\n", 10) is not None
    carrier = p.wait_for(rb"NO CARRIER\r\n", 15) is not None
    res = {"ok": got == data and bye and carrier, "bytes": len(got), "bye": bye,
           "no_carrier": carrier, "secs": round(time.time() - t, 2)}
    if got != data:
        res["first_diff"], res["diff"] = first_diff(data, got)
    return res


def sync(p, tries=5):
    """Drop leftovers from the previous test, then PING until PONG."""
    dropped = p.drain(quiet=0.5, cap=5)
    for _ in range(tries):
        p.write(b"PING\r")
        # 2 s, not 1.5: the measured round trip at 1200 bps is ~1.4 s
        if p.wait_for(rb"PONG\r\n", 2):
            return True, dropped
    return False, dropped


def on_alarm(signum, frame):
    raise Deadline()


def main():
    global IDLE, UP_CPS
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", required=True, help="serial port of the modem")
    ap.add_argument("--number", required=True, help="number to dial (the softmodem extension)")
    ap.add_argument("--init", default="ATE1V1W1X4&D0S95=32")
    ap.add_argument("--suite", default=DEFAULT_SUITE)
    ap.add_argument("--json")
    ap.add_argument("--idle", type=int, default=8)
    ap.add_argument("--max-secs", type=int, default=300)
    args = ap.parse_args()
    IDLE = args.idle
    suite = args.suite.split(",")
    signal.signal(signal.SIGALRM, on_alarm)
    signal.alarm(args.max_secs)

    result = {"start": time.strftime("%Y-%m-%d %H:%M:%S"), "number": args.number, "init": args.init,
              "connect": {}, "tests": []}
    p = Port(args.device)
    try:
        # reload the stored profile first, so settings from an earlier test
        # (e.g. AT%C0) cannot leak into this one
        p.command("ATZ", 3)
        p.command(args.init)
        log(f"dial {args.number}")
        p.buf = b""
        p.write(f"ATDT{args.number}\r".encode())
        m = p.wait_for(("(" + "|".join(FINAL) + ")[^\r\n]*\r\n").encode(), 30)
        head = p.buf  # whatever followed the final result
        connect_text = (m.string[:m.end()] if m else b"").decode("latin-1", "replace")
        for key in ("CARRIER", "PROTOCOL", "COMPRESSION", "CONNECT"):
            mm = re.search(key + r":? ?([^\r\n]*)", connect_text)
            if mm:
                result["connect"][key.lower()] = mm.group(1).strip()
        result["connect"]["secs"] = round(time.time() - t0, 1)
        if not m or not m.group(1).startswith(b"CONNECT"):
            log(f"no connect: {connect_text.strip()!r}")
            result["error"] = "no connect"
        else:
            log(f"connected: {result['connect']}, lines {p.lines()}")
            # 90 % of the async character rate: safe with or without V.42
            UP_CPS = int(result["connect"].get("carrier", "1200")) // 10 * 9 // 10
            p.buf = head
            for i, item in enumerate(suite):
                ok, dropped = sync(p)
                if not ok:
                    log(f"sync FAILED before {item} (dropped {dropped} bytes), skipping the rest")
                    result["error"] = f"sync lost before {item}"
                    break
                if dropped:
                    log(f"sync: dropped {dropped} leftover bytes")
                direction, kind, n = item.split(":")
                run = {"down": run_down, "up": run_up, "last": run_last}[direction]
                res = run(p, kind, int(n), i)
                res["test"] = item
                result["tests"].append(res)
                log(f"{item:20s} {'PASS' if res['ok'] else 'FAIL'} "
                    + " ".join(f"{k}={v}" for k, v in res.items() if k not in ("ok", "test")))
            p.write(b"QUIT\r")
            p.wait_for(rb"BYE\r\n", 5)
    except Deadline:
        log(f"DEADLINE: call aborted after {args.max_secs}s")
        result["error"] = f"deadline {args.max_secs}s"

    signal.alarm(30)  # hang-up and diagnostics must not hang either
    try:
        p.drain(quiet=0.5, cap=3)
        time.sleep(1.2)
        p.write(b"+++")
        p.wait_for(rb"OK\r\n", 3)
        p.command("ATH0", 5)
        diag = p.command("AT&V1", 3)
        result["diag"] = {k.strip(): v.strip()
                          for k, v in re.findall(r"([A-Za-z][A-Za-z ]+?)\.{2,}\s*([^\r\n]*)", diag)}
    except Deadline:
        log("DEADLINE during hang-up")
        result["diag"] = {}
    signal.alarm(0)
    os.close(p.fd)

    passed = sum(1 for t in result["tests"] if t["ok"])
    skipped = len(suite) - len(result["tests"])
    result["summary"] = f"{passed}/{len(suite)} passed" + (f" ({skipped} skipped)" if skipped else "")
    d = result.get("diag", {})
    log(f"summary: {result['summary']}; modem: {d.get('TERMINATION REASON')}, "
        f"rates tx {d.get('LAST TX rate')} rx {d.get('LAST RX rate')}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(result, f, indent=1)
    sys.exit(0 if passed == len(suite) else 1)


if __name__ == "__main__":
    main()
