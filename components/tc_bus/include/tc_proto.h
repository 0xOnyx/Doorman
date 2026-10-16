#ifndef TC_PROTO_H
#define TC_PROTO_H
/* Protocole TCS/Koch TC:Bus : logique pure, aucune dependance ESP-IDF (spec 11.1). */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Durees d'emission (us) */
#define TC_START_CMD_TX_US   5985
#define TC_START_ACK_TX_US   6200
#define TC_BIT1_TX_US        4000
#define TC_BIT0_TX_US        2000

/* Fenetres de reception (us) */
#define TC_BIT0_MIN_US       1000
#define TC_BIT0_MAX_US       2999
#define TC_BIT1_MIN_US       3000
#define TC_BIT1_MAX_US       4999
#define TC_START_CMD_MIN_US  5700
#define TC_START_CMD_MAX_US  6089
#define TC_START_ACK_MIN_US  6090
#define TC_START_ACK_MAX_US  6900

#define TC_MAX_SEGMENTS      96
#define TC_SEGMENTS_ACK      7     /* START LEN b3..b0 CHK */
#define TC_SEGMENTS_SHORT    19    /* START LEN 16 bits CHK */
#define TC_SEGMENTS_LONG     35    /* START LEN 32 bits CHK */

typedef enum {
    TC_TYPE_UNKNOWN = 0,
    TC_TYPE_ACK,
    TC_TYPE_DOOR_CALL,
    TC_TYPE_INTERNAL_CALL,
    TC_TYPE_FLOOR_CALL,
    TC_TYPE_START_TALKING,
    TC_TYPE_STOP_TALKING,
    TC_TYPE_OPEN_DOOR,
    TC_TYPE_OPEN_DOOR_LONG,
    TC_TYPE_LIGHT,
    TC_TYPE_CONTROL_FUNCTION,
    TC_TYPE_DOOR_OPENED,
    TC_TYPE_DOOR_CLOSED,
    TC_TYPE_END_OF_RINGTONE,
    TC_TYPE_END_OF_DOOR_READINESS,
    TC_TYPE_INIT_DOOR_STATION,
    TC_TYPE_PROGRAMMING_MODE,
    TC_TYPE_READ_MEMORY_BLOCK,
    TC_TYPE_WRITE_MEMORY,
    TC_TYPE_SELECT_MEMORY_PAGE,
    TC_TYPE_RESET,
    TC_TYPE_SEARCH_DEVICES,
    TC_TYPE_REQUEST_VERSION,
    TC_TYPE_COUNT
} tc_type_t;

typedef struct {
    uint32_t  raw;
    bool      is_long;      /* 32 bits ; false = 16 bits ou ACK 4 bits */
    bool      is_response;  /* depart 6090..6900 us */
    tc_type_t type;
    uint32_t  serial;       /* 20 bits, 0 si absent */
    uint8_t   address;      /* 6 bits, 0 si absent */
    uint32_t  payload;
} tc_telegram_t;

/* Un segment = une duree a un niveau. mark = bus tire a 0 V (TX GPIO = 1, RX GPIO = 0). */
typedef struct {
    uint16_t duration_us;
    bool     mark;
} tc_segment_t;

typedef enum {
    TC_ERR_NONE = 0,
    TC_ERR_LENGTH,    /* nombre de segments != 7, 19, 35 */
    TC_ERR_START,     /* premier segment hors fenetres de depart */
    TC_ERR_LEN_BIT,   /* bit LEN incoherent avec le nombre de segments */
    TC_ERR_TIMING,    /* un segment hors fenetres bit 0 / bit 1 */
    TC_ERR_PARITY
} tc_decode_error_t;

/* CHK = 1 XOR b(nbits-1) XOR ... XOR b0. nbits <= 32 (borne a 32 au-dela). */
uint8_t tc_proto_parity(uint32_t raw, unsigned nbits);

/* Construit la sequence de segments de la spec 4.3. Renvoie le nombre de segments,
 * 0 si refuse (ACK > 0xF, court > 0xFFFF, is_ack && is_long, tampon trop petit).
 * La sequence commence et finit par un mark (nombre impair de segments) : l'emetteur
 * doit relacher le bus (space) apres le CHK ; avec le RMT c'est le niveau de repos
 * du canal, pas un segment supplementaire. Les paires (2k, 2k+1) se projettent
 * directement sur rmt_symbol_word_t, la derniere moitie restant a duree 0. */
