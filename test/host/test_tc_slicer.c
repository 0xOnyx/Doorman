#include "t.h"
#include "tc_slicer.h"
#include <string.h>

/* Signal synthetique : repos 2950 mV, bas 860 mV (mesures sur la broche RX), rampe lineaire
 * sur chaque front, bruit pseudo-aleatoire, conversion lineaire 3100 mV pleine echelle. */
#define IDLE_MV   2950
#define LOW_MV    860
#define PERIOD_NS 50000u

static uint16_t mv_to_raw(int mv)
{
    if (mv < 0) mv = 0;
    if (mv > 3100) mv = 3100;
    return (uint16_t)(mv * 4095 / 3100);
}

static uint32_t s_rng = 12345;
static int noise(int amp)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return amp ? (int)((s_rng >> 16) % (2u * amp + 1)) - amp : 0;
}

/* Fronts : instants (us) ou le bus change de niveau ; edge_low[i] = niveau apres le front i. */
#define MAX_EDGES 256
static double s_edge_t[MAX_EDGES];
static bool   s_edge_low[MAX_EDGES];
static size_t s_nedges;

static void add_segments(double *t, const tc_segment_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s_edge_t[s_nedges] = *t;
        s_edge_low[s_nedges] = s[i].mark;
        s_nedges++;
        *t += s[i].duration_us;
    }
    s_edge_t[s_nedges] = *t;               /* relache du bus apres le dernier mark */
    s_edge_low[s_nedges] = false;
    s_nedges++;
}

/* Niveau (mV) a l'instant t : rampe de ramp_us apres chaque front. */
static int level_at(double t, double ramp_us)
{
    int mv = IDLE_MV;
    int prev = IDLE_MV;
    for (size_t i = 0; i < s_nedges && s_edge_t[i] <= t; i++) {
        int target = s_edge_low[i] ? LOW_MV : IDLE_MV;
        double dt = t - s_edge_t[i];
        mv = dt >= ramp_us ? target : (int)(prev + (target - prev) * dt / ramp_us);
        prev = target;
    }
    return mv;
}

typedef struct {
    int frames;                 /* trames rendues par le slicer */
    int decoded;
    tc_telegram_t t[4];
    int errors;
} result_t;

static void handle_frame(tc_slicer_t *sl, result_t *r)
{
    r->frames++;
    tc_segment_t segs[TC_MAX_SEGMENTS];
    memcpy(segs, sl->segs, sl->n * sizeof segs[0]);
    size_t k = tc_proto_merge_glitches(segs, sl->n, 1000);
    k = tc_proto_strip_trailing_idle(segs, k);
    size_t pos = 0, st, len;
    while (tc_proto_next_frame(segs, k, &pos, &st, &len)) {
        tc_telegram_t t;
        tc_decode_error_t err;
        if (tc_proto_decode(&segs[st], len, &t, &err)) {
            tc_proto_classify(&t);
            if (r->decoded < 4) {
                r->t[r->decoded] = t;
            }
            r->decoded++;
        } else {
            r->errors++;
        }
    }
}

static result_t run_stream(double total_us, double phase_us, double ramp_us, int noise_mv, uint32_t period_ns)
{
    tc_slicer_t sl;
    result_t r;
    memset(&r, 0, sizeof r);
    tc_slicer_init(&sl, mv_to_raw(1700), mv_to_raw(2100), period_ns);
    for (double t = phase_us; t < total_us; t += period_ns / 1000.0) {
        if (tc_slicer_push(&sl, mv_to_raw(level_at(t, ramp_us) + noise(noise_mv)))) {
            handle_frame(&sl, &r);
        }
    }
    return r;
}

static double build_real_capture(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    double t = 20000;                      /* 20 ms de repos */
    s_nedges = 0;
    size_t n = tc_proto_encode(0x14DEF480, true, false, s, TC_MAX_SEGMENTS);
    CHECK_EQ(n, 35);
    add_segments(&t, s, n);
    t += 3000;                             /* 3 ms de repos entre les deux telegrammes */
    n = tc_proto_encode(0x0001, false, false, s, TC_MAX_SEGMENTS);
    CHECK_EQ(n, 19);
    add_segments(&t, s, n);
    return t + 30000;                      /* repos final */
}

static void check_real(result_t r)
{
    CHECK_EQ(r.frames, 1);
    CHECK_EQ(r.errors, 0);
    CHECK_EQ(r.decoded, 2);
    if (r.decoded >= 2) {
        CHECK_EQ(r.t[0].raw, 0x14DEF480);
        CHECK(r.t[0].is_long);
        CHECK_EQ(r.t[0].type, TC_TYPE_OPEN_DOOR_LONG);
        CHECK_EQ(r.t[0].serial, 0x4DEF4);
        CHECK_EQ(r.t[0].address, 0);
        CHECK_EQ(r.t[1].raw, 0x0001);
        CHECK(!r.t[1].is_long);
    }
}

static void test_real_telegram(void)
{
    double total = build_real_capture();
    /* plusieurs dephasages echantillonnage / signal, rampes de 2 a 4 echantillons, bruit +-60 mV */
    static const double phases[] = { 0, 7, 19, 33, 49 };
    static const double ramps[] = { 100, 150, 200 };
    for (size_t i = 0; i < sizeof phases / sizeof phases[0]; i++) {
        for (size_t j = 0; j < sizeof ramps / sizeof ramps[0]; j++) {
            check_real(run_stream(total, phases[i], ramps[j], 60, PERIOD_NS));
        }
    }
}

