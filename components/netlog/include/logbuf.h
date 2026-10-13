#ifndef LOGBUF_H
#define LOGBUF_H
#include <stdint.h>
#include <stddef.h>

/* Tampon circulaire d'octets, logique pure (aucune dependance ESP-IDF).
 * L'appelant assure l'exclusion mutuelle entre write et read. */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t head;     /* index de la prochaine ecriture */
    size_t len;      /* octets valides (<= cap) */
    uint64_t total;  /* octets ecrits depuis init : curseur absolu */
} logbuf_t;

void logbuf_init(logbuf_t *lb, uint8_t *storage, size_t cap);

/* Ecrit n octets, ecrase les plus anciens si necessaire. */
void logbuf_write(logbuf_t *lb, const void *data, size_t n);

/* Copie au plus n octets a partir de la position absolue *cursor et avance *cursor.
 * Si *cursor est plus ancien que la fenetre disponible, il saute au plus ancien
 * et *lost (si non NULL) recoit le nombre d'octets sautes, sinon 0.
 * Renvoie le nombre d'octets copies. */
size_t logbuf_read(const logbuf_t *lb, uint64_t *cursor, void *out, size_t n, uint64_t *lost);

uint64_t logbuf_total(const logbuf_t *lb);

#endif
