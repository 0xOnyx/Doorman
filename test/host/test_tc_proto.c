#include "t.h"
#include "tc_proto.h"
#include <string.h>

static size_t enc(uint32_t raw, bool is_long, bool is_ack, tc_segment_t *s)
{
    return tc_proto_encode(raw, is_long, is_ack, s, TC_MAX_SEGMENTS);
}

static void test_parity(void)
{
    CHECK_EQ(tc_proto_parity(0x0000, 16), 1);
    CHECK_EQ(tc_proto_parity(0x0001, 16), 0);
    CHECK_EQ(tc_proto_parity(0x1100, 16), 1);       /* 2 bits a 1 */
    CHECK_EQ(tc_proto_parity(0x1200, 16), 1);
    CHECK_EQ(tc_proto_parity(0x0C30BA80, 32), 1);   /* 10 bits a 1 */
    CHECK_EQ(tc_proto_parity(0x1, 4), 0);
    CHECK_EQ(tc_proto_parity(0xFFFF0001, 16), 0);   /* bits > nbits ignores */
}

static void test_encode_0x1100(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    size_t n = enc(0x1100, false, false, s);
    CHECK_EQ(n, 19);
    CHECK_EQ(s[0].duration_us, 5985);
    CHECK(s[0].mark);
    CHECK_EQ(s[1].duration_us, 2000);     /* LEN = 0 (court) */
    CHECK(!s[1].mark);
    /* 0x1100 = 0001 0001 0000 0000, MSB en premier */
    static const uint16_t bits[16] = { 2000, 2000, 2000, 4000, 2000, 2000, 2000, 4000,
                                       2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000 };
    for (int i = 0; i < 16; i++) {
        CHECK_EQ(s[2 + i].duration_us, bits[i]);
    }
    CHECK_EQ(s[18].duration_us, 4000);    /* CHK = 1 */
    CHECK(s[18].mark);
    /* alternance stricte : index pair = mark, impair = space */
    for (size_t k = 0; k < n; k++) {
        CHECK(s[k].mark == (k % 2 == 0));
    }
}

static void test_encode_long_and_ack(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    size_t n = enc(0x0C30BA80, true, false, s);
    CHECK_EQ(n, 35);
    CHECK_EQ(s[1].duration_us, 4000);     /* LEN = 1 (long) */
    CHECK_EQ(s[2].duration_us, 2000);     /* bit 31 = 0 */
    CHECK_EQ(s[34].duration_us, 4000);    /* CHK = 1 */
    for (size_t k = 0; k < n; k++) {
        CHECK(s[k].mark == (k % 2 == 0));
    }

    n = enc(0x1, false, true, s);
    CHECK_EQ(n, 7);
    CHECK_EQ(s[0].duration_us, 6200);
    CHECK_EQ(s[1].duration_us, 2000);
    CHECK_EQ(s[5].duration_us, 4000);     /* b0 = 1 */
    CHECK_EQ(s[6].duration_us, 2000);     /* CHK = 1 XOR 1 = 0 */
    for (size_t k = 0; k < n; k++) {
        CHECK(s[k].mark == (k % 2 == 0));
    }
}

static void test_encode_refus(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    CHECK_EQ(enc(0x12345, false, false, s), 0);     /* court > 0xFFFF */
    CHECK_EQ(enc(0x10, false, true, s), 0);         /* ACK > 0xF */
    CHECK_EQ(enc(0x1, true, true, s), 0);           /* ACK long n'existe pas */
    CHECK_EQ(tc_proto_encode(0x1100, false, false, s, 18), 0);  /* tampon trop petit */
}

int main(void)
{
    RUN(test_parity);
    RUN(test_encode_0x1100);
    RUN(test_encode_long_and_ack);
    RUN(test_encode_refus);
    T_END();
}
