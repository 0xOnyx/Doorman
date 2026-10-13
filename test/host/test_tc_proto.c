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

static bool dec(const tc_segment_t *s, size_t n, tc_telegram_t *t, tc_decode_error_t *e)
{
    return tc_proto_decode(s, n, t, e);
}

static void test_encode_edges(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    size_t n = enc(0xFFFFFFFF, true, false, s);
    CHECK_EQ(n, 35);
    for (size_t i = 2; i < 34; i++) {
        CHECK_EQ(s[i].duration_us, 4000);
    }
    CHECK_EQ(s[34].duration_us, 4000);              /* 32 bits a 1 -> CHK = 1 */
    n = enc(0x00000000, true, false, s);
    CHECK_EQ(n, 35);
    for (size_t i = 2; i < 34; i++) {
        CHECK_EQ(s[i].duration_us, 2000);
    }
    CHECK_EQ(s[34].duration_us, 4000);              /* aucun bit -> CHK = 1 */
    CHECK_EQ(enc(0x0, false, true, s), 7);
    CHECK_EQ(s[6].duration_us, 4000);               /* ACK 0 -> CHK = 1 */
    CHECK_EQ(enc(0xF, false, true, s), 7);
    CHECK_EQ(s[6].duration_us, 4000);               /* 4 bits -> CHK = 1 */
    CHECK_EQ(enc(0x1100, true, false, s), 35);      /* long avec valeur courte : autorise */
    CHECK_EQ(tc_proto_encode(0x1100, false, false, s, 19), 19);   /* taille exacte */
    CHECK_EQ(tc_proto_encode(0x1, false, true, s, 7), 7);
    CHECK_EQ(tc_proto_encode(0x1, false, true, s, 0), 0);
    CHECK_EQ(tc_proto_encode(0x1, false, true, NULL, 96), 0);
}

static void test_decode_roundtrip(void)
{
    struct { uint32_t raw; bool is_long; bool is_ack; } v[] = {
        { 0x1100, false, false }, { 0x1200, false, false }, { 0x0C30BA80, true, false },
        { 0x1C30BA41, true, false }, { 0x1C30BA80, true, false }, { 0x1, false, true },
        { 0xFFFFFFFF, true, false }, { 0x0000, false, false }, { 0xF, false, true },
        { 0x1100, true, false },
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        tc_segment_t s[TC_MAX_SEGMENTS];
        size_t n = enc(v[i].raw, v[i].is_long, v[i].is_ack, s);
        tc_telegram_t t;
        tc_decode_error_t e;
        memset(&t, 0, sizeof t);
        CHECK(dec(s, n, &t, &e));
        CHECK_EQ(e, TC_ERR_NONE);
        CHECK_EQ(t.raw, v[i].raw);
        CHECK(t.is_long == v[i].is_long);
        CHECK(t.is_response == v[i].is_ack);
        CHECK_EQ(t.type, TC_TYPE_UNKNOWN);          /* decode ne classe pas */
        CHECK_EQ(t.serial, 0);
    }
}

