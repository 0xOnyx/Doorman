#include "tc_proto.h"
#include <string.h>

_Static_assert(TC_SEGMENTS_LONG == 3 + 32 && TC_SEGMENTS_SHORT == 3 + 16 && TC_SEGMENTS_ACK == 3 + 4,
               "nombre de segments = START + LEN + donnees + CHK");
_Static_assert(TC_MAX_SEGMENTS >= TC_SEGMENTS_LONG, "tampon de segments trop petit");

uint8_t tc_proto_parity(uint32_t raw, unsigned nbits)
{
    uint8_t p = 1;
    if (nbits > 32) {
        nbits = 32;        /* raw >> 32 serait indefini */
    }
    for (unsigned i = 0; i < nbits; i++) {
        p ^= (uint8_t)((raw >> i) & 1u);
    }
    return p;
}

static unsigned data_bits(bool is_long, bool is_ack)
{
    return is_ack ? 4u : (is_long ? 32u : 16u);
}

size_t tc_proto_encode(uint32_t raw, bool is_long, bool is_ack, tc_segment_t *out, size_t max)
{
    if (out == NULL || (is_ack && is_long)) {
        return 0;
    }
    if (is_ack && raw > 0xF) {
        return 0;
    }
    if (!is_long && !is_ack && raw > 0xFFFF) {
        return 0;
    }
    unsigned n = data_bits(is_long, is_ack);
    size_t total = 3 + n;
    if (max < total) {
        return 0;
    }
    size_t k = 0;
    out[k].duration_us = is_ack ? TC_START_ACK_TX_US : TC_START_CMD_TX_US;
    out[k].mark = true;
    k++;
    out[k].duration_us = is_long ? TC_BIT1_TX_US : TC_BIT0_TX_US;
    out[k].mark = false;
    k++;
    for (int i = (int)n - 1; i >= 0; i--) {
        bool bit = ((raw >> i) & 1u) != 0;
        out[k].duration_us = bit ? TC_BIT1_TX_US : TC_BIT0_TX_US;
        out[k].mark = (i & 1) != 0;       /* i impair = mark, pair = space */
        k++;
    }
    out[k].duration_us = tc_proto_parity(raw, n) ? TC_BIT1_TX_US : TC_BIT0_TX_US;
    out[k].mark = true;
    k++;
    return k;
}

static const char *const s_type_names[TC_TYPE_COUNT] = {
    [TC_TYPE_UNKNOWN] = "UNKNOWN",
    [TC_TYPE_ACK] = "ACK",
    [TC_TYPE_DOOR_CALL] = "DOOR_CALL",
    [TC_TYPE_INTERNAL_CALL] = "INTERNAL_CALL",
    [TC_TYPE_FLOOR_CALL] = "FLOOR_CALL",
    [TC_TYPE_START_TALKING] = "START_TALKING",
    [TC_TYPE_STOP_TALKING] = "STOP_TALKING",
    [TC_TYPE_OPEN_DOOR] = "OPEN_DOOR",
    [TC_TYPE_OPEN_DOOR_LONG] = "OPEN_DOOR_LONG",
    [TC_TYPE_LIGHT] = "LIGHT",
    [TC_TYPE_CONTROL_FUNCTION] = "CONTROL_FUNCTION",
    [TC_TYPE_DOOR_OPENED] = "DOOR_OPENED",
    [TC_TYPE_DOOR_CLOSED] = "DOOR_CLOSED",
    [TC_TYPE_END_OF_RINGTONE] = "END_OF_RINGTONE",
    [TC_TYPE_END_OF_DOOR_READINESS] = "END_OF_DOOR_READINESS",
    [TC_TYPE_INIT_DOOR_STATION] = "INIT_DOOR_STATION",
    [TC_TYPE_PROGRAMMING_MODE] = "PROGRAMMING_MODE",
    [TC_TYPE_READ_MEMORY_BLOCK] = "READ_MEMORY_BLOCK",
    [TC_TYPE_WRITE_MEMORY] = "WRITE_MEMORY",
    [TC_TYPE_SELECT_MEMORY_PAGE] = "SELECT_MEMORY_PAGE",
    [TC_TYPE_RESET] = "RESET",
    [TC_TYPE_SEARCH_DEVICES] = "SEARCH_DEVICES",
    [TC_TYPE_REQUEST_VERSION] = "REQUEST_VERSION",
};

const char *tc_type_name(tc_type_t t)
{
    if ((unsigned)t >= TC_TYPE_COUNT || s_type_names[t] == NULL) {
        return "?";
    }
    return s_type_names[t];
}

