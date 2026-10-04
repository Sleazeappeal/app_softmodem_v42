/*
 * Softmodem for Asterisk
 *
 * 2010, Christian Groeger <code@proquari.at>
 *
 * Based on app_fax.c by Dmitry Andrianov <asterisk@dima.spb.ru>
 * and Steve Underwood <steveu@coppice.org>
 *
 * Parity options added 2018 Rob O'Donnell
 *
 * Compiler fixes for Asterisk 18, XML documentation, bug fixes,
 * TDD (45.45 bps and 50 bps Baudot code) capabilities, Bell 202,
 * originate mode, TLS support, and coding guidelines fixes and updates,
 * by Naveen Albert <asterisk@phreaknet.org>, 2021, 2023.
 * Note that default behavior has been changed from MSB to LSB.
 *
 * V.42 error correction and V.42bis compression for V.22/V.22bis added 2026
 * for PiranjaBBS, tested against Rockwell, USRobotics, Smart Link (HaM) and
 * Conexant (SoftK56) modems:
 * - option 'c': V.42 (LAP-M) using spandsp's v42.c, with fallback to plain
 *   async framing when the caller does not answer V.42 detection;
 * - option 'b': V.42bis on top of V.42;
 * - a clean call end when the TCP side closes (flush, V.42 DISC, idle
 *   carrier, silence).
 * Needs a patched spandsp 0.0.6 (v42.c, v42bis.c, v22bis_rx.c): stock
 * spandsp discards the negotiated V.42bis parameters and has several LAP-M
 * and XID bugs that real modems do not tolerate.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 *
 */

/*** MODULEINFO
	<depend>spandsp</depend>
	<support_level>extended</support_level>
***/

/* Needed for spandsp headers */
#define ASTMM_LIBC ASTMM_IGNORE
#include "asterisk.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>
#include <errno.h>
#include <tiffio.h>
#include <time.h>

#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/opensslv.h>
#endif

/* For TDD stuff */
#define SPANDSP_EXPOSE_INTERNAL_STRUCTURES

#include <spandsp.h>
#ifdef HAVE_SPANDSP_EXPOSE_H
#include <spandsp/expose.h>
#endif
#include <spandsp/v22bis.h>
#include <spandsp/v18.h>
#include <spandsp/v42.h>
#include <spandsp/v42bis.h>

/* For TDD stuff */
#define SPANDSP_EXPOSE_INTERNAL_STRUCTURES
#include <spandsp/version.h>
#include <spandsp/logging.h>
#include <spandsp/fsk.h>
#include <spandsp/async.h>

#include "asterisk/file.h"
#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/strings.h"
#include "asterisk/lock.h"
#include "asterisk/app.h"
#include "asterisk/pbx.h"
#include "asterisk/format_cache.h"
#include "asterisk/logger.h"
#include "asterisk/utils.h"
#include "asterisk/dsp.h"
#include "asterisk/manager.h"

/*** DOCUMENTATION
	<application name="Softmodem" language="en_US">
		<synopsis>
			A Softmodem that connects the caller to a Telnet server (TCP port).
		</synopsis>
		<syntax>
			<parameter name="hostname" required="false">
				<para>Hostname. Default is 127.0.0.1</para>
			</parameter>
			<parameter name="port" required="false">
				<para>Port. Default is 23 (default Telnet port).</para>
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="d">
						<para>Amount of data bits (5-8, default 8)</para>
					</option>
					<option name="e">
						<para>Even parity bit</para>
					</option>
					<option name="f">
						<para>Flip the mode from answering to originating, for modes (e.g. Bell 103)
						that use different settings for the originating and answering sides.</para>
					</option>
					<option name="l">
						<para>Least significant bit first (default, but in older versions, MSB was the default,
						so as a best practice, always specify the LSB/MSB mode explicitly).</para>
					</option>
					<option name="m">
						<para>Most significant bit first (default is least significant bit)</para>
					</option>
					<option name="n">
						<para>Send NULL-Byte to modem after carrier detection (Btx)</para>
					</option>
					<option name="o">
						<para>Odd parity bit</para>
					</option>
					<option name="r">
						<para>RX cutoff (dBi, float, default: -35)</para>
					</option>
					<option name="s">
						<para>Amount of stop bits (1-2, default 1)</para>
					</option>
					<option name="t">
						<para>tx power (dBi, float, default: -28)</para>
					</option>
					<option name="u">
						<para>Send ULM header to Telnet server (Btx)</para>
					</option>
					<option name="v">
						<para>Modem version</para>
						<enumlist>
							<enum name="V21">
								<para>300/300 baud</para>
							</enum>
							<enum name="V23">
								<para>1200/75 baud (default)</para>
							</enum>
							<enum name="Bell103">
								<para>300/300 baud</para>
							</enum>
							<enum name="Bell202">
								<para>1200/1200 baud</para>
							</enum>
							<enum name="V22">
								<para>1200/1200 baud</para>
							</enum>
							<enum name="V22bis">
								<para>2400/2400 baud</para>
							</enum>
							<enum name="baudot45">
								<para>V18 45.45bps TDD (US TTY) Baudot code</para>
							</enum>
							<enum name="baudot50">
								<para>V18 50bps TDD (international) Baudot code</para>
							</enum>
						</enumlist>
					</option>
					<option name="x">
						<para>Use TLS encryption for the data connection.</para>
					</option>
					<option name="c">
						<para>Enable V.42 (LAP-M) error correction. Only applies to V22/V22bis.
						Falls back to raw (uncorrected) mode automatically if the far end does not
						support V.42.</para>
					</option>
					<option name="b">
						<para>Enable V.42bis data compression on top of V.42 (requires option 'c').
						Only takes effect if the far end offers V.42bis; otherwise V.42 runs
						uncompressed, as with 'c' alone. Requires a patched spandsp.</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>Simulates a FSK(V.23), V.22bis, or Baudot modem. The modem on the other end is connected to the specified server using a simple TCP connection (like Telnet).</para>
			<example title="Bell 103 modem gateway to local Telnet port">
			same => n,Softmodem(127.0.0.1,23,v(Bell103)l)
			</example>
			<example title="TAP terminal - 7 data bits, even parity">
			same => n,Softmodem(127.0.0.1,4827,v(Bell103)led(7))
			</example>
		</description>
	</application>
	<manager name="SoftmodemSessions" language="en_US" module="res_xmpp">
		<synopsis>
			List all tracked softmodem sessions.
		</synopsis>
		<syntax>
			<xi:include xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])" />
			<parameter name="Port" required="false">
				<para>Port used by softmodem client for connection. This is the local client port,
				not the remote server-side port to which the client connected.</para>
				<para>If provided, only the session with this local port will be returned.
				This can be used to retrieve information about a softmodem session from
				another system, which can correlate the session using the TCP port number.</para>
			</parameter>
		</syntax>
		<description>
			<para>List information about actively tracked softmodem sessions.</para>
		</description>
	</manager>
 ***/

static const char app[] = "Softmodem";

enum {
	OPT_RX_CUTOFF =      (1 << 0),
	OPT_TX_POWER =       (1 << 1),
	OPT_MODEM_VERSION =  (1 << 2),
	OPT_LSB_FIRST =      (1 << 3),
	OPT_MSB_FIRST =      (1 << 4),
	OPT_DATABITS =       (1 << 5),
	OPT_STOPBITS =       (1 << 6),
	OPT_ULM_HEADER =     (1 << 7),
	OPT_NULL =           (1 << 8),
	OPT_EVEN_PARITY = 	 (1 << 9),
	OPT_ODD_PARITY = 	 (1 << 10),
	OPT_TLS =            (1 << 11),
	OPT_FLIP_MODE =      (1 << 12),
	OPT_V42 =            (1 << 13),
	OPT_V42BIS =         (1 << 14),
};

enum {
	OPT_ARG_RX_CUTOFF,
	OPT_ARG_TX_POWER,
	OPT_ARG_MODEM_VERSION,
	OPT_ARG_DATABITS,
	OPT_ARG_STOPBITS,
	/* Must be the last element */
	OPT_ARG_ARRAY_SIZE,
};

AST_APP_OPTIONS(additional_options, BEGIN_OPTIONS
	AST_APP_OPTION_ARG('r', OPT_RX_CUTOFF, OPT_ARG_RX_CUTOFF),
	AST_APP_OPTION_ARG('t', OPT_TX_POWER, OPT_ARG_TX_POWER),
	AST_APP_OPTION_ARG('v', OPT_MODEM_VERSION, OPT_ARG_MODEM_VERSION),
	AST_APP_OPTION('l', OPT_LSB_FIRST),
	AST_APP_OPTION('m', OPT_MSB_FIRST),
	AST_APP_OPTION_ARG('d', OPT_DATABITS, OPT_ARG_DATABITS),
	AST_APP_OPTION_ARG('s', OPT_STOPBITS, OPT_ARG_STOPBITS),
	AST_APP_OPTION('e', OPT_EVEN_PARITY),
	AST_APP_OPTION('o', OPT_ODD_PARITY),
	AST_APP_OPTION('u', OPT_ULM_HEADER),
	AST_APP_OPTION('n', OPT_NULL),
	AST_APP_OPTION('x', OPT_TLS),
	AST_APP_OPTION('f', OPT_FLIP_MODE),
	AST_APP_OPTION('c', OPT_V42),
	AST_APP_OPTION('b', OPT_V42BIS),
END_OPTIONS );