static void test_decode_tolerance(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    size_t n = enc(0x1100, false, false, s);
    tc_telegram_t t;
    tc_decode_error_t e;
    memset(&t, 0, sizeof t);
    const int scale[4] = { 80, 120, 90, 110 };     /* pourcentages */
    for (int k = 0; k < 4; k++) {
        tc_segment_t m[TC_MAX_SEGMENTS];
        memcpy(m, s, sizeof m);
        for (size_t i = 1; i < n; i++) {           /* LEN, donnees, CHK */
            m[i].duration_us = (uint16_t)(m[i].duration_us * scale[k] / 100);
        }
        CHECK(dec(m, n, &t, &e));
        CHECK_EQ(t.raw, 0x1100);
    }
    /* depart commande a +-100 us */
    s[0].duration_us = 5885; CHECK(dec(s, n, &t, &e));
    s[0].duration_us = 6085; CHECK(dec(s, n, &t, &e));
    /* bornes exactes des fenetres */
    s[0].duration_us = 5700; CHECK(dec(s, n, &t, &e)); CHECK(!t.is_response);
    s[0].duration_us = 6089; CHECK(dec(s, n, &t, &e)); CHECK(!t.is_response);
    s[0].duration_us = 6090; CHECK(dec(s, n, &t, &e)); CHECK(t.is_response);
    s[0].duration_us = 6900; CHECK(dec(s, n, &t, &e)); CHECK(t.is_response);
    s[0].duration_us = 5699; CHECK(!dec(s, n, &t, &e)); CHECK_EQ(e, TC_ERR_START);
    s[0].duration_us = 6901; CHECK(!dec(s, n, &t, &e)); CHECK_EQ(e, TC_ERR_START);
    s[0].duration_us = 5985;
    s[2].duration_us = 2999; CHECK(dec(s, n, &t, &e)); CHECK_EQ(t.raw, 0x1100);
    s[2].duration_us = 1000; CHECK(dec(s, n, &t, &e)); CHECK_EQ(t.raw, 0x1100);
    s[2].duration_us = 999;  CHECK(!dec(s, n, &t, &e)); CHECK_EQ(e, TC_ERR_TIMING);
    s[2].duration_us = 2000;
    /* depart ACK a +-100 us */
    n = enc(0x1, false, true, s);
    s[0].duration_us = 6100; CHECK(dec(s, n, &t, &e)); CHECK(t.is_response);
    s[0].duration_us = 6300; CHECK(dec(s, n, &t, &e)); CHECK(t.is_response);
}

static void test_decode_rejects(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    tc_telegram_t t;
    tc_decode_error_t e;
    memset(&t, 0, sizeof t);
    size_t n = enc(0x1100, false, false, s);

    tc_segment_t m[TC_MAX_SEGMENTS];
    memcpy(m, s, sizeof m);
    m[5].duration_us = 5500;                       /* bit hors fenetres */
    CHECK(!dec(m, n, &t, &e)); CHECK_EQ(e, TC_ERR_TIMING);

    memcpy(m, s, sizeof m);
    m[0].duration_us = 5000;                       /* depart hors fenetres */
    CHECK(!dec(m, n, &t, &e)); CHECK_EQ(e, TC_ERR_START);

    memcpy(m, s, sizeof m);
    m[18].duration_us = 2000;                      /* CHK inverse */
    CHECK(!dec(m, n, &t, &e)); CHECK_EQ(e, TC_ERR_PARITY);

    memcpy(m, s, sizeof m);
    m[18].duration_us = 500;                       /* CHK hors fenetres */
    CHECK(!dec(m, n, &t, &e)); CHECK_EQ(e, TC_ERR_TIMING);

    memcpy(m, s, sizeof m);
    m[1].duration_us = 4000;                       /* LEN = 1 sur 19 segments */
    CHECK(!dec(m, n, &t, &e)); CHECK_EQ(e, TC_ERR_LEN_BIT);

    memcpy(m, s, sizeof m);
    m[1].duration_us = 7000;                       /* LEN hors fenetres */
    CHECK(!dec(m, n, &t, &e)); CHECK_EQ(e, TC_ERR_TIMING);

    memcpy(m, s, sizeof m);
    m[19].duration_us = 2000; m[19].mark = false;  /* 20 segments */
    CHECK(!dec(m, 20, &t, &e)); CHECK_EQ(e, TC_ERR_LENGTH);

    CHECK(!dec(m, 0, &t, &e)); CHECK_EQ(e, TC_ERR_LENGTH);
    CHECK(!dec(m, 18, &t, &e)); CHECK_EQ(e, TC_ERR_LENGTH);

    /* ACK a 7 segments avec LEN = 1 */
    n = enc(0x1, false, true, s);
    s[1].duration_us = 4000;
    CHECK(!dec(s, n, &t, &e)); CHECK_EQ(e, TC_ERR_LEN_BIT);

    /* err == NULL accepte */
    n = enc(0x1100, false, false, s);
    CHECK(dec(s, n, &t, NULL));
    s[18].duration_us = 2000;
    CHECK(!dec(s, n, &t, NULL));
    /* out == NULL accepte */
    s[18].duration_us = 4000;
    CHECK(dec(s, n, NULL, &e)); CHECK_EQ(e, TC_ERR_NONE);
}