const char *tc_decode_error_name(tc_decode_error_t e)
{
    switch (e) {
    case TC_ERR_NONE:    return "none";
    case TC_ERR_LENGTH:  return "length";
    case TC_ERR_START:   return "start";
    case TC_ERR_LEN_BIT: return "len_bit";
    case TC_ERR_TIMING:  return "timing";
    case TC_ERR_PARITY:  return "parity";
    default:             return "?";
    }
}

/* --- decodage, parasites, repos de fin --- */
size_t tc_proto_merge_glitches(tc_segment_t *segs, size_t n, uint16_t glitch_us)
{
    if (segs == NULL) {
        return 0;
    }
    for (;;) {
        /* le plus court d'abord : un parasite pres d'un front laisse un residu lui aussi
           sous le seuil ; fondre le vrai parasite en premier remet le residu au bon bit */
        size_t i = n;
        for (size_t k = 0; k < n; k++) {
            if (segs[k].duration_us < glitch_us && (i == n || segs[k].duration_us < segs[i].duration_us)) {
                i = k;
            }
        }
        if (i == n) {
            return n;
        }
        if (i > 0 && i + 1 < n) {
            /* fondre i-1, i, i+1 dans i-1 (les voisins ont le meme niveau) */
            uint32_t sum = (uint32_t)segs[i - 1].duration_us + segs[i].duration_us + segs[i + 1].duration_us;
            segs[i - 1].duration_us = (sum > 0xFFFF) ? 0xFFFF : (uint16_t)sum;
            memmove(&segs[i], &segs[i + 2], (n - i - 2) * sizeof(*segs));
            n -= 2;
        } else {
            /* en tete ou en queue : simple suppression */
            memmove(&segs[i], &segs[i + 1], (n - i - 1) * sizeof(*segs));
            n -= 1;
        }
    }
}

size_t tc_proto_strip_trailing_idle(tc_segment_t *segs, size_t n)
{
    while (n > 0 && segs[n - 1].duration_us > TC_START_ACK_MAX_US) {
        n--;
    }
    return n;
}

static int classify_bit(uint16_t d)
{
    if (d >= TC_BIT0_MIN_US && d <= TC_BIT0_MAX_US) {
        return 0;
    }
    if (d >= TC_BIT1_MIN_US && d <= TC_BIT1_MAX_US) {
        return 1;
    }
    return -1;
}

bool tc_proto_decode(const tc_segment_t *segs, size_t n, tc_telegram_t *out, tc_decode_error_t *err)
{
    tc_decode_error_t e = TC_ERR_NONE;
    tc_telegram_t t;
    unsigned nbits = 0;
    uint32_t raw = 0;
    memset(&t, 0, sizeof t);

    if (segs == NULL) {
        e = TC_ERR_LENGTH;
        goto done;
    }
    if (n == TC_SEGMENTS_ACK) {
        nbits = 4;
    } else if (n == TC_SEGMENTS_SHORT) {
        nbits = 16;
    } else if (n == TC_SEGMENTS_LONG) {
        nbits = 32;
    } else {
        e = TC_ERR_LENGTH;
        goto done;
    }

    {
        uint16_t s0 = segs[0].duration_us;
        if (s0 >= TC_START_CMD_MIN_US && s0 <= TC_START_CMD_MAX_US) {
            t.is_response = false;
        } else if (s0 >= TC_START_ACK_MIN_US && s0 <= TC_START_ACK_MAX_US) {
            t.is_response = true;
        } else {
            e = TC_ERR_START;
            goto done;
        }
    }

    {
        int len = classify_bit(segs[1].duration_us);
        if (len < 0) {
            e = TC_ERR_TIMING;
            goto done;
        }
        if ((nbits == 32) != (len == 1)) {
            e = TC_ERR_LEN_BIT;
            goto done;
        }
        t.is_long = (nbits == 32);
    }

    for (unsigned i = 0; i < nbits; i++) {
        int b = classify_bit(segs[2 + i].duration_us);
        if (b < 0) {
            e = TC_ERR_TIMING;
            goto done;
        }
        raw = (raw << 1) | (uint32_t)b;
    }

    {
        int chk = classify_bit(segs[n - 1].duration_us);
        if (chk < 0) {
            e = TC_ERR_TIMING;
            goto done;
        }
        if ((uint8_t)chk != tc_proto_parity(raw, nbits)) {
            e = TC_ERR_PARITY;
            goto done;
        }
    }
    t.raw = raw;

done:
    if (err) {
        *err = e;
    }
    if (e != TC_ERR_NONE) {
        return false;
    }
    if (out) {
        *out = t;
    }
    return true;
}

/* --- classification et liste blanche : tache 6 (stubs provisoires) --- */
void tc_proto_classify(tc_telegram_t *t) { (void)t; }
bool tc_proto_tx_allowed(const tc_telegram_t *t) { (void)t; return false; }
