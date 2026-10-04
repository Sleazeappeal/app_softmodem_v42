/* SPDX-License-Identifier: GPL-2.0-only */
/* Offline decode of one side of a MixMonitor recording (8 kHz, 16-bit mono
 * WAV) with a spandsp V.22bis receiver, async 8N1 framing, printed as text.
 *
 * usage: v22decode file.wav caller|answerer [t]
 *   t: print the time (s) before every decoded byte
 *   caller:   we play the calling modem, so this decodes the answerer's
 *             (softmodem's) transmission -- use the "-us.wav" file
 *   answerer: decodes the calling modem's transmission -- "-caller.wav"
 * Non-printable bytes are shown as <xx>. Data before carrier up is ignored.
 * Only meaningful for non-EC calls (V.42 frames are bit-synchronous).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <spandsp.h>

static int bitpos = -1;
static int byte;
static long nbytes;
static long samples_done;
static int show_times;

static void put_bit(void *user_data, int bit)
{
    if (bit < 0)
    {
        if (bit == SIG_STATUS_CARRIER_UP  ||  bit == SIG_STATUS_TRAINING_SUCCEEDED)
            printf("\n[%.2f s: status %d]\n", samples_done/8000.0, bit);
        return;
    }
    if (bitpos < 0)
    {
        if (bit == 0)
        {
            bitpos = 0;
            byte = 0;
        }
        return;
    }
    if (bitpos < 8)
    {
        byte |= (bit << bitpos);
        bitpos++;
        return;
    }
    /* stop bit */
    if (show_times)
        printf("[%.3f]", samples_done/8000.0);
    if (bit == 1)
    {
        if (byte == '\r'  ||  byte == '\n'  ||  (byte >= 0x20  &&  byte < 0x7F))
            putchar(byte);
        else
            printf("<%02x>", byte);
    }
    else
    {
        printf("<FE>");
    }
    nbytes++;
    bitpos = -1;
}

static int get_bit(void *user_data)
{
    return 1;
}

int main(int argc, char *argv[])
{
    FILE *f;
    int16_t amp[160];
    int16_t txbuf[160];
    v22bis_state_t *s;
    int n;
    int caller;

    if (argc < 3  ||  (f = fopen(argv[1], "rb")) == NULL)
    {
        fprintf(stderr, "usage: %s file.wav caller|answerer\n", argv[0]);
        return 1;
    }
    caller = (strcmp(argv[2], "caller") == 0);
    show_times = (argc > 3);
    fseek(f, 44, SEEK_SET);
    s = v22bis_init(NULL, 2400, 0, caller, get_bit, NULL, put_bit, NULL);
    v22bis_rx_signal_cutoff(s, -45.0f);
    while ((n = fread(amp, sizeof(int16_t), 160, f)) > 0)
    {
        v22bis_tx(s, txbuf, n);
        v22bis_rx(s, amp, n);
        samples_done += n;
    }
    printf("\n[end at %.2f s, %ld bytes, bit rate %d]\n", samples_done/8000.0, nbytes, v22bis_get_current_bit_rate(s));
    fclose(f);
    return 0;
}
