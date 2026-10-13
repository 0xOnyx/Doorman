#include "tc_proto.h"
#include <string.h>

uint8_t tc_proto_parity(uint32_t raw, unsigned nbits)
{
    uint8_t p = 1;
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

/* --- decodage, parasites, classification : taches 5 et 6 (stubs provisoires) --- */
size_t tc_proto_merge_glitches(tc_segment_t *segs, size_t n, uint16_t glitch_us) { (void)segs; (void)glitch_us; return n; }
size_t tc_proto_strip_trailing_idle(tc_segment_t *segs, size_t n) { (void)segs; return n; }
bool tc_proto_decode(const tc_segment_t *segs, size_t n, tc_telegram_t *out, tc_decode_error_t *err) { (void)segs; (void)n; (void)out; if (err) *err = TC_ERR_LENGTH; return false; }
void tc_proto_classify(tc_telegram_t *t) { (void)t; }
bool tc_proto_tx_allowed(const tc_telegram_t *t) { (void)t; return false; }
