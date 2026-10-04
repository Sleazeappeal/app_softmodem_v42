/* SPDX-License-Identifier: GPL-2.0-only */
/* Malformed-input and round-trip tests for the patched spandsp V.42 XID
 * parser and V.42bis codec, built with AddressSanitizer/UBSan by
 * fuzz_regress.sh. Every input sits in an exact-size heap buffer, so a read
 * past it is reported. Exit 0 = no crash; the printed values show whether an
 * input was rejected (comp/dict/maxstr 0) or parsed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#define SPANDSP_EXPOSE_INTERNAL_STRUCTURES
#include <spandsp.h>

static void put_nothing(void *user_data, const uint8_t *buf, int len)
{
}

static int get_nothing(void *user_data, uint8_t *buf, int max_len)
{
    return 0;
}

static uint8_t capture[65536];
static int capture_len;

static void put_capture(void *user_data, const uint8_t *buf, int len)
{
    memcpy(capture + capture_len, buf, len);
    capture_len += len;
}

/* Valid data must still survive compress -> decompress unchanged */
static void test_roundtrip(int dict)
{
    v42bis_state_t *enc;
    v42bis_state_t *dec;
    uint8_t data[8192];
    uint8_t coded[65536];
    int coded_len;
    int i;
    int rc;

    for (i = 0;  i < (int) sizeof(data);  i++)
        data[i] = (i % 3)  ?  "The quick brown fox jumps over the lazy dog. "[i % 45]  :  (uint8_t) (i*7919 >> 3);
    enc = v42bis_init(NULL, 3, dict, 32, put_capture, NULL, 1024, put_nothing, NULL, 1024);
    capture_len = 0;
    v42bis_compress(enc, data, sizeof(data));
    v42bis_compress_flush(enc);
    coded_len = capture_len;
    memcpy(coded, capture, coded_len);
    dec = v42bis_init(NULL, 3, dict, 32, put_nothing, NULL, 1024, put_capture, NULL, 1024);
    capture_len = 0;
    rc = v42bis_decompress(dec, coded, coded_len);
    v42bis_decompress_flush(dec);
    printf("v42bis roundtrip 8192 bytes         dict %4d: rc=%d, %d coded bytes, decoded %s\n",
           dict, rc, coded_len,
           (capture_len == (int) sizeof(data)  &&  memcmp(capture, data, sizeof(data)) == 0)  ?  "identical"  :  "DIFFERENT");
    v42bis_free(enc);
    v42bis_free(dec);
}

static uint8_t *heap_copy(const uint8_t *src, int len)
{
    uint8_t *p = malloc(len);

    memcpy(p, src, len);
    return p;
}

static void test_v42bis(const char *name, const uint8_t *in, int len, int dict)
{
    v42bis_state_t *s;
    uint8_t *buf;
    int rc;

    s = v42bis_init(NULL, 3, dict, 32, put_nothing, NULL, 1024, put_nothing, NULL, 1024);
    buf = heap_copy(in, len);
    rc = v42bis_decompress(s, buf, len);
    printf("v42bis %-28s dict %4d: decompress rc=%d\n", name, dict, rc);
    free(buf);
    v42bis_free(s);
}

static void test_xid(const char *name, const uint8_t *in, int len)
{
    v42_state_t *s;
    uint8_t *buf;
    int comp, dict, maxstr;

    s = v42_init(NULL, FALSE, TRUE, get_nothing, put_nothing, NULL);
    /* local request differs from every test XID, so a parsed XID shows up */
    v42_set_local_v42bis_request(s, 1, 512, 6);
    buf = heap_copy(in, len);
    lapm_receive(s, buf, len, TRUE);
    /* the same frame with the other C/R address */
    buf[0] ^= 0x02;
    lapm_receive(s, buf, len, TRUE);
    v42_get_negotiated_v42bis(s, &comp, &dict, &maxstr);
    printf("xid    %-28s len %3d: survived, v42bis comp=%d dict=%d maxstr=%d\n", name, len, comp, dict, maxstr);
    free(buf);
    v42_free(s);
}

