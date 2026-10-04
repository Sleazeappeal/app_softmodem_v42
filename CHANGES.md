# Changes

What this project changes compared with PhreakScript's `app_softmodem.c` and
spandsp 0.0.6+dfsg-2.2, and why. Each item was found with a real modem or a
sanitizer test, and verified the same way.

## spandsp `v42.c` (V.42 / LAP-M)

| Change | Why |
|---|---|
| The parsed V.42bis parameters are stored (`v42_update_config()`), new API `v42_set_local_v42bis_request()` / `v42_get_negotiated_v42bis()` | Stock spandsp parses the peer's V.42bis offer from the XID and throws it away (the call was commented out), and never requests compression itself. Without this, V.42bis cannot be negotiated at all. |
| V.42bis negotiation per V.42bis 5.1: a direction is used only if both sides ask for it, dictionary size and string length are the smaller of the two offers, missing values take their defaults, out-of-range values make the XID invalid | The peer's values overwrote ours; invalid values were silently clamped, which could leave the two sides with different dictionaries. |
| HDLC optional functions are sent as 3 octets (group length 19) instead of 4 | Rockwell modems discard an XID response with 4 octets and never send SABME, so no link comes up. |
| The V.42bis group is always sent, with P0 = 0 when declining | Rockwell modems take a missing group as acceptance of their own compression proposal. |
| No V.42bis group from the peer during link setup means "no compression" | Our response otherwise offered compression to a peer that had not asked for it. |
| The F bit of an RR echoes the P bit of the I-frame it acknowledges | Stock spandsp always set F = 1. Rockwell modems ignore such unsolicited final responses, so uploads only advanced on their retransmission timer (43 B/s, broken ZMODEM uploads). |
| A duplicate I-frame (N(S) in [V(R)-k, V(R)-1]) is discarded without REJ | A REJ makes the peer resend frames that are already on the way; they arrive as duplicates again, and every frame of an upload is sent twice (seen with a HaM softmodem). |
| XID parsing checks every group and parameter against the frame length first | The stock parser used `uint16_t` lengths, so its overrun checks never fired: a malformed XID made it read past the frame (ASan). |
| The XID user data subfield (group ID 0xFF, no length field) ends the parsing | It was read as a normal group and the whole XID rejected. Conexant modems put their V.44 offer there and got no XID response. |
| An XID that could not be parsed is not answered | Answering it confirmed parameters we had not seen. |
| Frames too short for N(R) are ignored before the handlers read it | Out-of-bounds read on 2-byte frames. |
| `rejected` is reset in `reset_lapm()` | A REJ condition from before a reset suppressed the first REJ after it. |
| T400 (detection phase) 2000 ms instead of 750 ms | T400 starts at carrier up, before the 2400 bit/s training; real modems send their detection pattern 0.7-1.1 s later. |
| Bad-CRC frames are reported; frame trace, XID hex dump and detection messages at `SPAN_LOG_FLOW` | Interop problems were otherwise impossible to see. Off unless a log level is set. |

## spandsp `v42bis.c`

| Change | Why |
|---|---|
| Decoder: codewords beyond the dictionary, strings longer than the buffer and STEPUPs past the dictionary size are rejected | A corrupt or hostile codeword stream wrote past the string buffer or looped (ASan: SEGV). |
| `v42bis_init()` accepts at most 4096 codewords | The dictionary array has 4096 entries; up to 65535 were accepted. |
| `v42bis_free()` frees the state | `v42bis_init(NULL, ...)` allocates it, but stock `v42bis_free()` never freed it: about 68 KB lost per call. |

## spandsp `v22bis_rx.c`

| Change | Why |
|---|---|
| A second S1 detector for the answering side, based on runs of alternating phase steps | The stock check slices unequalised points into quadrants. S1 points are only about 57 degrees apart at that stage, so depending on the carrier phase S1 was missed in about 40 % of calls, which then trained at 1200 instead of 2400 bit/s. |

## `app_softmodem.c`

| Change | Why |
|---|---|
| Option `c`: V.42 for V.22/V.22bis, with fallback to async framing when the detection phase times out | - |
| Option `b`: V.42bis, started at the first I-frame | The peer's decoder is live from link setup; any delay desynchronises the dictionaries. |
| With `c` alone the XID explicitly requests no compression | `v42_init()` defaults to requesting it, so the modem reported V.42bis while the softmodem ran no codec. |
| The negotiated P0 is passed to the codec per direction (bits swapped when we send the XID command) | One-way compression would otherwise run the wrong codec direction. |
| V.42 timers run at the trained bit rate | spandsp counts its timers in bits and assumes 28800 bit/s: at 2400 every timer was 12 times too long. |
| Reads from the server are bounded by the room left in the V.42bis transmit queue, and flushed once per burst | A full queue dropped compressed data in the middle of the stream, corrupting everything after it. Flushing per read added codewords and, in the middle of a match, held data back. |
| Writes to the server wait for room (up to 2 s) and end the session if they fail | Data the modem sent (and V.42 already acknowledged) was dropped when the socket buffer was full. |
| 0.5 s of idle marks before the first async character after the fallback | Data queued during detection went out back to back, and the modem's receiver locked onto a wrong start bit until the first pause. |
| Clean call end (`softmodem_drain()`): send the rest while the modem keeps acknowledging, DISC, wait for UA/DM, 1.5 s idle carrier, 2 s silence, then hang up | Hanging up at once lost the last data (a BBS's goodbye screen) and left some modems online, printing line noise. |
| A V.42 link released by the modem, a socket error, a decoder error or a failed codec start end the session | The session kept running, or passed compressed data on as plain data. |
| spandsp messages go to `ast_debug()` through a per-channel handler, and are only enabled when debug is on | `res_fax_spandsp` clears spandsp's global handler; formatting every frame trace line costs time on the audio path. |
| The generator is stopped before the modem objects are freed; `close()` on a failed connect; failed DNS lookup handled; session zeroed | Use after free when the TCP side ended the call; file descriptor leak; NULL dereference; uninitialised options. |

The other modulations, TDD, TLS and session tracking are unchanged.