static void test_merge_glitches(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    size_t n = enc(0x1100, false, false, s);
    tc_telegram_t t;
    tc_decode_error_t e;
    memset(&t, 0, sizeof t);

    /* parasite au milieu du segment 5 (bit 12 = 1, 4000 space) : 1800 space, 300 mark, 1900 space.
       Les deux morceaux restent >= 1000 : seul le 300 est un parasite. */
    tc_segment_t m[TC_MAX_SEGMENTS];
    memcpy(m, s, 5 * sizeof m[0]);
    m[5] = (tc_segment_t){ 1800, false };
    m[6] = (tc_segment_t){ 300, true };
    m[7] = (tc_segment_t){ 1900, false };
    memcpy(&m[8], &s[6], (n - 6) * sizeof m[0]);
    size_t k = tc_proto_merge_glitches(m, n + 2, 1000);
    CHECK_EQ(k, 19);
    CHECK_EQ(m[5].duration_us, 4000);
    CHECK(!m[5].mark);
    CHECK(dec(m, k, &t, &e));
    CHECK_EQ(t.raw, 0x1100);

    /* parasite en tete */
    m[0] = (tc_segment_t){ 300, false };
    memcpy(&m[1], s, n * sizeof m[0]);
    k = tc_proto_merge_glitches(m, n + 1, 1000);
    CHECK_EQ(k, 19);
    CHECK(dec(m, k, &t, &e));

    /* parasite en queue */
    memcpy(m, s, n * sizeof m[0]);
    m[n] = (tc_segment_t){ 200, false };
    k = tc_proto_merge_glitches(m, n + 1, 1000);
    CHECK_EQ(k, 19);

    /* deux parasites consecutifs : 1500 space, 200 mark, 300 space, 2500 mark :
       le 200 est fondu avec ses voisins (1500 + 200 + 300 = 2000 space), puis 2500 mark reste */
    m[0] = (tc_segment_t){ 1500, false };
    m[1] = (tc_segment_t){ 200, true };
    m[2] = (tc_segment_t){ 300, false };
    m[3] = (tc_segment_t){ 2500, true };
    k = tc_proto_merge_glitches(m, 4, 1000);
    CHECK_EQ(k, 2);
    CHECK_EQ(m[0].duration_us, 2000);
    CHECK_EQ(m[1].duration_us, 2500);

    /* aucun parasite : inchange */
    memcpy(m, s, n * sizeof m[0]);
    CHECK_EQ(tc_proto_merge_glitches(m, n, 1000), n);
    CHECK(memcmp(m, s, n * sizeof m[0]) == 0);

    /* un seul segment parasite -> 0 ; n == 0 -> 0 */
    m[0] = (tc_segment_t){ 100, true };
    CHECK_EQ(tc_proto_merge_glitches(m, 1, 1000), 0);
    CHECK_EQ(tc_proto_merge_glitches(m, 0, 1000), 0);

    /* parasite pres d'un front : [4000 M, 400 S, 300 M, 1300 S] doit redonner [4000 M, 2000 S],
       pas [4700 M, 1300 S] : le plus court (300) est fondu en premier */
    m[0] = (tc_segment_t){ 4000, true };
    m[1] = (tc_segment_t){ 400, false };
    m[2] = (tc_segment_t){ 300, true };
    m[3] = (tc_segment_t){ 1300, false };
    CHECK_EQ(tc_proto_merge_glitches(m, 4, 1000), 2);
    CHECK_EQ(m[0].duration_us, 4000);
    CHECK_EQ(m[1].duration_us, 2000);
    CHECK(m[0].mark && !m[1].mark);

    /* deux parasites de meme duree : le premier gagne, l'alternance est preservee */
    m[0] = (tc_segment_t){ 2000, true };
    m[1] = (tc_segment_t){ 100, false };
    m[2] = (tc_segment_t){ 100, true };
    m[3] = (tc_segment_t){ 1800, false };
    CHECK_EQ(tc_proto_merge_glitches(m, 4, 1000), 2);
    CHECK_EQ(m[0].duration_us, 2200);
    CHECK_EQ(m[1].duration_us, 1800);
    CHECK(m[0].mark != m[1].mark);

    /* parasite pendant le repos enregistre : merge avant strip -> 19 segments */
    memcpy(m, s, n * sizeof m[0]);
    m[n] = (tc_segment_t){ 1500, false };
    m[n + 1] = (tc_segment_t){ 300, true };
    m[n + 2] = (tc_segment_t){ 7000, false };
    k = tc_proto_merge_glitches(m, n + 3, 1000);
    CHECK_EQ(k, 20);
    CHECK_EQ(m[19].duration_us, 8800);
    CHECK_EQ(tc_proto_strip_trailing_idle(m, k), 19);
    CHECK(dec(m, 19, &t, &e));

    /* segs == NULL */
    CHECK_EQ(tc_proto_merge_glitches(NULL, 5, 1000), 0);
    CHECK(!dec(NULL, 19, &t, &e)); CHECK_EQ(e, TC_ERR_LENGTH);

    /* saturation : 65000 + 100 + 65000 est borne a 65535 */
    m[0] = (tc_segment_t){ 65000, true };
    m[1] = (tc_segment_t){ 100, false };
    m[2] = (tc_segment_t){ 65000, true };
    CHECK_EQ(tc_proto_merge_glitches(m, 3, 1000), 1);
    CHECK_EQ(m[0].duration_us, 65535);
}