static void test_real_telegram_rate_error(void)
{
    /* cadence reelle 2 % plus lente que la cadence supposee : les durees s'allongent de 2 %,
       un depart de 5985 us reste dans 5700..6089 ; avec la cadence corrigee tout passe */
    double total = build_real_capture();
    tc_slicer_t sl;
    result_t r;
    memset(&r, 0, sizeof r);
    tc_slicer_init(&sl, mv_to_raw(1700), mv_to_raw(2100), PERIOD_NS);
    tc_slicer_set_period(&sl, 51000);
    CHECK_EQ(sl.period_ns, 51000);
    tc_slicer_set_period(&sl, 0);          /* ignore */
    CHECK_EQ(sl.period_ns, 51000);
    for (double t = 3; t < total; t += 51.0) {
        if (tc_slicer_push(&sl, mv_to_raw(level_at(t, 150) + noise(40)))) {
            handle_frame(&sl, &r);
        }
    }
    check_real(r);
}

static void test_noise_only(void)
{
    s_nedges = 0;                          /* aucun front : repos seul */
    tc_slicer_t sl;
    tc_slicer_init(&sl, mv_to_raw(1700), mv_to_raw(2100), PERIOD_NS);
    int frames = 0;
    for (int i = 0; i < 200000; i++) {     /* 10 s */
        frames += tc_slicer_push(&sl, mv_to_raw(IDLE_MV + noise(300)));
    }
    CHECK_EQ(frames, 0);
    CHECK_EQ(tc_slicer_take_edges(&sl), 0);
    CHECK_EQ(sl.n, 0);
}

static void test_dip_above_threshold(void)
{
    /* creux a 1800 mV pendant 4 ms, jamais sous 1700 mV : rien */
    tc_slicer_t sl;
    tc_slicer_init(&sl, mv_to_raw(1700), mv_to_raw(2100), PERIOD_NS);
    int frames = 0;
    for (int i = 0; i < 2000; i++) {
        int mv = (i >= 400 && i < 480) ? 1800 + noise(50) : IDLE_MV + noise(50);
        frames += tc_slicer_push(&sl, mv_to_raw(mv));
    }
    CHECK_EQ(frames, 0);
    CHECK_EQ(tc_slicer_take_edges(&sl), 0);
}

static void test_hysteresis(void)
{
    /* un bas qui remonte entre 1700 et 2100 mV reste un seul mark */
    tc_slicer_t sl;
    tc_slicer_init(&sl, mv_to_raw(1700), mv_to_raw(2100), PERIOD_NS);
    int frames = 0;
    for (int i = 0; i < 40; i++) frames += tc_slicer_push(&sl, mv_to_raw(IDLE_MV));
    for (int i = 0; i < 60; i++) frames += tc_slicer_push(&sl, mv_to_raw(i % 7 != 3 ? LOW_MV : 1950));  /* 3 ms */
    int i = 0;
    while (i < 200 && !tc_slicer_push(&sl, mv_to_raw(IDLE_MV))) {
        i++;
    }
    CHECK_EQ(frames, 0);
    CHECK_EQ(i, 140);                      /* fin de trame des que le repos depasse 7 ms */
    frames = 1;
    CHECK_EQ(sl.n, 1);
    CHECK(sl.segs[0].mark);
    CHECK_EQ(sl.segs[0].duration_us, 3000);
    CHECK_EQ(tc_slicer_take_edges(&sl), 2);
    /* l'appel suivant efface la trame, le repos qui continue n'en produit pas d'autre */
    for (int i = 0; i < 400; i++) frames += tc_slicer_push(&sl, mv_to_raw(IDLE_MV));
    CHECK_EQ(frames, 1);
    CHECK_EQ(sl.n, 0);
}

static void test_reset_and_overflow(void)
{
    tc_slicer_t sl;
    tc_slicer_init(&sl, 2000, 1000, PERIOD_NS);   /* high < low : ramene a low */
    CHECK_EQ(sl.high_raw, 2000);
    tc_slicer_init(&sl, mv_to_raw(1700), mv_to_raw(2100), PERIOD_NS);
    /* 200 fronts sans repos de 7 ms : au plus TC_MAX_SEGMENTS segments */
    for (int k = 0; k < 200; k++) {
        for (int i = 0; i < 40; i++) {
            tc_slicer_push(&sl, mv_to_raw(k % 2 ? IDLE_MV : LOW_MV));
        }
    }
    CHECK_EQ(sl.n, TC_MAX_SEGMENTS);
    tc_slicer_reset(&sl);
    CHECK_EQ(sl.n, 0);
    CHECK(!sl.low);
    /* bus bloque bas : jamais de trame, duree bornee a la sortie */
    int frames = 0;
    for (int i = 0; i < 30000; i++) frames += tc_slicer_push(&sl, mv_to_raw(LOW_MV));   /* 1.5 s */
    CHECK_EQ(frames, 0);
    for (int i = 0; i < 200; i++) frames += tc_slicer_push(&sl, mv_to_raw(IDLE_MV));
    CHECK_EQ(frames, 1);
    CHECK_EQ(sl.segs[0].duration_us, 0xFFFF);
}

int main(void)
{
    RUN(test_real_telegram);
    RUN(test_real_telegram_rate_error);
    RUN(test_noise_only);
    RUN(test_dip_above_threshold);
    RUN(test_hysteresis);
    RUN(test_reset_and_overflow);
    T_END();
}
