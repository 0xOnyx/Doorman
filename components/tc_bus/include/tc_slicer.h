#ifndef TC_SLICER_H
#define TC_SLICER_H
/* Decoupage d'un signal echantillonne (ADC) en segments tc_segment_t : seuils avec hysteresis,
 * durees comptees en echantillons, fin de trame apres un repos prolonge. Logique pure, aucune
 * dependance ESP-IDF (testee sur PC). */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tc_proto.h"

#define TC_SLICER_FRAME_END_US  7000   /* repos plus long : fin de trame */

typedef struct {
    /* configuration */
    uint16_t low_raw;          /* au repos (haut), passe a "bas" (mark) si echantillon < low_raw */
    uint16_t high_raw;         /* en mark, repasse a "haut" si echantillon >= high_raw */
    uint32_t period_ns;        /* duree d'un echantillon (50000 ns a 20 kHz) */
    /* etat */
    bool     low;              /* niveau courant apres hysteresis (true = mark) */
    bool     ready;            /* une trame est disponible dans segs[0..n) */
    uint32_t run;              /* echantillons au niveau courant */
    uint32_t edges;            /* fronts vus depuis tc_slicer_take_edges() (activite) */
    size_t   n;
    tc_segment_t segs[TC_MAX_SEGMENTS];
} tc_slicer_t;

/* low_raw <= high_raw (sinon high_raw = low_raw). period_ns > 0. Etat initial : repos, aucun segment. */
void tc_slicer_init(tc_slicer_t *s, uint16_t low_raw, uint16_t high_raw, uint32_t period_ns);

/* Oublie la trame en cours (trou dans les echantillons) et repart au repos. Garde la configuration. */
void tc_slicer_reset(tc_slicer_t *s);

/* Change la duree d'un echantillon (cadence mesuree) ; ignore 0. */
void tc_slicer_set_period(tc_slicer_t *s, uint32_t period_ns);

/* Ajoute un echantillon. Renvoie true quand une trame vient de se terminer (repos > 7 ms apres au
 * moins un segment) : ses segments sont dans s->segs[0..s->n), commencent par un mark et ne
 * contiennent pas le repos final. Ils restent valides jusqu'a l'appel suivant, qui les efface.
 * Les segments au-dela de TC_MAX_SEGMENTS sont perdus ; une duree > 65535 us est bornee. */
bool tc_slicer_push(tc_slicer_t *s, uint16_t raw);

/* Nombre de fronts vus depuis l'appel precedent (remis a 0). */
uint32_t tc_slicer_take_edges(tc_slicer_t *s);

#endif
