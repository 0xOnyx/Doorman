#include "tc_slicer.h"
#include <string.h>

#define RUN_MAX 0x00FFFFFFu                /* borne le compteur (plus de 10 min a 20 kHz) */

void tc_slicer_init(tc_slicer_t *s, uint16_t low_raw, uint16_t high_raw, uint32_t period_ns)
{
    memset(s, 0, sizeof *s);
    s->low_raw = low_raw;
    s->high_raw = high_raw < low_raw ? low_raw : high_raw;
    s->period_ns = period_ns ? period_ns : 1;
}

void tc_slicer_reset(tc_slicer_t *s)
{
    s->low = false;
    s->ready = false;
    s->run = 0;
    s->n = 0;
}

void tc_slicer_set_period(tc_slicer_t *s, uint32_t period_ns)
{
    if (period_ns) {
        s->period_ns = period_ns;
    }
}

static uint16_t run_us(const tc_slicer_t *s)
{
    uint64_t us = ((uint64_t)s->run * s->period_ns + 500) / 1000;
    return us > 0xFFFF ? 0xFFFF : (uint16_t)us;
}

bool tc_slicer_push(tc_slicer_t *s, uint16_t raw)
{
    if (s->ready) {                        /* la trame precedente a ete lue */
        s->ready = false;
        s->n = 0;
    }
    bool new_low = s->low ? (raw < s->high_raw) : (raw < s->low_raw);
    if (new_low != s->low) {
        s->edges++;
        if (s->low || s->n > 0) {          /* au repos sans segment : le premier segment est un mark */
            if (s->n < TC_MAX_SEGMENTS) {
                s->segs[s->n].duration_us = run_us(s);
                s->segs[s->n].mark = s->low;   /* le segment qui se termine */
                s->n++;
            }
        }
        s->low = new_low;
        s->run = 1;                        /* cet echantillon appartient deja au nouveau niveau */
        return false;
    }
    if (s->run < RUN_MAX) {
        s->run++;
    }
    if (!s->low && s->n > 0 && (uint64_t)s->run * s->period_ns > (uint64_t)TC_SLICER_FRAME_END_US * 1000) {
        s->ready = true;                   /* repos > 7 ms : trame terminee */
        return true;
    }
    return false;
}

uint32_t tc_slicer_take_edges(tc_slicer_t *s)
{
    uint32_t e = s->edges;
    s->edges = 0;
    return e;
}