static void test_strip_trailing_idle(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    size_t n = enc(0x1100, false, false, s);
    s[n] = (tc_segment_t){ 7000, false };
    CHECK_EQ(tc_proto_strip_trailing_idle(s, n + 1), 19);
    s[n] = (tc_segment_t){ 32767, false };
    s[n + 1] = (tc_segment_t){ 32767, true };
    CHECK_EQ(tc_proto_strip_trailing_idle(s, n + 2), 19);
    CHECK_EQ(tc_proto_strip_trailing_idle(s, n), 19);
    CHECK_EQ(tc_proto_strip_trailing_idle(s, 0), 0);
    s[n] = (tc_segment_t){ 6900, false };            /* <= 6900 : conserve */
    CHECK_EQ(tc_proto_strip_trailing_idle(s, n + 1), 20);
}

static tc_telegram_t cls(uint32_t raw, bool is_long, bool is_response)
{
    tc_telegram_t t;
    memset(&t, 0, sizeof t);
    t.raw = raw;
    t.is_long = is_long;
    t.is_response = is_response;
    tc_proto_classify(&t);
    return t;
}

static void test_classify_long(void)
{
    tc_telegram_t t = cls(0x0C30BA80, true, false);
    CHECK_EQ(t.type, TC_TYPE_DOOR_CALL); CHECK_EQ(t.serial, 0xC30BA); CHECK_EQ(t.address, 0);
    t = cls(0x0C30BA43, true, false);
    CHECK_EQ(t.type, TC_TYPE_INTERNAL_CALL); CHECK_EQ(t.serial, 0xC30BA); CHECK_EQ(t.address, 3);
    t = cls(0x1C30BA41, true, false);
    CHECK_EQ(t.type, TC_TYPE_FLOOR_CALL); CHECK_EQ(t.serial, 0xC30BA);
    t = cls(0x1C30BA80, true, false);
    CHECK_EQ(t.type, TC_TYPE_OPEN_DOOR_LONG); CHECK_EQ(t.serial, 0xC30BA); CHECK_EQ(t.address, 0); CHECK_EQ(t.payload, 0);
    t = cls(0x1C30BAC2, true, false);
    CHECK_EQ(t.type, TC_TYPE_OPEN_DOOR_LONG); CHECK_EQ(t.address, 2); CHECK_EQ(t.payload, 1);
    t = cls(0x3C30BA80, true, false);
    CHECK_EQ(t.type, TC_TYPE_START_TALKING); CHECK_EQ(t.serial, 0xC30BA);
    t = cls(0x5C30BAC0, true, false);
    CHECK_EQ(t.type, TC_TYPE_REQUEST_VERSION); CHECK_EQ(t.serial, 0xC30BA);
    t = cls(0x6C30BA08, true, false);
    CHECK_EQ(t.type, TC_TYPE_CONTROL_FUNCTION); CHECK_EQ(t.serial, 0xC30BA); CHECK_EQ(t.payload, 8);
    t = cls(0x82140A0B, true, false);
    CHECK_EQ(t.type, TC_TYPE_WRITE_MEMORY); CHECK_EQ(t.payload, 0x140A0B);
    t = cls(0x810C30BA, true, false);
    CHECK_EQ(t.type, TC_TYPE_SELECT_MEMORY_PAGE); CHECK_EQ(t.serial, 0xC30BA);
    t = cls(0x890C30BA, true, false);
    CHECK_EQ(t.type, TC_TYPE_SELECT_MEMORY_PAGE);
    t = cls(0x9ABCDEF0, true, false);
    CHECK_EQ(t.type, TC_TYPE_UNKNOWN); CHECK_EQ(t.serial, 0); CHECK_EQ(t.payload, 0x9ABCDEF0);
    t = cls(0x1C30BA00, true, false);           /* type 1, ni 0x41 ni bit 7 */
    CHECK_EQ(t.type, TC_TYPE_UNKNOWN);
    t = cls(0x5C30BA00, true, false);           /* type 5 sans 0xC0 */
    CHECK_EQ(t.type, TC_TYPE_UNKNOWN);
}