#define MAX_SAMPLES 240
/* Compressed TX data waiting for LAP-M, which takes N401 (128) bytes at a time */
#define V42BIS_TXQUEUE_SIZE (V42BIS_MAX_OUTPUT_LENGTH * 8)

enum {
	VERSION_V21,
	VERSION_V23,
	VERSION_BELL103,
	VERSION_BELL202,
	VERSION_V22,
	VERSION_V22BIS,
	VERSION_V18_45, /* V18_MODE_5BIT_45 */
	VERSION_V18_50, /* V18_MODE_5BIT_50 */
};

/*! \brief V.42 state of a session with option 'c'.
 * OFF: waiting for carrier; async framing owns the bit stream.
 * NEGOTIATING, ACTIVE, RELEASED (link disconnected after ACTIVE): V.42 owns it.
 * FALLBACK: the caller does not do V.42; async framing owns it. */
enum {
	V42_MODE_OFF = 0,
	V42_MODE_NEGOTIATING,
	V42_MODE_ACTIVE,
	V42_MODE_FALLBACK,
	V42_MODE_RELEASED,
};

/* Idle marks before the first character after falling back to async mode */
#define RAW_IDLE_MS		500

typedef struct {
	struct ast_channel *chan;
	const char *host;	//telnetd host+port
	int port;
	float txpower;
	float rxcutoff;
	int version;
	int lsb;
	int databits;
	int stopbits;
	int ulmheader;
	int sendnull;
	volatile int finished;
	int	paritytype;
	unsigned int flipmode:1;
	unsigned int track_session:1;
	unsigned int use_v42:1;
	int v42mode;
	/* Mark bits still to send before the first async data in FALLBACK mode */
	int raw_idle_bits;
	v42_state_t v42;
	unsigned int use_v42bis:1;
	/* Set once V.42bis is known not to run, so it is not checked again */
	unsigned int v42bis_decided:1;
	/* Allocated by spandsp (v42bis_init(NULL, ...)), unlike v42 above */
	v42bis_state_t *v42bis;
	uint8_t v42bis_txqueue[V42BIS_TXQUEUE_SIZE];
	int v42bis_txqueue_len;
	/* For the trained bit rate */
	v22bis_state_t *v22_modem;
} modem_session;

#define MODEM_BITBUFFER_SIZE 16
typedef struct {
	int answertone;		/* terminal is active (=sends data) */
	int nulsent;		/* we sent a NULL as very first character (at least the DBT03 expects this) */
} connection_state;

typedef struct {
	int sock;
#ifdef HAVE_OPENSSL
	SSL *ssl;
#endif
	int bitbuffer[MODEM_BITBUFFER_SIZE];
	int writepos;
	int readpos;
	int fill;
	connection_state *state;
	modem_session *session;
} modem_data;

/* How long a write to the server may wait for room in the socket buffer */
#define SOCKET_WRITE_MS		2000

/*! \brief Writes all of buf to the server. The socket is non-blocking, so a
 * full send buffer is waited out for up to SOCKET_WRITE_MS. Data from the
 * modem must not be dropped (with V.42 it is already acknowledged), so if it
 * cannot be written the session ends. */
static void socket_write_all(modem_data *d, const void *buf, int len)
{
	const char *p = buf;
	int rc;

	while (len > 0) {
#ifdef HAVE_OPENSSL
		if (d->ssl) {
			rc = SSL_write(d->ssl, p, len);
			if (rc <= 0) {
				break;
			}
		} else
#endif
		{
			rc = send(d->sock, p, len, MSG_NOSIGNAL);
			if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
				struct pollfd pfd = { .fd = d->sock, .events = POLLOUT };

				if (poll(&pfd, 1, SOCKET_WRITE_MS) > 0) {
					continue;
				}
				break;
			}
			if (rc <= 0) {
				break;
			}
		}
		p += rc;
		len -= rc;
	}
	if (len > 0) {
		ast_log(LOG_WARNING, "Could not write %d bytes to the server on %s, ending session\n",
			len, ast_channel_name(d->session->chan));
		d->session->finished = 1;
	}
}

/*! \brief This is called by spandsp whenever it filters a new bit from the line */
static void modem_put_bit(void *user_data, int bit)
{
	int stop, stop2, i;
	modem_data *rx = (modem_data*) user_data;

	int databits = rx->session->databits;
	int stopbits = rx->session->stopbits;
	int paritybits = 0;

	if (rx->session->paritytype) {
		paritybits = 1;
	}

	/* modem recognized us and starts responding through sending its pilot signal */
	if (rx->state->answertone <= 0) {
		if (bit == SIG_STATUS_CARRIER_UP) {
			rx->state->answertone = 0;
		} else if (bit == 1 && rx->state->answertone == 0) {
			rx->state->answertone = 1;
		}
	} else if (bit != 1 && bit != 0) {
		/* ignore other spandsp-stuff */
		ast_debug(1, "Bit is %d? Ignoring!\n", bit);
	} else {
		/* insert bit into our bitbuffer */
		rx->bitbuffer[rx->writepos] = bit;
		rx->writepos++;
		if (rx->writepos >= MODEM_BITBUFFER_SIZE) {
			rx->writepos = 0;
		}
		if (rx->fill < MODEM_BITBUFFER_SIZE) {
			rx->fill++;
		} else {
			/* our bitbuffer is full, this probably won't happen */
			ast_debug(3, "full buffer!\n");
			rx->readpos++;
			if (rx->readpos >= MODEM_BITBUFFER_SIZE) {
				rx->readpos = 0;
			}
		}

		/* full byte = 1 startbit + databits + paritybits + stopbits */
		while (rx->fill >= (1 + databits + paritybits + stopbits)) {
			if (rx->bitbuffer[rx->readpos] == 0) { /* check for startbit */
				stop = (rx->readpos + 1 + paritybits + databits) % MODEM_BITBUFFER_SIZE;
				stop2 = (rx->readpos + 2 + paritybits + databits) % MODEM_BITBUFFER_SIZE;
				if ((rx->bitbuffer[stop] == 1) && (stopbits == 1 || (stopbits == 2 && rx->bitbuffer[stop2] == 1))) { /* check for stopbit -> valid framing */
					char byte = 0;
					for (i = 0; i < databits; i++) {	/* generate byte */
						if (rx->session->lsb) { /* LSB first */
							if (rx->bitbuffer[(rx->readpos + 1 + i) % MODEM_BITBUFFER_SIZE]) {
								byte |= (1 << i);
							}
						} else { /* MSB first */
							if (rx->bitbuffer[(rx->readpos + databits - i) % MODEM_BITBUFFER_SIZE]) {
								byte |= (1 << i);
							}
						}
					}

					if (!paritybits || (paritybits && (rx->bitbuffer[(rx->readpos + databits + 1) % MODEM_BITBUFFER_SIZE] == ((rx->session->paritytype == 2) ^ __builtin_parity(byte))))) {
						ast_debug(7, "send(%d), [%d] %c\n", rx->sock, byte, isprint(byte) ? byte : ' ');
						socket_write_all(rx, &byte, 1);
					} /* else invalid parity, ignore byte */
					rx->readpos= (rx->readpos + 10) % MODEM_BITBUFFER_SIZE; /* XXX Why does this increment by 10? */
					rx->fill -= 10;
				} else { /* no valid framing (no stopbit), remove first bit and maybe try again */
					rx->fill--;
					rx->readpos++;
					rx->readpos %= MODEM_BITBUFFER_SIZE;
				}
			} else { /* no valid framing either (no startbit) */
				rx->fill--;
				rx->readpos++;
				rx->readpos %= MODEM_BITBUFFER_SIZE;
			}
		}
	}
}

/*! \brief V.42bis encode handler: queues compressed output until
 * v42_iframe_get() hands it to LAP-M. user_data is the modem_session. */
static void v42bis_encode_put(void *user_data, const uint8_t *buf, int len)
{
	modem_session *s = (modem_session*) user_data;

	if (s->v42bis_txqueue_len + len > (int) sizeof(s->v42bis_txqueue)) {
		/* v42_iframe_get() bounds its reads so this cannot happen. Dropping
		 * data would corrupt the compressed stream: end the session. */
		ast_log(LOG_WARNING, "V.42bis TX queue overflow on %s, ending session\n",
			ast_channel_name(s->chan));
		s->finished = 1;
		return;
	}
	memcpy(s->v42bis_txqueue + s->v42bis_txqueue_len, buf, len);
	s->v42bis_txqueue_len += len;
}

/*! \brief V.42bis decode handler: writes decompressed data to the socket.
 * user_data is a modem_data (rx and tx share the socket). */
static void v42bis_decode_put(void *user_data, const uint8_t *buf, int len)
{
	socket_write_all((modem_data*) user_data, buf, len);
}

