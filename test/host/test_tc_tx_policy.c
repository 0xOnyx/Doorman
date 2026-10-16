#include "t.h"
#include "tc_tx_policy.h"
#include <string.h>

#define MS 1000LL
#define S  1000000LL

/* Telegramme d'ouverture classe, comme le construit l'emetteur */
static tc_telegram_t open_tg(bool long_form)
{
    tc_telegram_t t;
    memset(&t, 0, sizeof t);
    t.raw = tc_tx_open_raw(long_form, 0x4DEF4, 0, &t.is_long);
    tc_proto_classify(&t);
    return t;
}

static void test_open_raw(void)
{
    bool is_long = false;
    CHECK_EQ(tc_tx_open_raw(true, 0x4DEF4, 0, &is_long), 0x14DEF480);
    CHECK(is_long);
    CHECK_EQ(tc_tx_open_raw(false, 0x4DEF4, 0, &is_long), 0x1100);
    CHECK(!is_long);
    /* adresse masquee sur 6 bits */
    CHECK_EQ(tc_tx_open_raw(false, 0, 0xFF, &is_long), 0x113F);
    CHECK_EQ(tc_tx_open_raw(true, 0x4DEF4, 0x45, &is_long), 0x14DEF485);
    /* numero de serie masque sur 20 bits */
    CHECK_EQ(tc_tx_open_raw(true, 0xF4DEF4, 0, &is_long), 0x14DEF480);
    /* is_long facultatif */
    CHECK_EQ(tc_tx_open_raw(true, 0x4DEF4, 0, NULL), 0x14DEF480);

    tc_telegram_t t = open_tg(true);
    CHECK_EQ(t.type, TC_TYPE_OPEN_DOOR_LONG);
    CHECK_EQ(t.serial, 0x4DEF4);
    CHECK(tc_proto_tx_allowed(&t));
    t = open_tg(false);
    CHECK_EQ(t.type, TC_TYPE_OPEN_DOOR);
    CHECK(tc_proto_tx_allowed(&t));
    /* toutes les adresses donnent un telegramme autorise */
    for (unsigned a = 0; a < 256; a++) {
        tc_telegram_t u;
        memset(&u, 0, sizeof u);
        u.raw = tc_tx_open_raw(true, 0x4DEF4, (uint8_t)a, &u.is_long);
        tc_proto_classify(&u);
        CHECK(tc_proto_tx_allowed(&u));
        memset(&u, 0, sizeof u);
        u.raw = tc_tx_open_raw(false, 0x4DEF4, (uint8_t)a, &u.is_long);
        tc_proto_classify(&u);
        CHECK(tc_proto_tx_allowed(&u));
    }
}

static void test_ok_first(void)
{
    tc_tx_limiter_t l;
    tc_tx_limiter_init(&l);
    tc_telegram_t t = open_tg(true);
    CHECK_EQ(tc_tx_check(&l, &t, 10 * S, 0, false), TC_TX_OK);
    /* tout debut : pas encore d'emission, bus calme depuis le boot */
    CHECK_EQ(tc_tx_check(&l, &t, 200 * MS, 0, false), TC_TX_OK);
}

static void test_blocked_first(void)
{
    tc_tx_limiter_t l;
    tc_tx_limiter_init(&l);
    tc_telegram_t t = open_tg(true);
    CHECK_EQ(tc_tx_check(&l, &t, 10 * S, 0, true), TC_TX_BLOCKED);
    /* bloque prioritaire sur tout le reste */
    tc_telegram_t bad = t;
    bad.raw = 0x5000;
    bad.is_long = false;
    tc_proto_classify(&bad);
    CHECK_EQ(tc_tx_check(&l, &bad, 10 * S, 10 * S, true), TC_TX_BLOCKED);
    tc_tx_record(&l, 10 * S);
    CHECK_EQ(tc_tx_check(&l, &t, 10 * S + 1, 10 * S, true), TC_TX_BLOCKED);
    CHECK_EQ(tc_tx_check(&l, NULL, 10 * S, 0, true), TC_TX_BLOCKED);
}

