#ifndef TC_TX_POLICY_H
#define TC_TX_POLICY_H
/* Politique d'emission sur le bus (spec phase 2, section 2) : logique pure, aucune dependance
 * ESP-IDF (testee sur PC). Ne fait qu'autoriser ou refuser ; l'emetteur (tc_tx.c) applique. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tc_proto.h"

#define TC_TX_MIN_GAP_US      200000LL               /* entre deux emissions */
#define TC_TX_RATE_MAX        5                      /* emissions au plus ... */
#define TC_TX_RATE_WINDOW_US  (60LL * 1000000)       /* ... par minute glissante */
#define TC_TX_QUIET_US        200000LL               /* bus sans front depuis au moins ceci */

typedef struct {
    int64_t sent_us[TC_TX_RATE_MAX];   /* dernieres emissions, la plus ancienne d'abord */
    size_t  n;                         /* entrees valides de sent_us */
    int64_t last_us;                   /* derniere emission (sans objet si n == 0) */
} tc_tx_limiter_t;

typedef enum {
    TC_TX_OK = 0,
    TC_TX_REFUSED_TYPE,   /* hors liste blanche (tc_proto_tx_allowed) */
    TC_TX_TOO_SOON,       /* moins de 200 ms depuis la derniere emission */
    TC_TX_RATE_LIMIT,     /* deja 5 emissions dans la derniere minute */
    TC_TX_BUS_BUSY,       /* front sur le bus il y a moins de 200 ms */
    TC_TX_BLOCKED,        /* OTA en cours, SoftAP de secours, recepteur arrete */
} tc_tx_verdict_t;

/* Telegramme d'ouverture : forme longue (1 << 28) | serial << 8 | 0x80 | adresse (OPEN_DOOR_LONG),
 * forme courte 0x1100 | adresse (OPEN_DOOR). serial masque sur 20 bits, adresse sur 6 bits.
 * *is_long (facultatif) recoit la forme. */
uint32_t tc_tx_open_raw(bool long_form, uint32_t serial, uint8_t address, bool *is_long);

/* Limiteur vide (aucune emission). */
void tc_tx_limiter_init(tc_tx_limiter_t *l);

/* Verdict pour emettre t a now_us, le dernier front du bus datant de bus_quiet_since_us.
 * Regles dans l'ordre : bloque -> BLOCKED ; hors liste blanche (t reclasse, t NULL) -> REFUSED_TYPE ;
 * < 200 ms depuis la derniere emission -> TOO_SOON ; 5 emissions dans les 60 s precedentes ->
 * RATE_LIMIT ; front depuis < 200 ms -> BUS_BUSY. Une date dans le futur (horloges lues dans le
 * desordre) compte comme "trop recent". */
tc_tx_verdict_t tc_tx_check(const tc_tx_limiter_t *l, const tc_telegram_t *t, int64_t now_us,
                            int64_t bus_quiet_since_us, bool blocked);

/* Enregistre une emission a now_us (garde les 5 plus recentes). */
void tc_tx_record(tc_tx_limiter_t *l, int64_t now_us);

/* Symboles RMT d'emission : mots de 32 bits au format rmt_symbol_word_t (duration0 bits 0-14,
 * level0 bit 15, duration1 bits 16-30, level1 bit 31), 1 tick = 1 us. Mark = niveau 1 (TX GPIO a 1,
 * transistor passant, bus tire a 0 V). */
#define TC_TX_RELEASE_US   100        /* relachement explicite (niveau 0) apres le dernier mark */
#define TC_TX_MAX_SYMBOLS  ((TC_MAX_SEGMENTS + 1) / 2)

/* Convertit des segments de tc_proto_encode en symboles : paires (mark, space), chaque symbole
 * finit a 0 ; un nombre impair de segments est complete par TC_TX_RELEASE_US a 0. Refuse (renvoie 0)
 * si la suite ne commence pas par un mark, n'alterne pas, contient une duree nulle (fin RMT) ou
 * superieure a TC_START_ACK_MAX_US, ou ne tient pas dans max mots. Sinon renvoie le nombre de mots. */
size_t tc_tx_segments_to_symbols(const tc_segment_t *segs, size_t n, uint32_t *words, size_t max);

/* "ok", "refused_type", "too_soon", "rate_limit", "bus_busy", "blocked" ; "?" sinon. */
const char *tc_tx_verdict_name(tc_tx_verdict_t v);

#endif