/*! \brief Starts V.42bis once V.42 is ACTIVE, if the XID exchange agreed on
 * it. Called from both I-frame callbacks; a no-op once decided. Compression
 * must cover every byte from the first I-frame on: the peer's decoder is
 * live from link setup, so any delay desynchronises the dictionaries. */
static void v42bis_maybe_start(modem_session *s, void *v42bis_user_data)
{
	int comp, dict_size, max_string;

	if (!s->use_v42bis || s->v42bis || s->v42bis_decided || s->v42mode != V42_MODE_ACTIVE) {
		return;
	}
	if (!v42_get_negotiated_v42bis(&s->v42, &comp, &dict_size, &max_string)) {
		return;
	}
	if (comp == V42BIS_P0_NEITHER_DIRECTION) {
		ast_verb(2, "V.42bis not offered by peer on %s, running V.42 without compression\n",
			ast_channel_name(s->chan));
		s->v42bis_decided = 1;
		return;
	}
	/* P0 bit 1 is the XID initiator -> responder direction, bit 2 the other
	 * one; v42bis_init() reads bit 1 as "decompress", bit 2 as "compress",
	 * which is right for the responder. The calling party (flipmode) sends
	 * the XID command, so swap the bits there. */
	if (s->flipmode) {
		comp = ((comp & 1) << 1) | ((comp & 2) >> 1);
	}
	s->v42bis = v42bis_init(NULL, comp, dict_size, max_string,
		v42bis_encode_put, s, sizeof(s->v42bis_txqueue),
		v42bis_decode_put, v42bis_user_data, V42BIS_MAX_OUTPUT_LENGTH);
	if (!s->v42bis) {
		/* The peer will compress anyway, so plain V.42 is no fallback */
		ast_log(LOG_WARNING, "V.42bis negotiated but v42bis_init failed on %s, ending session\n",
			ast_channel_name(s->chan));
		s->v42bis_decided = 1;
		s->finished = 1;
		return;
	}
	ast_verb(2, "V.42bis compression active on %s (comp %d, dict %d, max string %d)\n",
		ast_channel_name(s->chan), comp, dict_size, max_string);
}

/*! \brief V.42 iframe_get: called from v42_tx_bit() when LAP-M has room for
 * data. Must not block (the socket is O_NONBLOCK). Returns the number of
 * bytes, or 0 for none right now. When the server closed the connection or
 * the socket failed, s->finished is set and softmodem_drain() sends what is
 * still queued. */
static int v42_iframe_get(void *user_data, uint8_t *buf, int max_len)
{
	modem_data *tx = (modem_data*) user_data;
	uint8_t rxbuf[512];
	int rc, n;
	int got_any;
	int budget, want;

	v42bis_maybe_start(tx->session, tx);
	/* Ending without a codec: nothing more to read from the server */
	if (tx->session->finished && !tx->session->v42bis) {
		return 0;
	}

	if (tx->session->v42bis) {
		if (tx->session->v42bis_txqueue_len == 0 && !tx->session->finished) {
			/* Read everything available, then flush once: a flush per
			 * recv() adds a FLUSH codeword each time, and
			 * v42bis_compress_flush() silently keeps data back when called
			 * in the middle of a match. */
			got_any = 0;
			/* Never read more than the queue can take in the worst case:
			 * 2 output bytes per input byte (transparent mode, every byte
			 * escaped), plus what spandsp holds in its own output buffer.
			 * A dropped chunk would corrupt the LZW stream. */
			budget = ((int) sizeof(tx->session->v42bis_txqueue) - tx->session->v42bis_txqueue_len
				- V42BIS_MAX_OUTPUT_LENGTH - 16) / 2;
			while (budget > 0) {
				want = (budget < (int) sizeof(rxbuf)) ? budget : (int) sizeof(rxbuf);
#ifdef HAVE_OPENSSL
				if (tx->ssl) {
					rc = SSL_read(tx->ssl, rxbuf, want);
				} else
#endif
				{
					rc = recv(tx->sock, rxbuf, want, 0);
				}
				if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
					break;
				}
				if (rc <= 0) {
					tx->session->finished = 1;
					break;
				}
				v42bis_compress(tx->session->v42bis, rxbuf, rc);
				got_any = 1;
				budget -= rc;
			}
			if (got_any) {
				v42bis_compress_flush(tx->session->v42bis);
			}
		}
		n = tx->session->v42bis_txqueue_len;
		if (n > max_len) {
			n = max_len;
		}
		if (n > 0) {
			memcpy(buf, tx->session->v42bis_txqueue, n);
			tx->session->v42bis_txqueue_len -= n;
			memmove(tx->session->v42bis_txqueue, tx->session->v42bis_txqueue + n, tx->session->v42bis_txqueue_len);
		}
		return n;
	}

#ifdef HAVE_OPENSSL
	if (tx->ssl) {
		rc = SSL_read(tx->ssl, buf, max_len);
	} else
#endif
	{
		rc = recv(tx->sock, buf, max_len, 0);
	}
	if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		return 0;
	}
	if (rc <= 0) {
		tx->session->finished = 1;
		return 0;
	}
	return rc;
}

/*! \brief V.42 iframe_put: called from v42_rx_bit() with the data of an
 * in-order, error-free I-frame; decompressed first if V.42bis runs. */
static void v42_iframe_put(void *user_data, const uint8_t *frame, int len)
{
	modem_data *rx = (modem_data*) user_data;

	v42bis_maybe_start(rx->session, rx);
	/* The session is ending (server gone, or no codec for compressed data) */
	if (rx->session->finished) {
		return;
	}

	if (rx->session->v42bis) {
		int rc = v42bis_decompress(rx->session->v42bis, frame, len);
		if (rc != 0) {
			/* Invalid codeword: the dictionaries are out of sync and
			 * everything after it would be garbage */
			ast_log(LOG_WARNING, "v42bis_decompress returned %d on %s, ending session\n", rc, ast_channel_name(rx->session->chan));
			rx->session->finished = 1;
			return;
		}
		/* Without a flush, a single keystroke stays in spandsp's output
		 * buffer and never reaches the server */
		v42bis_decompress_flush(rx->session->v42bis);
		return;
	}
	socket_write_all(rx, frame, len);
}

/*! \brief Forwards spandsp's span_log() output (V.22bis training, V.42 frame
 * trace) to the Asterisk debug log: core set debug 1 app_softmodem.so */
static void spandsp_log_handler(int level, const char *text)
{
	ast_debug(1, "spandsp: %s", text);
}

/*! \brief V.42 status callback: switches between V.42 and async framing */
static void v42_status_handler(void *user_data, int status)
{
	modem_session *s = (modem_session*) user_data;
	const char *str = lapm_status_to_str(status);

	/* lapm_status_to_str() only knows the LAPM_* states, not the generic
	 * SIG_STATUS_* codes this callback also gets, so log the number too */
	ast_verb(3, "V.42 status on %s: %s (%d)\n", ast_channel_name(s->chan), str, status);
	if (!strcmp(str, "LAPM_V42_UNSUPPORTED")) {
		ast_verb(2, "V.42 not supported by peer on %s, falling back to raw mode\n", ast_channel_name(s->chan));
		s->v42mode = V42_MODE_FALLBACK;
		/* Data queued during V.42 detection (the server's banner) would
		 * otherwise start back to back right away, and a modem receiver can
		 * lock onto a wrong start bit until the first pause */
		s->raw_idle_bits = (s->v22_modem ? v22bis_get_current_bit_rate(s->v22_modem) : 1200) * RAW_IDLE_MS / 1000;
	} else if (status == SIG_STATUS_LINK_CONNECTED) {
		ast_verb(2, "V.42 error correction active on %s\n", ast_channel_name(s->chan));
		/* Training has finished by now, so the timers get the final rate
		 * (see v42_or_raw_put_bit()) */
		if (s->v22_modem) {
			s->v42.tx_bit_rate = v22bis_get_current_bit_rate(s->v22_modem);
			ast_verb(3, "V.42 timers set for %d bit/s on %s\n", s->v42.tx_bit_rate, ast_channel_name(s->chan));
		}
		s->v42mode = V42_MODE_ACTIVE;
	} else if (status == SIG_STATUS_LINK_DISCONNECTED && s->v42mode == V42_MODE_ACTIVE) {
		/* Released by our DISC (softmodem_drain()) or by the modem's: either
		 * way the session ends */
		ast_verb(2, "V.42 link released on %s\n", ast_channel_name(s->chan));
		s->v42mode = V42_MODE_RELEASED;
		s->finished = 1;
	}
}

/*! \brief Receive bit callback with option 'c': passes each bit to V.42 or to
 * the async framing, depending on v42mode */