static void test_refused_types(void)
{
    tc_tx_limiter_t l;
    tc_tx_limiter_init(&l);
    struct { uint32_t raw; bool is_long; bool is_response; } cases[] = {
        { 0x5000, false, false },      /* PROGRAMMING_MODE */
        { 0x5001, false, false },
        { 0x82000000, true, false },   /* WRITE_MEMORY */
        { 0x82FFFFFF, true, false },
        { 0x1200, true, false },       /* LIGHT en long : incoherent */
        { 0x11200, false, false },     /* court > 0xFFFF */
        { 0x1100FFFF, false, false },
        { 0x5100, false, false },      /* RESET */
        { 0x5200, false, false },      /* SEARCH_DEVICES */
        { 0x8400, false, false },      /* READ_MEMORY_BLOCK */
        { 0x0DEF480, true, false },    /* DOOR_CALL */
        { 0x34DEF400, true, false },   /* START_TALKING */
        { 0x1100, false, true },       /* reponse */
        { 0x14DEF480, false, false },  /* long declare court */
        { 0x1100, true, false },       /* court declare long */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        tc_telegram_t t;
        memset(&t, 0, sizeof t);
        t.raw = cases[i].raw;
        t.is_long = cases[i].is_long;
        t.is_response = cases[i].is_response;
        tc_proto_classify(&t);
        CHECK_EQ(tc_tx_check(&l, &t, 10 * S, 0, false), TC_TX_REFUSED_TYPE);
        /* type force a OPEN_DOOR : reclasse, toujours refuse */
        t.type = TC_TYPE_OPEN_DOOR;
        CHECK_EQ(tc_tx_check(&l, &t, 10 * S, 0, false), TC_TX_REFUSED_TYPE);
        t.type = TC_TYPE_OPEN_DOOR_LONG;
        CHECK_EQ(tc_tx_check(&l, &t, 10 * S, 0, false), TC_TX_REFUSED_TYPE);
    }
    CHECK_EQ(tc_tx_check(&l, NULL, 10 * S, 0, false), TC_TX_REFUSED_TYPE);
    /* refus de type prioritaire sur too_soon */
    tc_tx_record(&l, 10 * S);
    tc_telegram_t t;
    memset(&t, 0, sizeof t);
    t.raw = 0x5000;
    tc_proto_classify(&t);
    CHECK_EQ(tc_tx_check(&l, &t, 10 * S + 1, 0, false), TC_TX_REFUSED_TYPE);
}

static void test_too_soon(void)
{
    tc_tx_limiter_t l;
    tc_tx_limiter_init(&l);
    tc_telegram_t t = open_tg(true);
    tc_tx_record(&l, 10 * S);
    CHECK_EQ(tc_tx_check(&l, &t, 10 * S, 0, false), TC_TX_TOO_SOON);
    CHECK_EQ(tc_tx_check(&l, &t, 10 * S + 199 * MS, 0, false), TC_TX_TOO_SOON);
    CHECK_EQ(tc_tx_check(&l, &t, 10 * S + 199999, 0, false), TC_TX_TOO_SOON);
    CHECK_EQ(tc_tx_check(&l, &t, 10 * S + 200 * MS, 0, false), TC_TX_OK);
    /* horloge qui recule : prudence */
    CHECK_EQ(tc_tx_check(&l, &t, 9 * S, 0, false), TC_TX_TOO_SOON);
}

static void test_rate_limit(void)
{
    tc_tx_limiter_t l;
    tc_tx_limiter_init(&l);
    tc_telegram_t t = open_tg(false);
    int64_t now = 100 * S;
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(tc_tx_check(&l, &t, now, 0, false), TC_TX_OK);   /* 1re a 5e */
        tc_tx_record(&l, now);
        now += 1 * S;
    }
    /* 6e dans la minute : refusee */
    CHECK_EQ(tc_tx_check(&l, &t, now, 0, false), TC_TX_RATE_LIMIT);
    /* la plus ancienne (100 s) sort de la fenetre a 160 s exactement */
    CHECK_EQ(tc_tx_check(&l, &t, 160 * S - 1, 0, false), TC_TX_RATE_LIMIT);
    CHECK_EQ(tc_tx_check(&l, &t, 160 * S, 0, false), TC_TX_OK);
    tc_tx_record(&l, 160 * S);
    /* glissement : la suivante (101 s) sort a 161 s */
    CHECK_EQ(tc_tx_check(&l, &t, 160 * S + 500 * MS, 0, false), TC_TX_RATE_LIMIT);
    CHECK_EQ(tc_tx_check(&l, &t, 161 * S, 0, false), TC_TX_OK);
    tc_tx_record(&l, 161 * S);
    CHECK_EQ(l.n, 5);
    /* tres longtemps apres : tout est sorti */
    CHECK_EQ(tc_tx_check(&l, &t, 1000 * S, 0, false), TC_TX_OK);
    /* too_soon prioritaire sur rate_limit */
    CHECK_EQ(tc_tx_check(&l, &t, 161 * S + 100 * MS, 0, false), TC_TX_TOO_SOON);
}