static void test_classify_short(void)
{
    tc_telegram_t t = cls(0x1100, false, false);
    CHECK_EQ(t.type, TC_TYPE_OPEN_DOOR); CHECK_EQ(t.address, 0); CHECK_EQ(t.payload, 0);
    t = cls(0x1181, false, false);
    CHECK_EQ(t.type, TC_TYPE_OPEN_DOOR); CHECK_EQ(t.address, 1); CHECK_EQ(t.payload, 1);
    t = cls(0x1200, false, false);
    CHECK_EQ(t.type, TC_TYPE_LIGHT);
    t = cls(0x3040, false, false);
    CHECK_EQ(t.type, TC_TYPE_STOP_TALKING); CHECK_EQ(t.address, 0); CHECK_EQ(t.payload, 1);
    t = cls(0x3005, false, false);
    CHECK_EQ(t.type, TC_TYPE_STOP_TALKING); CHECK_EQ(t.address, 5); CHECK_EQ(t.payload, 0);
    t = cls(0x2180, false, false);
    CHECK_EQ(t.type, TC_TYPE_DOOR_CLOSED);
    t = cls(0x2100, false, false);
    CHECK_EQ(t.type, TC_TYPE_DOOR_OPENED);
    CHECK_EQ(cls(0x2200, false, false).type, TC_TYPE_END_OF_RINGTONE);
    CHECK_EQ(cls(0x2400, false, false).type, TC_TYPE_END_OF_DOOR_READINESS);
    CHECK_EQ(cls(0x2800, false, false).type, TC_TYPE_INIT_DOOR_STATION);
    t = cls(0x5041, false, false);
    CHECK_EQ(t.type, TC_TYPE_PROGRAMMING_MODE); CHECK_EQ(t.payload, 1);
    CHECK_EQ(cls(0x5100, false, false).type, TC_TYPE_RESET);
    CHECK_EQ(cls(0x5200, false, false).type, TC_TYPE_SEARCH_DEVICES);
    t = cls(0x840C, false, false);
    CHECK_EQ(t.type, TC_TYPE_READ_MEMORY_BLOCK); CHECK_EQ(t.payload, 0x0C);
    t = cls(0x7FFF, false, false);
    CHECK_EQ(t.type, TC_TYPE_UNKNOWN); CHECK_EQ(t.payload, 0x7FFF);
    /* ACK : seulement avec un depart reponse */
    t = cls(0x5, false, true);
    CHECK_EQ(t.type, TC_TYPE_ACK); CHECK_EQ(t.payload, 5);
    t = cls(0x5, false, false);
    CHECK_EQ(t.type, TC_TYPE_UNKNOWN);
    t = cls(0x1100, false, true);               /* reponse > 0xF : pas un ACK */
    CHECK_EQ(t.type, TC_TYPE_OPEN_DOOR);
    t = cls(0x5, true, true);                   /* long : jamais un ACK */
    CHECK_EQ(t.type, TC_TYPE_DOOR_CALL);
}