static void v42_or_raw_put_bit(void *user_data, int bit)
{
	modem_data *rx = (modem_data*) user_data;
	modem_session *s = rx->session;

	if (bit == SIG_STATUS_CARRIER_UP) {
		rx->state->answertone = 0;
		if (s->v42mode == V42_MODE_OFF) {
			/* v42.c counts its timers in bits; v42_init() assumes 28800
			 * bit/s, which made every timer 12 times too long at 2400.
			 * Set the current rate before v42_restart() starts T400.
			 * (v42_start() is declared but not exported by libspandsp.) */
			if (s->v22_modem) {
				s->v42.tx_bit_rate = v22bis_get_current_bit_rate(s->v22_modem);
				ast_verb(3, "V.42 timers set for %d bit/s on %s\n", s->v42.tx_bit_rate, ast_channel_name(s->chan));
			}
			v42_restart(&s->v42);
			s->v42mode = V42_MODE_NEGOTIATING;
		}
		return;
	}
	if (bit != 0 && bit != 1) {
		return;
	}

	if (s->v42mode == V42_MODE_FALLBACK || s->v42mode == V42_MODE_OFF) {
		modem_put_bit(user_data, bit);
		return;
	}
	v42_rx_bit(&s->v42, bit);
}

static void tdd_put_msg(void *user_data, const unsigned char *msg, int len)
{
	modem_data *rx = (modem_data*) user_data;
	/* In practice this function is called for every single byte, so len should be 1 */
	if (len != 1) {
		ast_log(LOG_WARNING, "Expected len 1 but have %d?\n", len);
		return;
	}
	ast_debug(1, "put msg: %d (%c)\n", *msg, isprint(*msg) ? *msg : ' ');
	/* We don't need modem_put_bit here, the other TDD functions already encapsulate this functionality, we can write to the socket directly */
#ifdef HAVE_OPENSSL
	if (rx->ssl) {
		SSL_write(rx->ssl, msg, len);
	} else
#endif
	{
		send(rx->sock, msg, len, 0);
	}
}

/*! \brief Same as v18_tdd_put_async_byte in spandsp's v18.c, except flush every byte immediately, not after 256.
 * This way we force any TDD input to be sent immediately onto the socket as soon as we get it, not buffered or when carrier pauses. */
static void my_v18_tdd_put_async_byte(void *user_data, int byte)
{
	v18_state_t *s;
	uint8_t octet;
  
	s = (v18_state_t *) user_data;
	if (byte < 0) {
		/* Special conditions */
		span_log(&s->logging, SPAN_LOG_FLOW, "V.18 signal status is %s (%d)\n", signal_status_to_str(byte), byte);
		switch (byte) {
		case SIG_STATUS_CARRIER_UP:
			s->consecutive_ones = 0;
			s->bit_pos = 0;
			s->in_progress = 0;
			s->rx_msg_len = 0;
			break;
		case SIG_STATUS_CARRIER_DOWN:
			if (s->rx_msg_len > 0) {
				/* Whatever we have to date constitutes the message */
				s->rx_msg[s->rx_msg_len] = '\0';
				s->put_msg(s->user_data, s->rx_msg, s->rx_msg_len);
				s->rx_msg_len = 0;
			}
			break;
		default:
			span_log(&s->logging, SPAN_LOG_WARNING, "Unexpected special put byte value - %d!\n", byte);
			break;
		}
		return;
	}
	if ((octet = v18_decode_baudot(s, (uint8_t) (byte & 0x1F))))
		s->rx_msg[s->rx_msg_len++] = octet;
#if 0
	if (s->rx_msg_len >= 256)
#else
	if (s->rx_msg_len >= 1) /* Send data as soon as we have any */
#endif
	{
		ast_assert(s->put_msg != NULL);
		s->rx_msg[s->rx_msg_len] = '\0';
		s->put_msg(s->user_data, s->rx_msg, s->rx_msg_len);
		s->rx_msg_len = 0;
	}
}

/*! \brief spandsp asks us for a bit to send onto the line */
static int modem_get_bit(void *user_data)
{
	modem_data *tx = (modem_data*) user_data;
	char byte = 0;
	int i, rc;

	int databits = tx->session->databits;
	int stopbits = tx->session->stopbits;
	int paritybits = 0;

	if (tx->session->paritytype) {
		paritybits = 1;
	}

	/* no new data in send (bit)buffer,
	 * either we just picked up the line, the terminal started to respond,
	 * than we check for new data on the socket
	 * or there's no new data, so we send 1s (mark) */
	if (tx->writepos == tx->readpos) {
		if (tx->state->nulsent > 0) {	/* connection is established, look for data on socket */
#ifdef HAVE_OPENSSL
			if (tx->ssl) {
				rc = SSL_read(tx->ssl, &byte, 1);
			} else
#endif
			{
				rc = recv(tx->sock, &byte, 1, 0);
			}
			if (rc > 0) {
				/* new data on socket, we put that byte into our bitbuffer */
				for (i = 0; i < (databits + paritybits + stopbits); i++) {
					if (paritybits && (i == databits) ) {
						tx->bitbuffer[tx->writepos] = (tx->session->paritytype == 2) ^ __builtin_parity( byte);
					} else if (i >= databits) {
						tx->bitbuffer[tx->writepos] = 1;	/* stopbits */
					} else { /* databits */
						if (tx->session->lsb) {
							if (byte & (1 << i)) {
								tx->bitbuffer[tx->writepos] = 1;
							} else {
								tx->bitbuffer[tx->writepos] = 0;
							}
						} else {
							if (byte & (1 << (databits - 1 - i))) {
								tx->bitbuffer[tx->writepos] = 1;
							} else {
								tx->bitbuffer[tx->writepos] = 0;
							}
						}
					}
					tx->writepos++;
					if (tx->writepos >= MODEM_BITBUFFER_SIZE) {
						tx->writepos = 0;
					}
				}
				return 0; /* return startbit immediately */
			} else if (rc == 0) {
				if (!tx->session->finished) {
					ast_verb(4, "TCP server closed connection, hanging up...\n");
				}
				tx->session->finished = 1;
			}
		} else {
			int res;
			/* check if socket was closed before connection was terminated */
#ifdef HAVE_OPENSSL
			if (tx->ssl) {
				res = SSL_peek(tx->ssl, &byte, 1);
			} else
#endif
			{
				res = recv(tx->sock, &byte, 1, MSG_PEEK);
			}
			if (res == 0) {
				tx->session->finished = 1;
				return 1;
			}
			if (tx->state->answertone > 0) {
				if (tx->session->sendnull) { /* send null byte */
					ast_debug(3, "Got TE's tone, will send null-byte\n");
					for (i = 0; i < (databits + paritybits + stopbits); i++) {
						if (paritybits && i == databits) {
							tx->bitbuffer[tx->writepos] = (tx->session->paritytype == 2) ^ __builtin_parity( 0 ); /* yes, I know! */
						} else if (i >= databits) {
							tx->bitbuffer[tx->writepos] = 1; /* stopbits */
						} else {
							tx->bitbuffer[tx->writepos] = 0; /* databits */
						}
						tx->writepos++;
						if (tx->writepos >= MODEM_BITBUFFER_SIZE) {
							tx->writepos = 0;
						}
					}
				}
				tx->state->nulsent = 1;

				if (tx->session->ulmheader) {
					char header[60];
					int headerlength;
					float tx_baud, rx_baud;
					/* send ULM relay protocol header, include connection speed */
					if (tx->session->version == VERSION_V21) {
						tx_baud = 300;
						rx_baud = 300;
					} else if (tx->session->version == VERSION_V23) {
						tx_baud = 1200;
						rx_baud = 75;
					} else if (tx->session->version == VERSION_BELL103) {
						tx_baud = 300;
						rx_baud = 300;
					} else if (tx->session->version == VERSION_BELL202) {
						tx_baud = 1200;
						rx_baud = 1200;
					} else if (tx->session->version == VERSION_V22) {
						tx_baud = 1200;
						rx_baud = 1200;
					} else if (tx->session->version == VERSION_V22BIS) {
						tx_baud = 2400;
						rx_baud = 2400;
					} else {
						tx_baud = 0;
						rx_baud = 0;
					}

					headerlength = sprintf(header, "Version: 1\r\nTXspeed: %.2f\r\nRXspeed: %.2f\r\n\r\n", tx_baud /(1 + databits + stopbits), rx_baud / (1 + databits + stopbits));
#ifdef HAVE_OPENSSL
					if (tx->ssl) {
						SSL_write(tx->ssl, header, headerlength);
					} else
#endif
					{
						send(tx->sock, header, headerlength, 0);
					}
				}

				if (tx->session->sendnull) {
					return 0;
				} else {
					return 1;
				}
			}
		}

		/* no new data on socket, NULL-byte already sent, send mark-frequency */
		return 1;
	} else {
		/* there still is data in the bitbuffer, so we just send that out */
		i = tx->bitbuffer[tx->readpos];
		tx->readpos++;
		if (tx->readpos >= MODEM_BITBUFFER_SIZE) {
			tx->readpos = 0;
		}
		return i;
	}
}

/*! \brief Transmit bit callback with option 'c', the counterpart of
 * v42_or_raw_put_bit() */
static int v42_or_raw_get_bit(void *user_data)
{
	modem_data *tx = (modem_data*) user_data;
	modem_session *s = tx->session;

	if (s->v42mode == V42_MODE_FALLBACK || s->v42mode == V42_MODE_OFF) {
		/* Idle marks before the first async data, see v42_status_handler() */
		if (s->raw_idle_bits > 0 && tx->writepos == tx->readpos) {
			s->raw_idle_bits--;
			return 1;
		}
		return modem_get_bit(user_data);
	}
	return v42_tx_bit(&s->v42);
}