static void test_rate_limit_burst(void)
{
    /* 5 emissions espacees de 200 ms : la 6e attend une minute */
    tc_tx_limiter_t l;
    tc_tx_limiter_init(&l);
    tc_telegram_t t = open_tg(true);
    int64_t now = 5 * S;
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(tc_tx_check(&l, &t, now, 0, false), TC_TX_OK);
        tc_tx_record(&l, now);
        now += 200 * MS;
    }
    CHECK_EQ(tc_tx_check(&l, &t, now, 0, false), TC_TX_RATE_LIMIT);
    CHECK_EQ(tc_tx_check(&l, &t, 64 * S, 0, false), TC_TX_RATE_LIMIT);
    CHECK_EQ(tc_tx_check(&l, &t, 65 * S, 0, false), TC_TX_OK);
}

static void test_bus_busy(void)
{
    tc_tx_limiter_t l;
    tc_tx_limiter_init(&l);
    tc_telegram_t t = open_tg(true);
    int64_t edge = 50 * S;
    CHECK_EQ(tc_tx_check(&l, &t, edge, edge, false), TC_TX_BUS_BUSY);
    CHECK_EQ(tc_tx_check(&l, &t, edge + 199 * MS, edge, false), TC_TX_BUS_BUSY);
    CHECK_EQ(tc_tx_check(&l, &t, edge + 199999, edge, false), TC_TX_BUS_BUSY);
    CHECK_EQ(tc_tx_check(&l, &t, edge + 200 * MS, edge, false), TC_TX_OK);
    /* front date apres "maintenant" (horloges lues dans le desordre) : occupe */
    CHECK_EQ(tc_tx_check(&l, &t, edge, edge + 5 * MS, false), TC_TX_BUS_BUSY);
    /* rate_limit prioritaire sur bus_busy */
    for (int i = 0; i < 5; i++) {
        tc_tx_record(&l, edge - 10 * S + i * S);
    }
    CHECK_EQ(tc_tx_check(&l, &t, edge, edge, false), TC_TX_RATE_LIMIT);
}

static void test_record(void)
{
    tc_tx_limiter_t l;
    tc_tx_limiter_init(&l);
    CHECK_EQ(l.n, 0);
    for (int i = 0; i < 12; i++) {
        tc_tx_record(&l, (int64_t)(i + 1) * S);
    }
    CHECK_EQ(l.n, 5);
    CHECK_EQ(l.last_us, 12 * S);
    /* garde les 5 plus recentes */
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(l.sent_us[i], (int64_t)(8 + i) * S);
    }
}

static void test_verdict_names(void)
{
    CHECK(strcmp(tc_tx_verdict_name(TC_TX_OK), "ok") == 0);
    CHECK(strcmp(tc_tx_verdict_name(TC_TX_REFUSED_TYPE), "refused_type") == 0);
    CHECK(strcmp(tc_tx_verdict_name(TC_TX_TOO_SOON), "too_soon") == 0);
    CHECK(strcmp(tc_tx_verdict_name(TC_TX_RATE_LIMIT), "rate_limit") == 0);
    CHECK(strcmp(tc_tx_verdict_name(TC_TX_BUS_BUSY), "bus_busy") == 0);
    CHECK(strcmp(tc_tx_verdict_name(TC_TX_BLOCKED), "blocked") == 0);
    CHECK(strcmp(tc_tx_verdict_name((tc_tx_verdict_t)99), "?") == 0);
}

static unsigned w_d0(uint32_t w) { return w & 0x7FFFu; }
static unsigned w_l0(uint32_t w) { return (w >> 15) & 1u; }
static unsigned w_d1(uint32_t w) { return (w >> 16) & 0x7FFFu; }
static unsigned w_l1(uint32_t w) { return (w >> 31) & 1u; }

