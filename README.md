# V.42 / V.42bis for Asterisk app_softmodem

## Credits

This project builds on the work of others:

- **app_softmodem** was written by **Christian Groeger** (2010), based on
  Asterisk's `app_fax.c` by **Dmitry Andrianov** and **Steve Underwood**.
  Original repository: [proquar/asterisk-Softmodem](https://github.com/proquar/asterisk-Softmodem).
- Parity options by **Rob O'Donnell** (2018).
- Asterisk 18+ compatibility, TDD (Baudot), Bell 202, originate mode, TLS and
  many fixes by **Naveen Albert** (2021, 2023), maintained in
  [InterLinked1/phreakscript](https://github.com/InterLinked1/phreakscript)
  ([`apps/app_softmodem.c`](https://github.com/InterLinked1/phreakscript/blob/master/apps/app_softmodem.c)).
  **This project starts from that version.**
- **spandsp** by **Steve Underwood**, which provides the modems, V.42 and
  V.42bis ([freeswitch/spandsp](https://github.com/freeswitch/spandsp); here
  the Debian package 0.0.6+dfsg-2.2).
- The V.42bis negotiation follows **slmodem** by Smart Link Ltd. as a
  reference.

Thank you to all of them.

The changes in this project (the V.42/V.42bis integration, the spandsp
fixes, the test tools and this documentation) were developed with
**Claude Opus 5.5** (Anthropic), working in Claude Code, together with the
repository owner, who did all the testing with the real modems and made the
design decisions. The code was also reviewed with **GPT-6.1 SOL** (OpenAI Codex).

## What this adds

`app_softmodem` lets Asterisk answer analogue modem calls and connect the
caller to a TCP service, for example a BBS. This project adds what a real
1990s modem expects on a V.22bis (2400 bit/s) or V.22 (1200 bit/s) call:

- **V.42 (LAP-M) error correction** (option `c`), with fallback to plain
  async data when the caller does not do V.42;
- **V.42bis data compression** on top of V.42 (option `b`);
- **a clean call end** when the TCP service closes the connection: the rest
  of the data is delivered, the V.42 link is released with DISC, and the
  carrier stops before the call is hung up.

It uses spandsp's V.42 and V.42bis code, which needed a number of fixes
before real modems would work with it. Those fixes are part of this project
as a patch against spandsp 0.0.6.

## Status

Runs on Debian 13 with Asterisk 22.11 and spandsp 0.0.6+dfsg-2.2 (the
Debian package). Tested with these modems, calling through an analogue
telephone adapter (Grandstream HT802, Cisco SPA112) and a SIP PBX:

| Modem | V.42 + V.42bis | V.42 only | no error correction |
|---|---|---|---|
| Rockwell RCV56DPF (V.90, external) | yes | yes | yes |
| Rockwell RCV336DPFSP (V.34) | yes | yes | yes |
| USRobotics Sportster 28800 V.FC | yes | yes | yes |
| HaM / Smart Link MD5628D-L-A PCI softmodem (Windows ME) | yes | not tested | see limitations |
| Conexant SoftK56 "SoftV92" PCI softmodem | yes | not tested | not tested |

Tests covered ZMODEM and YMODEM transfers in both directions, and automated
calls with byte-exact checks of compressible, random and mixed data (see
`tools/`). Only the answering side (the softmodem answers, a modem calls) has
been tested.

## Contents

| Path | What |
|---|---|
| `src/app_softmodem.c` | the module with all changes |
| `patches/app_softmodem-v42.patch` | the same changes as a patch against PhreakScript's `apps/app_softmodem.c` |
| `patches/spandsp-0.0.6-v42-v42bis-v22bis.patch` | the spandsp fixes, against spandsp 0.0.6+dfsg-2.2 |
| `tools/` | test server, modem-side test client, offline decoder, sanitizer tests |
| `CHANGES.md` | what was changed and why |

The module is based on PhreakScript's `app_softmodem.c` (see Credits).

## Building

### 1. Patched spandsp

The module calls functions that only the patched spandsp has, and its
`v42_state_t` layout differs from stock spandsp, so the patched library is
required. On Debian:

```sh
apt-get source spandsp=0.0.6+dfsg-2.2
cd spandsp-0.0.6+dfsg
patch -p1 < /path/to/patches/spandsp-0.0.6-v42-v42bis-v22bis.patch
autoreconf -fi
CFLAGS="$(dpkg-buildflags --get CFLAGS) -Wno-error=incompatible-pointer-types" \
CPPFLAGS="$(dpkg-buildflags --get CPPFLAGS)" \
LDFLAGS="$(dpkg-buildflags --get LDFLAGS)" \
./configure --prefix=/usr --libdir=/usr/lib/x86_64-linux-gnu
make -C src
```

Then replace the installed library with `src/.libs/libspandsp.so.2.0.0`
(keep a copy of the original) and run `ldconfig`. The patch only changes
V.42, V.42bis and the V.22bis receiver; other users of libspandsp (for
example `res_fax_spandsp`) are not affected. A package upgrade of
`libspandsp2` will overwrite the patched library.

### 2. The Asterisk module

`app_softmodem` is not part of Asterisk: put `src/app_softmodem.c` into the
`apps/` directory of an Asterisk source tree. It must be compiled against the
**patched** spandsp headers, so put the patched source tree first on the
include path:

```sh
cp src/app_softmodem.c /path/to/asterisk/apps/
cd /path/to/asterisk
make ASTCFLAGS="-I/path/to/spandsp-0.0.6+dfsg/src" apps
make install
```

Building against the stock headers compiles, but the module then reads
spandsp's internal structures at the wrong offsets.

## Usage

```
exten => _X.,1,Answer()
 same => n,Wait(1)
 same => n,Softmodem(127.0.0.1,2323,v(V22bis)ld(8)s(1)cbt(-16))
 same => n,Hangup()
```

| Option | Meaning |
|---|---|
| `v(V22bis)` / `v(V22)` | 2400 or 1200 bit/s; `c` and `b` only work with these two |
| `c` | V.42 error correction, with fallback to async data |
| `b` | V.42bis compression (needs `c`); used only if the caller offers it |
| `t(-16)` | transmit level in dBm0. The default -28 was too weak for some softmodem receivers behind an ATA; -16 worked with every modem above |
| `l`, `d(8)`, `s(1)` | LSB first, 8 data bits, 1 stop bit (async framing) |

The other options (`r`, `e`, `o`, `m`, `n`, `u`, `x`, `f` and the other
modulations) are unchanged from PhreakScript's module.

What happens on a call with `c`:

1. V.22bis training. After carrier up, the V.42 detection phase runs for up
   to 2 s (T400; V.42 says 750 ms, but it starts before the 2400 bit/s
   training, and real modems answer 0.7-1.1 s after carrier up).
2. With V.42: the XID exchange agrees on frame size, window and V.42bis, then
   LAP-M carries the data. Without V.42: 0.5 s of idle marks, then async data.
3. When the TCP service closes: the rest of the data is sent (as long as the
   modem keeps acknowledging), then DISC, 1.5 s of idle carrier, 2 s of
   silence, hang-up. If the modem releases the link, the call ends too.

### Logging

`V.42 error correction active`, `V.42bis compression active` and similar
lines are logged at verbose level 2-3. A full trace of the V.22bis training
and of every LAP-M frame is available at debug level:

```
core set debug 1 app_softmodem.so
```

It takes effect from the next call, and the logger must write debug messages
somewhere (`logger.conf`).

## Known limitations

- Not implemented: V.44, MNP, SREJ, V.8. Modems that offer them fall back
  to V.42bis, V.42 and the V.22bis handshake by themselves.
- The calling side (`f` option) with V.42 has not been tested. It would
  matter if the softmodem dialled out to a modem, for example for
  BBS-to-BBS mail exchange; a BBS that only answers calls does not use it.
- TLS (`x`) has not been tested together with V.42. It would matter if the
  TCP service ran on another host; with the service on the same machine
  (`127.0.0.1`) there is nothing to encrypt.
- Without error correction, some softmodems (the HaM above) do not notice
  that the carrier is gone and keep printing line noise after the call ends.
  An ATA that cuts the loop current on hang-up ("current disconnect") fixes
  this. The same HaM also prints a short burst of garbage at the start of a
  call without error correction, until it retrains by itself.
- One call from the Conexant modem (out of six) failed to train at
  2400 bit/s; not reproduced.
- spandsp 0.0.6 only. Newer spandsp versions have a different V.42 code base
  and the patch does not apply.

### Not fixed in the original module

A code review found these in parts of `app_softmodem.c` this project did not
change, on paths it does not use. They are listed so that anyone using those
parts knows about them:

- **TLS (`x`):** the read paths decide whether to retry from `errno` instead
  of `SSL_get_error()`, so a temporary TLS condition can end the session and
  a real error can be retried; the handshake loop spins on `WANT_READ` and
  does not handle `WANT_WRITE`; the server certificate is not verified (the
  code says so itself).
- **TDD (Baudot, `v(baudot45)` / `v(baudot50)`):** data read from the socket
  is stored at `buf + 2` but terminated at `buf[pres]`, so the last two bytes
  of each read are cut off and a stale byte can remain.
- **Async framing:** the receiver always consumes 10 bits per character,
  which is only right for 10-bit frames such as 8N1 or 7E1; 8N2 or 7N1
  framing is decoded wrong. With V.42 (or its 8N1 fallback) this does not
  apply.
- **FSK modes (V.21, V.23, Bell 103, Bell 202):** the FSK modem objects are
  never freed (a small leak per call), and some early error returns skip
  restoring the channel's audio formats.

## Tools

See `tools/`. They need Python 3 and, for the C tools, the patched spandsp.

| Tool | What |
|---|---|
| `smtest_server.py [port]` | stands in for the TCP service: a menu for terminal programs (text screens, echo, ZMODEM/YMODEM/XMODEM via lrzsz) and a line protocol for `modemtest.py` |
| `modemtest.py --device <tty> --number <ext>` | dials the softmodem with a modem on a serial port, runs a transfer suite against `smtest_server.py` and checks every byte |
| `smtest_common.py` | deterministic test payloads shared by both |
| `v22decode.c` | decodes one side of a call recording (8 kHz WAV) as V.22bis + 8N1, to see what was really on the line |
| `fuzz_regress.c`, `fuzz_regress.sh <spandsp dir>` | AddressSanitizer/UBSan tests of the XID parser and the V.42bis codec with malformed and real inputs |

## See also

[synchronet-bbs-modem-dialin](https://github.com/Sleazeappeal/synchronet-bbs-modem-dialin):
the complete setup this module was written for. A Synchronet BBS that real
modems dial over VoIP, with the Asterisk and PBX configuration, a
byte-transparent RLogin relay, the ATA settings and an installation guide.

## License

GNU General Public License version 2 (`LICENSE`), as the original module and
Asterisk: `src/`, `patches/app_softmodem-v42.patch` and `tools/`.
`patches/spandsp-0.0.6-v42-v42bis-v22bis.patch` changes spandsp and is under
spandsp's license, the GNU Lesser General Public License version 2.1.