static void *modem_generator_alloc(struct ast_channel *chan, void *params)
{
	return params;
}

static int fsk_generator_generate(struct ast_channel *chan, void *data, int len, int samples)
{
	fsk_tx_state_t *tx = (fsk_tx_state_t*) data;
	uint8_t buffer[AST_FRIENDLY_OFFSET + MAX_SAMPLES * sizeof(uint16_t)];
	int16_t *buf = (int16_t *) (buffer + AST_FRIENDLY_OFFSET);

	struct ast_frame outf = {
		.frametype = AST_FRAME_VOICE,
		.subclass.format = ast_format_slin,
		.src = __FUNCTION__,
	};

	if (samples > MAX_SAMPLES) {
		ast_log(LOG_WARNING, "Only generating %d samples, where %d requested\n", MAX_SAMPLES, samples);
		samples = MAX_SAMPLES;
	}

	if ((len = fsk_tx(tx, buf, samples)) > 0) {
		outf.samples = len;
		AST_FRAME_SET_BUFFER(&outf, buffer, AST_FRIENDLY_OFFSET, len * sizeof(int16_t));

		if (ast_write(chan, &outf) < 0) {
			ast_log(LOG_WARNING, "Failed to write frame to %s: %s\n", ast_channel_name(chan), strerror(errno));
			return -1;
		}
	}

	return 0;
}

static int v22_generator_generate(struct ast_channel *chan, void *data, int len, int samples)
{
	v22bis_state_t *tx = (v22bis_state_t*) data;
	uint8_t buffer[AST_FRIENDLY_OFFSET + MAX_SAMPLES * sizeof(uint16_t)];
	int16_t *buf = (int16_t *) (buffer + AST_FRIENDLY_OFFSET);

	struct ast_frame outf = {
		.frametype = AST_FRAME_VOICE,
		.subclass.format = ast_format_slin,
		.src = __FUNCTION__,
	};

	if (samples > MAX_SAMPLES) {
		ast_log(LOG_WARNING, "Only generating %d samples, where %d requested\n", MAX_SAMPLES, samples);
		samples = MAX_SAMPLES;
	}

	if ((len = v22bis_tx(tx, buf, samples)) > 0) {
		outf.samples = len;
		AST_FRAME_SET_BUFFER(&outf, buffer, AST_FRIENDLY_OFFSET, len * sizeof(int16_t));

		if (ast_write(chan, &outf) < 0) {
			ast_log(LOG_WARNING, "Failed to write frame to %s: %s\n", ast_channel_name(chan), strerror(errno));
			return -1;
		}
	}

	return 0;
}

static int v18_generator_generate(struct ast_channel *chan, void *data, int len, int samples)
{
	v18_state_t  *tx = (v18_state_t *) data;
	uint8_t buffer[AST_FRIENDLY_OFFSET + MAX_SAMPLES * sizeof(uint16_t)];
	int16_t *buf = (int16_t *) (buffer + AST_FRIENDLY_OFFSET);

	struct ast_frame outf = {
		.frametype = AST_FRAME_VOICE,
		.subclass.format = ast_format_slin,
		.src = __FUNCTION__,
	};

	if (samples > MAX_SAMPLES) {
		ast_log(LOG_WARNING, "Only generating %d samples, where %d requested\n", MAX_SAMPLES, samples);
		samples = MAX_SAMPLES;
	}

	len = v18_tx(tx, buf, samples);
	if (len > 0) {
		outf.samples = len;
		AST_FRAME_SET_BUFFER(&outf, buffer, AST_FRIENDLY_OFFSET, len * sizeof(int16_t));

		if (ast_write(chan, &outf) < 0) {
			ast_log(LOG_WARNING, "Failed to write frame to %s: %s\n", ast_channel_name(chan), strerror(errno));
			return -1;
		}
	}

	return 0;
}

struct ast_generator fsk_generator = {
	alloc:		modem_generator_alloc,
	generate: 	fsk_generator_generate,
};

struct ast_generator v22_generator = {
	alloc:		modem_generator_alloc,
	generate: 	v22_generator_generate,
};

struct ast_generator v18_generator = {
	alloc:		modem_generator_alloc,
	generate: 	v18_generator_generate,
};

struct softmodem_session {
	struct ast_channel *chan; /* The channel itself cannot go away while this application is using it, even with a masquerade, so storing the channel pointer is safe */
	int port;
	time_t started;
	AST_RWLIST_ENTRY(softmodem_session) entry;
};

static AST_RWLIST_HEAD_STATIC(sessions, softmodem_session);

static void untrack_session(modem_session *s)
{
	struct softmodem_session *ss;
	int port = 0;

	AST_RWLIST_WRLOCK(&sessions);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&sessions, ss, entry) {
		if (ss->chan == s->chan) {
			AST_RWLIST_REMOVE_CURRENT(entry);
			port = ss->port;
			ast_free(ss);
			break;
		}
	}
	AST_RWLIST_TRAVERSE_SAFE_END;
	AST_RWLIST_UNLOCK(&sessions);
	if (!ss) {
		ast_log(LOG_WARNING, "Couldn't find tracked softmodem session in session list?\n");
	} else {
		ast_debug(3, "Removed session %d from track list\n", port);
	}
}

static void track_session(modem_session *s, int lport)
{
	struct softmodem_session *ss;

	ss = ast_calloc(1, sizeof(*ss));
	if (!ss) {
		return;
	}
	ss->chan = s->chan;
	ss->port = lport;
	ss->started = time(NULL);
	AST_RWLIST_WRLOCK(&sessions);
	AST_RWLIST_INSERT_HEAD(&sessions, ss, entry);
	AST_RWLIST_UNLOCK(&sessions);
	ast_debug(3, "Added session %d to track list\n", lport);
}

static int manager_softmodem_sessions(struct mansession *s, const struct message *m)
{
	const char *actionid = astman_get_header(m, "ActionID");
	const char *portstr = astman_get_header(m, "Port");
	char idText[256];
	struct softmodem_session *ss;
	int sessions_found = 0;
	unsigned int port = 0;
	time_t now = time(NULL);

	if (!ast_strlen_zero(portstr)) {
		port = atoi(portstr);
	}

	if (!ast_strlen_zero(actionid)) {
		snprintf(idText, sizeof(idText), "ActionID: %s\r\n", actionid);
	} else {
		idText[0] = '\0';
	}
	astman_send_listack(s, m, "Sessions will follow", "start");

	AST_RWLIST_RDLOCK(&sessions);
	AST_RWLIST_TRAVERSE(&sessions, ss, entry) {
		time_t elapsed;
		if (port && ss->port != port) {
			continue; /* Doesn't match filter */
		}
		elapsed = (long int) (now - ss->started);
		sessions_found++;
		astman_append(s,
			"Event: SoftmodemSession\r\n"
			"%s"
			"Channel: %s\r\n"
			"CallerIDNumber: %s\r\n"
			"CallerIDName: %s\r\n"
			"Port: %d\r\n"
			"Duration: %ld\r\n"
			"\r\n",
			idText,
			ast_channel_name(ss->chan),
			S_OR(ast_channel_caller(ss->chan)->id.number.str, "<unknown>"),
			S_OR(ast_channel_caller(ss->chan)->id.name.str, "<unknown>"),
			ss->port, elapsed);
	}
	AST_RWLIST_UNLOCK(&sessions);

	astman_send_list_complete_start(s, m, "SoftmodemSessionsComplete", sessions_found);
	astman_append(s, "Total: %d\r\n", sessions_found);
	astman_send_list_complete_end(s);
	return 0;
}

/* softmodem_drain() limits: time without progress while sending what is
 * still queued, time to wait for the answer to DISC, idle carrier after the
 * last data (softmodems lag behind the line; 0.5 s lost the last line on
 * one), and the silence before hanging up. */
#define DRAIN_STALL_MS		5000
#define DRAIN_DISC_MS		3000
#define DRAIN_TAIL_MS		1500
#define DRAIN_SILENCE_MS	2000

/*! \brief Ends a V.22/V.22bis call cleanly after the session finished: with
 * V.42, send what is still queued (as long as the modem keeps acknowledging),
 * then DISC, and wait for UA/DM; then, in every mode, keep an idle carrier for
 * DRAIN_TAIL_MS and send silence before the caller hangs up the channel.
 * Hanging up straight away lost the last data and left some modems online,
 * printing line noise.
 * Returns -1 if the channel hung up meanwhile, 0 otherwise. */