static void test_symbols_open_long(void)
{
    tc_segment_t seg[TC_MAX_SEGMENTS];
    uint32_t w[TC_TX_MAX_SYMBOLS];
    size_t n = tc_proto_encode(0x14DEF480, true, false, seg, TC_MAX_SEGMENTS);
    CHECK_EQ(n, 35);
    size_t k = tc_tx_segments_to_symbols(seg, n, w, TC_TX_MAX_SYMBOLS);
    CHECK_EQ(k, 18);
    /* premier symbole : depart (mark = 1) puis LEN long (space = 0) */
    CHECK_EQ(w_d0(w[0]), 5985);
    CHECK_EQ(w_l0(w[0]), 1);
    CHECK_EQ(w_d1(w[0]), 4000);
    CHECK_EQ(w_l1(w[0]), 0);
    for (size_t i = 0; i < k; i++) {
        CHECK_EQ(w_l0(w[i]), 1);
        CHECK_EQ(w_l1(w[i]), 0);   /* chaque symbole se termine a 0 : jamais de bus tenu bas */
        CHECK(w_d0(w[i]) > 0);
        CHECK(w_d1(w[i]) > 0);
    }
    /* dernier : CHK (mark) puis relachement explicite */
    CHECK_EQ(w_d0(w[17]), seg[34].duration_us);
    CHECK_EQ(w_d1(w[17]), TC_TX_RELEASE_US);
    /* aller-retour : les mots relus donnent les memes segments, inverses (RX : mark = niveau 0) */
    tc_segment_t back[TC_MAX_SEGMENTS];
    uint32_t inv[TC_TX_MAX_SYMBOLS];
    for (size_t i = 0; i < k; i++) {
        inv[i] = w[i] ^ 0x80008000u;
    }
    size_t m = tc_proto_rmt_words_to_segments(inv, k, back, TC_MAX_SEGMENTS);
    CHECK_EQ(m, 36);
    m = tc_proto_strip_trailing_idle(back, m - 1);   /* sans le relachement */
    tc_telegram_t t;
    tc_decode_error_t err;
    CHECK(tc_proto_decode(back, m, &t, &err));
    CHECK_EQ(t.raw, 0x14DEF480);
    CHECK(t.is_long);
}

static void test_symbols_open_short(void)
{
    tc_segment_t seg[TC_MAX_SEGMENTS];
    uint32_t w[TC_TX_MAX_SYMBOLS];
    size_t n = tc_proto_encode(0x1100, false, false, seg, TC_MAX_SEGMENTS);
    CHECK_EQ(tc_tx_segments_to_symbols(seg, n, w, TC_TX_MAX_SYMBOLS), 10);
    CHECK_EQ(w_d1(w[9]), TC_TX_RELEASE_US);
    CHECK_EQ(w_l1(w[9]), 0);
}

static void test_symbols_rejects(void)
{
    tc_segment_t seg[TC_MAX_SEGMENTS];
    uint32_t w[TC_TX_MAX_SYMBOLS];
    size_t n = tc_proto_encode(0x1100, false, false, seg, TC_MAX_SEGMENTS);
    CHECK_EQ(tc_tx_segments_to_symbols(seg, n, w, 9), 0);          /* tampon trop petit */
    CHECK_EQ(tc_tx_segments_to_symbols(NULL, n, w, 20), 0);
    CHECK_EQ(tc_tx_segments_to_symbols(seg, n, NULL, 20), 0);
    CHECK_EQ(tc_tx_segments_to_symbols(seg, 0, w, 20), 0);
    tc_segment_t bad[TC_MAX_SEGMENTS];
    memcpy(bad, seg, sizeof bad);
    bad[0].mark = false;                                              /* ne commence pas par un mark */
    CHECK_EQ(tc_tx_segments_to_symbols(bad, n, w, 20), 0);
    memcpy(bad, seg, sizeof bad);
    bad[4].mark = !bad[4].mark;                                       /* alternance cassee */
    CHECK_EQ(tc_tx_segments_to_symbols(bad, n, w, 20), 0);
    memcpy(bad, seg, sizeof bad);
    bad[3].duration_us = 0;                                           /* duree nulle = fin RMT */
    CHECK_EQ(tc_tx_segments_to_symbols(bad, n, w, 20), 0);
    memcpy(bad, seg, sizeof bad);
    bad[6].duration_us = 40000;                                       /* > 15 bits */
    CHECK_EQ(tc_tx_segments_to_symbols(bad, n, w, 20), 0);
    memcpy(bad, seg, sizeof bad);
    bad[0].duration_us = 7001;                                        /* mark plus long qu'un depart */
    CHECK_EQ(tc_tx_segments_to_symbols(bad, n, w, 20), 0);
    CHECK_EQ(tc_tx_segments_to_symbols(seg, TC_MAX_SEGMENTS + 1, w, 100), 0);
    /* nombre pair : se termine deja par un space, pas de relachement ajoute */
    CHECK_EQ(tc_tx_segments_to_symbols(seg, 18, w, 20), 9);
    CHECK_EQ(w_d1(w[8]), seg[17].duration_us);
    CHECK_EQ(w_l1(w[8]), 0);
}

int main(void)
{
    RUN(test_open_raw);
    RUN(test_ok_first);
    RUN(test_blocked_first);
    RUN(test_refused_types);
    RUN(test_too_soon);
    RUN(test_rate_limit);
    RUN(test_rate_limit_burst);
    RUN(test_bus_busy);
    RUN(test_record);
    RUN(test_verdict_names);
    RUN(test_symbols_open_long);
    RUN(test_symbols_open_short);
    RUN(test_symbols_rejects);
    T_END();
}
