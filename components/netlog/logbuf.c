#include "logbuf.h"
#include <string.h>

void logbuf_init(logbuf_t *lb, uint8_t *storage, size_t cap)
{
    lb->buf = storage;
    lb->cap = cap;
    lb->head = 0;
    lb->len = 0;
    lb->total = 0;
}

void logbuf_write(logbuf_t *lb, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    lb->total += n;

    if (n >= lb->cap) {
        /* seuls les cap derniers octets survivent */
        memcpy(lb->buf, p + (n - lb->cap), lb->cap);
        lb->head = 0;
        lb->len = lb->cap;
        return;
    }
    size_t first = lb->cap - lb->head;
    if (first > n) {
        first = n;
    }
    memcpy(lb->buf + lb->head, p, first);
    memcpy(lb->buf, p + first, n - first);
    lb->head = (lb->head + n) % lb->cap;
    lb->len = (lb->len + n > lb->cap) ? lb->cap : lb->len + n;
}

size_t logbuf_read(const logbuf_t *lb, uint64_t *cursor, void *out, size_t n, uint64_t *lost)
{
    uint64_t oldest = lb->total - lb->len;
    uint64_t skipped = 0;

    if (*cursor < oldest) {
        skipped = oldest - *cursor;
        *cursor = oldest;
    }
    if (*cursor > lb->total) {
        *cursor = lb->total;
    }
    if (lost) {
        *lost = skipped;
    }
    uint64_t avail = lb->total - *cursor;
    if (n > avail) {
        n = (size_t)avail;
    }
    if (n == 0) {
        return 0;
    }
    size_t oldest_idx = (lb->head + lb->cap - lb->len) % lb->cap;
    size_t start = (oldest_idx + (size_t)(*cursor - oldest)) % lb->cap;
    size_t first = lb->cap - start;
    if (first > n) {
        first = n;
    }
    memcpy(out, lb->buf + start, first);
    memcpy((uint8_t *)out + first, lb->buf, n - first);
    *cursor += n;
    return n;
}

uint64_t logbuf_total(const logbuf_t *lb)
{
    return lb->total;
}