static int softmodem_drain(modem_session *s, v22bis_state_t *v22_modem)
{
	struct timeval start = ast_tvnow();
	struct ast_frame *inf;
	struct ast_silence_generator *silgen;
	int disc_sent = 0;
	int tail_started = 0;
	int last_va = -1;
	int last_queue = -1;
	int elapsed, res;

	for (;;) {
		elapsed = ast_tvdiff_ms(ast_tvnow(), start);
		if (s->v42mode == V42_MODE_ACTIVE && !disc_sent) {
			if (s->v42.lapm.va != last_va || s->v42bis_txqueue_len != last_queue) {
				/* Still progressing: restart the stall timeout */
				last_va = s->v42.lapm.va;
				last_queue = s->v42bis_txqueue_len;
				start = ast_tvnow();
				elapsed = 0;
			}
			if (elapsed >= DRAIN_STALL_MS || (s->v42bis_txqueue_len == 0
				&& s->v42.lapm.info_acked == s->v42.lapm.info_put)) {
				if (elapsed >= DRAIN_STALL_MS) {
					ast_log(LOG_WARNING, "V.42 data not acknowledged for %d ms on %s, dropping the rest\n",
						DRAIN_STALL_MS, ast_channel_name(s->chan));
				}
				ast_verb(3, "Sending V.42 DISC on %s\n", ast_channel_name(s->chan));
				v42_stop(&s->v42);
				disc_sent = 1;
				start = ast_tvnow();
			}
		} else if (s->v42mode == V42_MODE_ACTIVE && elapsed < DRAIN_DISC_MS) {
			/* Waiting for UA/DM */
		} else if (!tail_started) {
			if (s->v42mode == V42_MODE_ACTIVE) {
				ast_verb(3, "No answer to V.42 DISC on %s\n", ast_channel_name(s->chan));
			}
			tail_started = 1;
			start = ast_tvnow();
		} else if (elapsed >= DRAIN_TAIL_MS) {
			break;
		}
		if (ast_waitfor(s->chan, 20) < 0) {
			return -1;
		}
		inf = ast_read(s->chan);
		if (!inf) {
			return -1;
		}
		if (inf->frametype == AST_FRAME_VOICE && inf->subclass.format == ast_format_slin) {
			v22bis_rx(v22_modem, inf->data.ptr, inf->samples);
		}
		ast_frfree(inf);
	}

	ast_deactivate_generator(s->chan);
	silgen = ast_channel_start_silence_generator(s->chan);
	res = ast_safe_sleep(s->chan, DRAIN_SILENCE_MS);
	ast_channel_stop_silence_generator(s->chan, silgen);
	return res ? -1 : 0;
}

/*!
 * \internal
 * \retval -1 on error or hangup after connection
 * \retval 1 if connection failed
 */
static int softmodem_communicate(modem_session *s, int tls)
{
	int res = -1;
	struct ast_format *original_read_fmt;
	struct ast_format *original_write_fmt;
#ifdef HAVE_OPENSSL
	SSL *ssl = NULL;
	SSL_CTX *ctx = NULL;
#endif

	modem_data rxdata, txdata;

	struct ast_frame *inf = NULL;

	fsk_tx_state_t *modem_tx = NULL;
	fsk_rx_state_t *modem_rx = NULL;

	v22bis_state_t *v22_modem = NULL;
	v18_state_t  *v18_modem = NULL;

	int sock;
	struct sockaddr_in server;
	struct hostent *hp;
	struct ast_hostent ahp;
	connection_state state;

	/* Used for TDD only */
	struct pollfd pfd;
	int pres;
	int lastoutput = 0;
	char buf[256] = "  ";

	original_read_fmt = ast_channel_readformat(s->chan);
	if (original_read_fmt != ast_format_slin) {
		res = ast_set_read_format(s->chan, ast_format_slin);
		if (res < 0) {
			ast_log(LOG_WARNING, "Unable to set to linear read mode on %s\n", ast_channel_name(s->chan));
			return 1;
		}
	}

	original_write_fmt = ast_channel_writeformat(s->chan);
	if (original_write_fmt != ast_format_slin) {
		res = ast_set_write_format(s->chan, ast_format_slin);
		if (res < 0) {
			ast_log(LOG_WARNING, "Unable to set to linear write mode on %s\n", ast_channel_name(s->chan));
			return 1;
		}
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		ast_log(LOG_ERROR, "Could not create socket: %s\n", strerror(errno));
		return 1;
	}

	server.sin_family = AF_INET;
	hp = ast_gethostbyname(s->host, &ahp);
	if (!hp) {
		ast_log(LOG_ERROR, "Cannot resolve host '%s'\n", s->host);
		close(sock);
		return 1;
	}
	memcpy((char *) &server.sin_addr, hp->h_addr, hp->h_length);
	server.sin_port = htons(s->port);

	/* If the connect takes a while, we should autoservice the channel */
	ast_autoservice_start(s->chan);

	if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
		ast_log(LOG_ERROR, "Cannot connect to remote host '%s': %s\n", s->host, strerror(errno));
		close(sock);
		ast_autoservice_stop(s->chan);
		return 1;
	}

	if (s->track_session) {
		int lport;
		struct sockaddr_in sin;
		socklen_t slen = sizeof(sin);
		/* Figure out what port we're using locally for this connection */
		if (getsockname(sock, (struct sockaddr *) &sin, &slen)) {
			ast_log(LOG_WARNING, "getsockname failed: %s\n", strerror(errno));
		} else {
			lport = ntohs(sin.sin_port);
			track_session(s, lport);
		}
	}

	ast_autoservice_stop(s->chan);

	fcntl(sock, F_SETFL, O_NONBLOCK);

	state.answertone = -1; /* no carrier yet */
	state.nulsent = 0;

	rxdata.sock = sock;
	rxdata.writepos = 0;
	rxdata.readpos = 0;
	rxdata.fill = 0;
	rxdata.state = &state;
	rxdata.session = s;

#ifdef HAVE_OPENSSL
	rxdata.ssl = txdata.ssl = NULL;