static void test_whitelist(void)
{
    CHECK(tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x1100, .type = TC_TYPE_OPEN_DOOR }));
    CHECK(tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x1181, .type = TC_TYPE_OPEN_DOOR }));
    CHECK(tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x1C30BA80, .is_long = true, .type = TC_TYPE_OPEN_DOOR_LONG }));
    CHECK(tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x1200, .type = TC_TYPE_LIGHT }));

    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x5041, .type = TC_TYPE_PROGRAMMING_MODE }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x82140A0B, .is_long = true, .type = TC_TYPE_WRITE_MEMORY }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x840C, .type = TC_TYPE_READ_MEMORY_BLOCK }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x810C30BA, .is_long = true, .type = TC_TYPE_SELECT_MEMORY_PAGE }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x5100, .type = TC_TYPE_RESET }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x2800, .type = TC_TYPE_INIT_DOOR_STATION }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x5200, .type = TC_TYPE_SEARCH_DEVICES }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x7FFF, .type = TC_TYPE_UNKNOWN }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x0C30BA80, .is_long = true, .type = TC_TYPE_DOOR_CALL }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x1, .is_response = true, .type = TC_TYPE_ACK }));
    /* type menteur : raw dit PROGRAMMING_MODE, type dit OPEN_DOOR -> refuse */
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x5041, .type = TC_TYPE_OPEN_DOOR }));
    /* is_long incoherent avec le type */
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x1100, .is_long = true, .type = TC_TYPE_OPEN_DOOR }));
    /* reponse jamais emise, meme avec un raw autorise */
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x1100, .is_response = true, .type = TC_TYPE_OPEN_DOOR }));
    /* NULL */
    CHECK(!tc_proto_tx_allowed(NULL));
    /* balayage : aucun raw court hors 0x11xx / 0x12xx n'est autorise */
    for (uint32_t raw = 0; raw <= 0xFFFF; raw++) {
        tc_telegram_t t = cls(raw, false, false);
        bool allowed = tc_proto_tx_allowed(&t);
        bool expected = (t.type == TC_TYPE_OPEN_DOOR || t.type == TC_TYPE_LIGHT);
        if (allowed != expected) {
            CHECK(allowed == expected);
            break;
        }
    }
    CHECK(true);
}

static void test_names(void)
{
    CHECK(strcmp(tc_type_name(TC_TYPE_OPEN_DOOR), "OPEN_DOOR") == 0);
    CHECK(strcmp(tc_type_name(TC_TYPE_UNKNOWN), "UNKNOWN") == 0);
    CHECK(strcmp(tc_type_name((tc_type_t)999), "?") == 0);
    CHECK(strcmp(tc_decode_error_name(TC_ERR_PARITY), "parity") == 0);
}

int main(void)
{
    RUN(test_parity);
    RUN(test_encode_0x1100);
    RUN(test_encode_long_and_ack);
    RUN(test_encode_refus);
    RUN(test_encode_edges);
    RUN(test_decode_roundtrip);
    RUN(test_decode_tolerance);
    RUN(test_decode_rejects);
    RUN(test_merge_glitches);
    RUN(test_strip_trailing_idle);
    RUN(test_classify_long);
    RUN(test_classify_short);
    RUN(test_whitelist);
    RUN(test_names);
    T_END();
}