int main(void)
{
    /* four STEPUPs, then a 13-bit codeword (index 8191) */
    static const uint8_t stepup[] = {0x00, 0x00, 0x02, 0x04, 0x10, 0x80, 0x00, 0xfc, 0x7f};
    /* codeword 511 with a 512-entry dictionary: unassigned but in range */
    static const uint8_t unassigned[] = {0xff, 0x01};
    /* XID whose parameter length runs past the group */
    static const uint8_t param_past[] = {0x01, 0xaf, 0x82, 0x80, 0x00, 0x03, 0x05, 0x04, 0x00};
    static const uint8_t xid_short1[] = {0x01};
    static const uint8_t xid_short2[] = {0x01, 0xaf};
    static const uint8_t xid_trunc_group[] = {0x01, 0xaf, 0x82, 0x80, 0x00};
    static const uint8_t xid_group_too_long[] = {0x01, 0xaf, 0x82, 0xf0, 0x00, 0x20, 0x00};
    /* a real Rockwell XID, must parse */
    static const uint8_t xid_rockwell[] = {
        0x03, 0xaf, 0x82, 0x80, 0x00, 0x13, 0x03, 0x03, 0x8a, 0x89, 0x00, 0x05, 0x02, 0x04, 0x00,
        0x06, 0x02, 0x04, 0x00, 0x07, 0x01, 0x0f, 0x08, 0x01, 0x0f, 0xf0, 0x00, 0x0f, 0x00, 0x03,
        0x56, 0x34, 0x32, 0x01, 0x01, 0x03, 0x02, 0x02, 0x08, 0x00, 0x03, 0x01, 0x20};
    /* a real Conexant SoftK56 XID (first 64 bytes, then one closing
       parameter): V.44 offer in a user data subfield (0xFF, no length) */
    static const uint8_t xid_softk56[] = {
        0x03, 0xaf, 0x82, 0x80, 0x00, 0x13, 0x03, 0x03, 0x8a, 0x89, 0x00, 0x05, 0x02, 0x04, 0x00,
        0x06, 0x02, 0x04, 0x00, 0x07, 0x01, 0x0f, 0x08, 0x01, 0x0f, 0xf0, 0x00, 0x0f, 0x00, 0x03,
        0x56, 0x34, 0x32, 0x01, 0x01, 0x03, 0x02, 0x02, 0x08, 0x00, 0x03, 0x01, 0x20, 0xff, 0x40,
        0x03, 0x56, 0x34, 0x34, 0x41, 0x01, 0x00, 0x42, 0x01, 0x03, 0x43, 0x02, 0x08, 0x00, 0x44,
        0x02, 0x08, 0x00, 0x45, 0x01, 0x00};
    /* valid frame, but V.42bis P2 = 251 (out of range): must be ignored */
    static const uint8_t xid_bad_p2[] = {
        0x03, 0xaf, 0x82, 0xf0, 0x00, 0x0f, 0x00, 0x03, 0x56, 0x34, 0x32, 0x01, 0x01, 0x03,
        0x02, 0x02, 0x08, 0x00, 0x03, 0x01, 0xfb};
    /* user data subfield only, and a bare 0xFF at the end */
    static const uint8_t xid_userdata_only[] = {0x03, 0xaf, 0x82, 0xff};
    static const uint8_t i_short[] = {0x01, 0x00};

    /* XID first: the stock decoder crashes on the STEPUP input */
    test_xid("param past group", param_past, sizeof(param_past));
    test_xid("1 byte frame", xid_short1, sizeof(xid_short1));
    test_xid("2 byte U frame", xid_short2, sizeof(xid_short2));
    test_xid("truncated group header", xid_trunc_group, sizeof(xid_trunc_group));
    test_xid("group longer than frame", xid_group_too_long, sizeof(xid_group_too_long));
    test_xid("Rockwell XID (valid)", xid_rockwell, sizeof(xid_rockwell));
    test_xid("SoftK56 XID + V.44 user data", xid_softk56, sizeof(xid_softk56));
    test_xid("V.42bis P2 out of range", xid_bad_p2, sizeof(xid_bad_p2));
    test_xid("user data subfield only", xid_userdata_only, sizeof(xid_userdata_only));
    test_xid("2 byte I frame", i_short, sizeof(i_short));
    test_roundtrip(512);
    test_roundtrip(2048);
    test_roundtrip(4096);
    test_v42bis("unassigned codeword", unassigned, sizeof(unassigned), 512);
    test_v42bis("stepup x4 + 13-bit code", stepup, sizeof(stepup), 512);
    test_v42bis("stepup x4 + 13-bit code", stepup, sizeof(stepup), 4096);
    printf("done\n");
    return 0;
}