#endif

	txdata.sock = sock;
	txdata.writepos = 0;
	txdata.readpos = 0;
	txdata.fill = 0;
	txdata.state = &state;
	txdata.session = s;

	if (tls) {
#ifdef HAVE_OPENSSL
		int sres;
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
		ctx = SSL_CTX_new(TLS_method());
#else
		ctx = SSL_CTX_new(TLSv1_method()); /* If the system is this old, it probably should use an old TLS version anyways */
#endif
		if (!ctx) {
			close(sock);
			return -1;
		}
		ssl = SSL_new(ctx);
		if (!ssl) {
			SSL_CTX_free(ctx);
			close(sock);
			return -1;
		}

		/* Set hostname if needed for SNI */
		SSL_set_tlsext_host_name(ssl, s->host);

		if (SSL_set_fd(ssl, sock) != 1) {
			ast_log(LOG_ERROR, "Failed to set SSL fd: %s\n", ERR_error_string(ERR_get_error(), NULL));
			SSL_CTX_free(ctx);
			close(sock);
			return -1;
		}
		/* Since fd is nonblocking, retry as needed */
		do {
			sres = SSL_connect(ssl);
		} while (sres == -1 && SSL_get_error(ssl, -1) == SSL_ERROR_WANT_READ);
		if (sres == -1) {
			ast_log(LOG_ERROR, "Failed to connect SSL: %s\n", ERR_error_string(ERR_get_error(), NULL));
			SSL_CTX_free(ctx);
			close(sock);
			return -1;
		}
		/* XXX No certificate verification is done here currently
		 * For that it would be good to reuse some of the logic in tcptls.c rather than recreating it here. */
		rxdata.ssl = txdata.ssl = ssl;
#else
		ast_log(LOG_ERROR, "Asterisk was compiled without TLS support\n");
		close(sock);
		return -1;
#endif
	}

	/* initialise spandsp-stuff, give it our callback functions */
	if (s->version == VERSION_V21) {
		if (s->flipmode) {
			modem_tx = fsk_tx_init(NULL, &preset_fsk_specs[FSK_V21CH1], modem_get_bit, &txdata);
			modem_rx = fsk_rx_init(NULL, &preset_fsk_specs[FSK_V21CH2], FSK_FRAME_MODE_SYNC, modem_put_bit, &rxdata);
		} else {
			modem_tx = fsk_tx_init(NULL, &preset_fsk_specs[FSK_V21CH2], modem_get_bit, &txdata);
			modem_rx = fsk_rx_init(NULL, &preset_fsk_specs[FSK_V21CH1], FSK_FRAME_MODE_SYNC, modem_put_bit, &rxdata);
		}
	} else if (s->version == VERSION_V23) {
		if (s->flipmode) {
			modem_tx = fsk_tx_init(NULL, &preset_fsk_specs[FSK_V23CH2], modem_get_bit, &txdata);
			modem_rx = fsk_rx_init(NULL, &preset_fsk_specs[FSK_V23CH1], FSK_FRAME_MODE_SYNC, modem_put_bit, &rxdata);
		} else {
			modem_tx = fsk_tx_init(NULL, &preset_fsk_specs[FSK_V23CH1], modem_get_bit, &txdata);
			modem_rx = fsk_rx_init(NULL, &preset_fsk_specs[FSK_V23CH2], FSK_FRAME_MODE_SYNC, modem_put_bit, &rxdata);
		}
	} else if (s->version == VERSION_BELL103) {
		if (s->flipmode) {
			modem_tx = fsk_tx_init(NULL, &preset_fsk_specs[FSK_BELL103CH2], modem_get_bit, &txdata);
			modem_rx = fsk_rx_init(NULL, &preset_fsk_specs[FSK_BELL103CH1], FSK_FRAME_MODE_SYNC, modem_put_bit, &rxdata);
		} else {
			modem_tx = fsk_tx_init(NULL, &preset_fsk_specs[FSK_BELL103CH1], modem_get_bit, &txdata);
			modem_rx = fsk_rx_init(NULL, &preset_fsk_specs[FSK_BELL103CH2], FSK_FRAME_MODE_SYNC, modem_put_bit, &rxdata);
		}
	} else if (s->version == VERSION_BELL202) {
		modem_tx = fsk_tx_init(NULL, &preset_fsk_specs[FSK_BELL202], modem_get_bit, &txdata);
		modem_rx = fsk_rx_init(NULL, &preset_fsk_specs[FSK_BELL202], FSK_FRAME_MODE_SYNC, modem_put_bit, &rxdata);
	} else if (s->version == VERSION_V22 || s->version == VERSION_V22BIS) {
		int baud = (s->version == VERSION_V22BIS) ? 2400 : 1200;
		if (s->use_v42) {
			/* One V.42 context for both directions; it takes over from the
			 * async framing at carrier up (v42_or_raw_put_bit()). flipmode
			 * set means we call, as for v22bis_init() below. */
			s->v42mode = V42_MODE_OFF;
			v42_init(&s->v42, s->flipmode, 1, v42_iframe_get, v42_iframe_put, &txdata);
			v42_set_status_callback(&s->v42, v42_status_handler, s);
			/* Frame level trace, only with debug on (spandsp formats every
			 * line it logs). A per-instance handler, because
			 * res_fax_spandsp clears spandsp's global message handler. */
			if (DEBUG_ATLEAST(1)) {
				span_log_set_level(&s->v42.logging, SPAN_LOG_FLOW | SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL);
				span_log_set_message_handler(&s->v42.logging, spandsp_log_handler);
			}
			/* What our XIDs request. Needed in both cases: v42_init() sets
			 * comp = 1, so without an explicit "neither direction" 'c' alone
			 * would still accept V.42bis that this side never runs. */
			v42_set_local_v42bis_request(&s->v42,
				s->use_v42bis ? V42BIS_P0_BOTH_DIRECTIONS : V42BIS_P0_NEITHER_DIRECTION,
				V42BIS_MAX_CODEWORDS, V42BIS_MAX_STRING_SIZE);
			v22_modem = v22bis_init(NULL, baud, 0, s->flipmode, v42_or_raw_get_bit, &txdata, v42_or_raw_put_bit, &rxdata);
			s->v22_modem = v22_modem;
		} else {
			v22_modem = v22bis_init(NULL, baud, 0, s->flipmode, modem_get_bit, &txdata, modem_put_bit, &rxdata);
		}
		/* Training trace (S1 detection, rates), only with debug on */
		if (v22_modem && DEBUG_ATLEAST(1)) {
			logging_state_t *v22log = v22bis_get_logging_state(v22_modem);
			span_log_set_level(v22log, SPAN_LOG_FLOW | SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL);
			span_log_set_message_handler(v22log, spandsp_log_handler);
		}
	} else if (s->version == VERSION_V18_45 || s->version == VERSION_V18_50) {
		fsk_rx_state_t *fs = NULL;
		/* This softmodem is the called side, so answerer mode */
		v18_modem = v18_init(NULL, 0, s->version == VERSION_V18_45 ? V18_MODE_5BIT_45 : V18_MODE_5BIT_50, tdd_put_msg, &rxdata);
		fs = &v18_modem->fskrx;
		fsk_rx_set_put_bit(fs, my_v18_tdd_put_async_byte, v18_modem); /* override v18_tdd_put_async_byte to my_v18_tdd_put_async_byte */
		pfd.fd = sock;
		pfd.events = POLLIN;
		pfd.revents = 0;
	} else {
		ast_log(LOG_ERROR,"Unsupported modem type\n");
		goto cleanup;
	}

	if (s->version == VERSION_V21 || s->version == VERSION_V23 || s->version ==  VERSION_BELL103 || s->version ==  VERSION_BELL202) {
		fsk_tx_power (modem_tx, s->txpower);
		fsk_rx_signal_cutoff(modem_rx, s->rxcutoff);
	} else if (s->version == VERSION_V22 || s->version == VERSION_V22BIS) {
		v22bis_tx_power(v22_modem, s->txpower);
		v22bis_rx_signal_cutoff(v22_modem, s->rxcutoff);
	}

	if (s->version == VERSION_V21 || s->version == VERSION_V23 || s->version == VERSION_BELL103 || s->version ==  VERSION_BELL202) {
		ast_activate_generator(s->chan, &fsk_generator, modem_tx);
	} else if (s->version == VERSION_V22 || s->version == VERSION_V22BIS) {
		ast_activate_generator(s->chan, &v22_generator, v22_modem);
	} else if (s->version == VERSION_V18_45 || s->version == VERSION_V18_50) {
		ast_activate_generator(s->chan, &v18_generator, v18_modem);
	}

	while (!s->finished) {
		res = ast_waitfor(s->chan, 20);
		if (res < 0) {
			break;
		} else if (res > 0) {
			res = 0;
		}

		inf = ast_read(s->chan);
		if (!inf) {
			ast_debug(1, "Channel hangup\n");
			res = -1;
			break;
		}

		/* Check the frame type. Format also must be checked because there is a chance
		   that a frame in old format was already queued before we set chanel format
		   to slinear so it will still be received by ast_read */
		if (inf->frametype == AST_FRAME_VOICE && inf->subclass.format == ast_format_slin) {
			if (s->version == VERSION_V21 || s->version == VERSION_V23 || s->version == VERSION_BELL103 || s->version ==  VERSION_BELL202) {
				if (fsk_rx(modem_rx, inf->data.ptr, inf->samples) < 0) {
					/* I know fsk_rx never returns errors. The check here is for good style only */
					ast_log(LOG_WARNING, "softmodem returned error\n");
					res = -1;
					break;
				}
			} else if (s->version == VERSION_V22 || s->version == VERSION_V22BIS) {
				if (v22bis_rx(v22_modem, inf->data.ptr, inf->samples) < 0) {
					ast_log(LOG_WARNING, "softmodem returned error\n");
					res = -1;
					break;
				}
			} else if (s->version == VERSION_V18_45 || s->version == VERSION_V18_50) {
				/* You might think it's sloppy to do it this way, since
				 * we'll only be able to send or receive at any one time.
				 * This is fine, since TDDs by nature are not full duplex devices.
				 * If the TDD is receiving output from the server, then it will
				 * itself buffer any input until the output has stopped,
				 * so we won't get anything back from the TDD until output has
				 * finished being sent. */
				/* Data from channel to send to socket? */
				if (v18_rx(v18_modem, inf->data.ptr, inf->samples) < 0) {
					ast_log(LOG_WARNING, "softmodem returned error\n");
					res = -1;
					break;
				}
				/* Data from socket to send to channel? */
				pres = poll(&pfd, 1, 0);
				if (pres < 0) {
					ast_debug(1, "Socket disconnected\n");
					res = -1;
					break;
				} else if (pres > 0) {
					char *writebuf;
					int written, writelen, now = time(NULL);
					int needspaces = lastoutput < now - 2;
#ifdef HAVE_OPENSSL
					if (ssl) {
						pres = SSL_read(ssl, buf + 2, sizeof(buf) - 3);
					} else
#endif
					{
						pres = read(sock, buf + 2, sizeof(buf) - 3);
					}
					if (pres <= 0) {
#ifdef HAVE_OPENSSL
						if (ssl) {
							int sslerr = SSL_get_error(ssl, pres);
							ast_debug(1, "SSL_read returned %d\n", pres);
							/* Since socket is nonblocking, we may need to retry */
							if (sslerr == SSL_ERROR_WANT_READ) {
								continue;
							}
						} else
#else
						{
							ast_debug(1, "read returned %d, socket must have disconnected on us\n", pres); /* Socket hangup, read will return 0 */
						}
#endif
						res = -1;
						break;
					}
					buf[pres] = '\0'; /* Safe */
					/* This is probably completely the wrong way to do this.
					 * If carrier has paused and we get something again,
					 * the first 2 characters can get clipped off by the time carrier has stabilized again.
					 * Send 2 spaces to get things moving and then send the real output, if that's the case. */
					writelen = needspaces ? pres + 2 : pres;
					writebuf = needspaces ? buf : buf + 2;
					written = v18_put(v18_modem, writebuf, writelen); /* v18_generator_generate will actually write the frames onto the channel towards the TDD */
					ast_debug(3, "v18 put: %d+%d bytes from socket onto modem, written: %d\n", needspaces ? 2 : 0, pres, written);
					while (written < writelen) {
						writebuf += written;
						writelen -= written;
						/* v18_put couldn't accept all the data available for the modem,
						 * because the SpanDSP buffer was full.
						 * We need to retry and make sure all data gets sent. */
						/* It will take time for the SpanDSP buffer to empty itself... we're in no hurry here. */
						if (ast_safe_sleep(s->chan, 1000)) { /* Check if channel hung up while we were waiting */
							res = -1;
							break;
						}
						written = v18_put(v18_modem, writebuf, writelen);
						ast_debug(3, "v18 put RETRY: %d bytes from socket onto modem, written: %d\n", writelen, written);
					}
					if (written <= 0) {
						ast_debug(1, "v18_put returned %d?\n", written);
						res = -1;
						break;
					}
				} /* else, no new data */
			}
		}

		ast_frfree(inf);
		inf = NULL;
	}

	if (res == 0 && s->finished && (s->version == VERSION_V22 || s->version == VERSION_V22BIS)) {
		res = softmodem_drain(s, v22_modem);
	}

