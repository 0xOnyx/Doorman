#include "tc_tx_policy.h"
#include <string.h>

uint32_t tc_tx_open_raw(bool long_form, uint32_t serial, uint8_t address, bool *is_long)
{
    uint32_t a = (uint32_t)address & 0x3Fu;
    if (is_long) {
        *is_long = long_form;
    }
    if (long_form) {
        return (1u << 28) | ((serial & 0xFFFFFu) << 8) | 0x80u | a;
    }
    return 0x1100u | a;
}

void tc_tx_limiter_init(tc_tx_limiter_t *l)
{
    memset(l, 0, sizeof *l);
}

tc_tx_verdict_t tc_tx_check(const tc_tx_limiter_t *l, const tc_telegram_t *t, int64_t now_us,
                            int64_t bus_quiet_since_us, bool blocked)
{
    if (blocked) {
        return TC_TX_BLOCKED;
    }
    if (l == NULL || !tc_proto_tx_allowed(t)) {
        return TC_TX_REFUSED_TYPE;
    }
    if (l->n > 0 && now_us - l->last_us < TC_TX_MIN_GAP_US) {
        return TC_TX_TOO_SOON;
    }
    size_t recent = 0;
    for (size_t i = 0; i < l->n && i < TC_TX_RATE_MAX; i++) {
        if (now_us - l->sent_us[i] < TC_TX_RATE_WINDOW_US) {
            recent++;
        }
    }
    if (recent >= TC_TX_RATE_MAX) {
        return TC_TX_RATE_LIMIT;
    }
    if (now_us - bus_quiet_since_us < TC_TX_QUIET_US) {
        return TC_TX_BUS_BUSY;
    }
    return TC_TX_OK;
}

void tc_tx_record(tc_tx_limiter_t *l, int64_t now_us)
{
    if (l->n >= TC_TX_RATE_MAX) {
        memmove(&l->sent_us[0], &l->sent_us[1], (TC_TX_RATE_MAX - 1) * sizeof l->sent_us[0]);
        l->n = TC_TX_RATE_MAX - 1;
    }
    l->sent_us[l->n++] = now_us;
    l->last_us = now_us;
}

size_t tc_tx_segments_to_symbols(const tc_segment_t *segs, size_t n, uint32_t *words, size_t max)
{
    if (segs == NULL || words == NULL || n == 0 || n > TC_MAX_SEGMENTS) {
        return 0;
    }
    size_t nsym = (n + 1) / 2;
    if (nsym > max) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (segs[i].mark != (i % 2 == 0) || segs[i].duration_us == 0 ||
            segs[i].duration_us > TC_START_ACK_MAX_US) {
            return 0;
        }
    }
    for (size_t k = 0; k < nsym; k++) {
        uint32_t d0 = segs[2 * k].duration_us;
        uint32_t d1 = (2 * k + 1 < n) ? segs[2 * k + 1].duration_us : TC_TX_RELEASE_US;
        /* level0 = 1 (mark), level1 = 0 (space) : chaque symbole rend le bus */
        words[k] = (d0 & 0x7FFFu) | (1u << 15) | ((d1 & 0x7FFFu) << 16);
    }
    return nsym;
}

const char *tc_tx_verdict_name(tc_tx_verdict_t v)
{
    switch (v) {
    case TC_TX_OK:           return "ok";
    case TC_TX_REFUSED_TYPE: return "refused_type";
    case TC_TX_TOO_SOON:     return "too_soon";
    case TC_TX_RATE_LIMIT:   return "rate_limit";
    case TC_TX_BUS_BUSY:     return "bus_busy";
    case TC_TX_BLOCKED:      return "blocked";
    default:                 return "?";
    }
}