size_t tc_proto_encode(uint32_t raw, bool is_long, bool is_ack, tc_segment_t *out, size_t max);

/* Supprime les segments < glitch_us en les fondant dans leurs voisins (le plus court d'abord).
 * Renvoie le nouveau nombre. A appeler AVANT tc_proto_strip_trailing_idle : un parasite
 * pendant le repos de fin doit etre fondu dans ce repos avant qu'il ne soit retire. */
size_t tc_proto_merge_glitches(tc_segment_t *segs, size_t n, uint16_t glitch_us);

/* Supprime les segments de fin > TC_START_ACK_MAX_US (repos enregistre par le RMT). */
size_t tc_proto_strip_trailing_idle(tc_segment_t *segs, size_t n);

/* Decode une trame complete. true si valide ; *out rempli (raw, is_long, is_response), le reste a 0. */
bool tc_proto_decode(const tc_segment_t *segs, size_t n, tc_telegram_t *out, tc_decode_error_t *err);

/* Remplit type, serial, address, payload a partir de raw / is_long / is_response. */
void tc_proto_classify(tc_telegram_t *t);

/* Liste blanche d'emission : OPEN_DOOR, OPEN_DOOR_LONG, LIGHT, et rien d'autre. */
bool tc_proto_tx_allowed(const tc_telegram_t *t);

/* Le telegramme concerne-t-il la station interieure my_serial ? Faux si my_serial == 0.
 * Vrai pour DOOR_CALL, INTERNAL_CALL, FLOOR_CALL, START_TALKING, CONTROL_FUNCTION, OPEN_DOOR_LONG
 * et REQUEST_VERSION dont serial == my_serial ; pour un OPEN_DOOR court dont address == door_address
 * seulement si recent_call_for_me (un appel pour moi vient d'etre vu : l'appelant gere la fenetre),
 * car un OPEN_DOOR court ne porte pas de numero de serie. */
bool tc_proto_is_for_me(const tc_telegram_t *t, uint32_t my_serial, uint8_t door_address, bool recent_call_for_me);

/* Sonnette HomeKit : quelqu'un sonne-t-il chez my_serial ? Predicat strict, distinct de
 * tc_proto_is_for_me (repere de log, volontairement large). Vrai seulement pour DOOR_CALL (platine
 * de rue) et FLOOR_CALL (sonnette paliere) qui ne sont pas des reponses, dont serial == my_serial,
 * avec my_serial != 0. Jamais pour INTERNAL_CALL (sens du numero non verifie), START_TALKING,
 * CONTROL_FUNCTION, OPEN_DOOR_LONG, REQUEST_VERSION (nos propres actions portent notre numero).
 * L'echo de nos emissions n'est pas transmis au rappel de tc_bus et ne passe donc jamais ici. */
bool tc_proto_is_ring_for_me(const tc_telegram_t *t, uint32_t my_serial);

/* Convertit des mots rmt_symbol_word_t (passes en uint32_t : duration0 bits 0-14, level0 bit 15,
 * duration1 bits 16-30, level1 bit 31) en segments. Une duree nulle termine la trame ;
 * mark = (level == 0) (bus bas = GPIO RX a 0). Ecrit au plus max segments, renvoie leur nombre. */
size_t tc_proto_rmt_words_to_segments(const uint32_t *words, size_t nwords, tc_segment_t *out, size_t max);

/* Cherche la prochaine trame dans segs[*pos..n) : premier segment dans une fenetre de depart,
 * longueur deduite (ACK 7, court 19, long 35 selon le depart et le bit LEN). Renvoie true et
 * remplit *start / *len si une trame complete est disponible, et avance *pos apres elle ;
 * sinon *pos = n. Sert a separer des telegrammes enchaines sans repos de 7 ms (ex. ACK). */
bool tc_proto_next_frame(const tc_segment_t *segs, size_t n, size_t *pos, size_t *start, size_t *len);

const char *tc_type_name(tc_type_t t);
const char *tc_decode_error_name(tc_decode_error_t e);

#endif