cleanup:
	/* The modem objects below are the generator's data, and the channel is
	 * still up when the TCP side ended the session: stop it before freeing */
	ast_deactivate_generator(s->chan);
	close(sock);
#ifdef HAVE_OPENSSL
	if (ssl) {
		SSL_free(ssl);
		SSL_CTX_free(ctx);
	}
#endif

	if (s->version == VERSION_V22 || s->version == VERSION_V22BIS) {
		v22bis_release(v22_modem);
		v22bis_free(v22_modem);
		if (s->use_v42) {
			/* No v42_free(): s->v42 is embedded in the session (on the stack) */
			v42_release(&s->v42);
			if (s->v42bis) {
				v42bis_release(s->v42bis);
				v42bis_free(s->v42bis);
			}
		}
	} else if (s->version == VERSION_V18_45 || s->version == VERSION_V18_50) {
		v18_release(v18_modem);
		v18_free(v18_modem);
	}

	if (original_write_fmt != ast_format_slin) {
		if (ast_set_write_format(s->chan, original_write_fmt) < 0) {
			ast_log(LOG_WARNING, "Unable to restore write format on '%s'\n", ast_channel_name(s->chan));
		}
	}

	if (original_read_fmt != ast_format_slin) {
		if (ast_set_read_format(s->chan, original_read_fmt) < 0) {
			ast_log(LOG_WARNING, "Unable to restore read format on '%s'\n", ast_channel_name(s->chan));
		}
	}

	return res;
}

static int softmodem_exec(struct ast_channel *chan, const char *data)
{
	int res = 0;
	char *parse;
	modem_session session;
	struct ast_flags options = { 0, };
	char *option_args[OPT_ARG_ARRAY_SIZE];

	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(host);
		AST_APP_ARG(port);
		AST_APP_ARG(options);
	);

	if (!chan) {
		ast_log(LOG_ERROR, "Channel is NULL.\n");
		return -1;
	}

	/* answer channel if not already answered */
	res = ast_auto_answer(chan);
	if (res) {
		ast_log(LOG_WARNING, "Could not answer channel '%s'\n", ast_channel_name(chan));
		return res;
	}

	/* Set defaults; everything not set here is zero */
	memset(&session, 0, sizeof(session));
	session.chan = chan;
	session.finished = 0;
	session.rxcutoff = -35.0f;
	session.txpower = -28.0f;
	session.version = VERSION_V23;
	session.lsb = 1;
	session.databits = 8;
	session.stopbits = 1;
	session.paritytype = 0;
	session.ulmheader = 0;
	session.sendnull = 0;

	parse = ast_strdupa(S_OR(data, ""));
	AST_STANDARD_APP_ARGS(args, parse);

	if (args.host) {
		session.host = ast_strip(args.host); /* if there are spaces in the hostname, we crash at the memcpy after hp = ast_gethostbyname(s->host, &ahp); */
		if (strcmp(session.host, args.host)) {
			ast_log(LOG_WARNING, "Please avoid spaces in the hostname: '%s'\n", args.host);
		}
	} else {
		session.host = "localhost";
	}

	if (args.port) {
		session.port = atoi(args.port);
		if ((session.port < 0) || (session.port > 65535)) {
			ast_log(LOG_ERROR, "Please specify a valid port number.\n");
			return -1;
		}
	} else {
		session.port = 23;
	}

	if (args.options) {
		ast_app_parse_options(additional_options, &options, option_args, args.options);

		if (ast_test_flag(&options, OPT_RX_CUTOFF) && !ast_strlen_zero(option_args[OPT_ARG_RX_CUTOFF])) {
			session.rxcutoff = atof(option_args[OPT_ARG_RX_CUTOFF]);
		}

		if (ast_test_flag(&options, OPT_TX_POWER) && !ast_strlen_zero(option_args[OPT_ARG_TX_POWER])) {
			session.txpower = atof(option_args[OPT_ARG_TX_POWER]);
		}

		if (ast_test_flag(&options, OPT_MODEM_VERSION)) {
			if (!ast_strlen_zero(option_args[OPT_ARG_MODEM_VERSION])) {
				if (!strcasecmp(option_args[OPT_ARG_MODEM_VERSION], "V21")) {
					session.version = VERSION_V21;
				} else if (!strcasecmp(option_args[OPT_ARG_MODEM_VERSION], "V23")) {
					session.version = VERSION_V23;
				} else if (!strcasecmp(option_args[OPT_ARG_MODEM_VERSION], "Bell103")) {
					session.version = VERSION_BELL103;
				} else if (!strcasecmp(option_args[OPT_ARG_MODEM_VERSION], "Bell202")) {
					session.version = VERSION_BELL202;
				} else if (!strcasecmp(option_args[OPT_ARG_MODEM_VERSION], "V22")) {
					session.version = VERSION_V22;
				} else if (!strcasecmp(option_args[OPT_ARG_MODEM_VERSION], "V22bis")) {
					session.version = VERSION_V22BIS;
				} else if (!strcasecmp(option_args[OPT_ARG_MODEM_VERSION], "baudot45")) {
					session.version = VERSION_V18_45;
				} else if (!strcasecmp(option_args[OPT_ARG_MODEM_VERSION], "baudot50")) {
					session.version = VERSION_V18_50;
				}
			}
		}

		if (ast_test_flag(&options, OPT_MSB_FIRST)) {
			if (ast_test_flag(&options, OPT_LSB_FIRST)) {
				ast_log(LOG_ERROR, "Please only set l or m flag, not both.\n");
				return -1;
			}
			session.lsb = 0;
		}

		if (ast_test_flag(&options, OPT_DATABITS)) {
			if (!ast_strlen_zero(option_args[OPT_ARG_DATABITS])) {
				session.databits = atoi(option_args[OPT_ARG_DATABITS]);
				if ((session.databits < 5) || (session.databits > 8)) {
					ast_log(LOG_ERROR, "Only 5-8 data bits are supported.\n");
					return -1;
				}
			}
		}

		if (ast_test_flag(&options, OPT_STOPBITS)) {
			if (!ast_strlen_zero(option_args[OPT_ARG_STOPBITS])) {
				session.stopbits = atoi(option_args[OPT_ARG_STOPBITS]);
				if ((session.stopbits < 1) || (session.stopbits > 2)) {
					ast_log(LOG_ERROR, "Only 1-2 stop bits are supported.\n");
					return -1;
				}
			}
		}

		if (ast_test_flag(&options, OPT_EVEN_PARITY)) {
			if (ast_test_flag(&options, OPT_ODD_PARITY)) {
				ast_log(LOG_ERROR, "Please only set e or o (parity) flag, not both.\n");
				return -1;
			}
			session.paritytype = 1;
		}
		if (ast_test_flag(&options, OPT_ODD_PARITY)) {
			session.paritytype = 2;
		}
		if (ast_test_flag(&options, OPT_ULM_HEADER)) {
			session.ulmheader = 1;
		}
		if (ast_test_flag(&options, OPT_NULL)) {
			session.sendnull = 1;
		}
		session.flipmode = ast_test_flag(&options, OPT_FLIP_MODE) ? 1 : 0;
		if (ast_test_flag(&options, OPT_V42)) {
			if (session.version != VERSION_V22 && session.version != VERSION_V22BIS) {
				ast_log(LOG_WARNING, "V.42 (option c) is only supported for V22/V22bis, ignoring\n");
			} else {
				session.use_v42 = 1;
			}
		}
		if (ast_test_flag(&options, OPT_V42BIS)) {
			if (!session.use_v42) {
				ast_log(LOG_WARNING, "V.42bis (option b) requires V.42 (option c), ignoring\n");
			} else {
				session.use_v42bis = 1;
			}
		}
	}

	session.track_session = 1;
	res = softmodem_communicate(&session, ast_test_flag(&options, OPT_TLS));
	if (res <= 0 && session.track_session) { /* Session was only tracked if we didn't return 1 */
		untrack_session(&session);
	}
	return res;
}

static int unload_module(void)
{
	ast_manager_unregister("SoftmodemSessions");
	return ast_unregister_application(app);
}

static int load_module(void)
{
	if (ast_manager_register_xml("SoftmodemSessions", EVENT_FLAG_CALL, manager_softmodem_sessions)) {
		return -1;
	}
	return ast_register_application_xml(app, softmodem_exec);
}

AST_MODULE_INFO_STANDARD_EXTENDED(ASTERISK_GPL_KEY, "Softmodem (V18, V21, V22, V23, Baudot)");
