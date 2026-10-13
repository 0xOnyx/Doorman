#include "t.h"
#include "logbuf.h"
#include <string.h>

static void test_write_read(void)
{
    uint8_t st[16];
    logbuf_t lb;
    logbuf_init(&lb, st, sizeof st);
    logbuf_write(&lb, "hello", 5);
    CHECK_EQ(logbuf_total(&lb), 5);
    CHECK_EQ(logbuf_oldest(&lb), 0);

    char out[32] = {0};
    uint64_t cur = 0, lost = 99;
    size_t n = logbuf_read(&lb, &cur, out, sizeof out, &lost);
    CHECK_EQ(n, 5);
    CHECK_EQ(lost, 0);
    CHECK_EQ(cur, 5);
    CHECK(memcmp(out, "hello", 5) == 0);

    /* plus rien a lire */
    n = logbuf_read(&lb, &cur, out, sizeof out, &lost);
    CHECK_EQ(n, 0);
    CHECK_EQ(lost, 0);
}

static void test_wrap(void)
{
    uint8_t st[8];
    logbuf_t lb;
    logbuf_init(&lb, st, sizeof st);
    logbuf_write(&lb, "abcde", 5);
    logbuf_write(&lb, "fgh", 3);
    logbuf_write(&lb, "ij", 2);        /* total 10, le tampon garde "cdefghij" */
    CHECK_EQ(logbuf_total(&lb), 10);
    CHECK_EQ(logbuf_oldest(&lb), 2);

    char out[16] = {0};
    uint64_t cur = 0, lost = 0;
    size_t n = logbuf_read(&lb, &cur, out, sizeof out, &lost);
    CHECK_EQ(lost, 2);
    CHECK_EQ(n, 8);
    CHECK(memcmp(out, "cdefghij", 8) == 0);
    CHECK_EQ(cur, 10);
}

static void test_overflow_double(void)
{
    uint8_t st[16];
    logbuf_t lb;
    logbuf_init(&lb, st, sizeof st);
    logbuf_write(&lb, "hello", 5);
    logbuf_write(&lb, "0123456789abcdef0123456789abcdef", 32);   /* 2 x cap */
    char out[32] = {0};
    uint64_t cur = 0, lost = 0;
    size_t n = logbuf_read(&lb, &cur, out, sizeof out, &lost);
    CHECK_EQ(lost, 21);                 /* 37 ecrits, 16 conserves */
    CHECK_EQ(n, 16);
    CHECK(memcmp(out, "0123456789abcdef", 16) == 0);
}

static void test_block_larger_than_cap(void)
{
    uint8_t st[8];
    logbuf_t lb;
    logbuf_init(&lb, st, sizeof st);
    logbuf_write(&lb, "0123456789abcdefghij", 20);
    char out[8];
    uint64_t cur = 0, lost = 0;
    size_t n = logbuf_read(&lb, &cur, out, sizeof out, &lost);
    CHECK_EQ(lost, 12);
    CHECK_EQ(n, 8);
    CHECK(memcmp(out, "cdefghij", 8) == 0);
}

static void test_incremental_reads(void)
{
    uint8_t st[8];
    logbuf_t lb;
    logbuf_init(&lb, st, sizeof st);
    logbuf_write(&lb, "abcdefgh", 8);
    char out[3];
    uint64_t cur = 0, lost = 0;
    CHECK_EQ(logbuf_read(&lb, &cur, out, 3, &lost), 3);
    CHECK(memcmp(out, "abc", 3) == 0);
    logbuf_write(&lb, "ij", 2);        /* ecrase "ab", le curseur (3) reste valide */
    CHECK_EQ(logbuf_read(&lb, &cur, out, 3, &lost), 3);
    CHECK_EQ(lost, 0);
    CHECK(memcmp(out, "def", 3) == 0);
    CHECK_EQ(logbuf_read(&lb, &cur, out, 3, &lost), 3);
    CHECK(memcmp(out, "ghi", 3) == 0);
    CHECK_EQ(logbuf_read(&lb, &cur, out, 3, &lost), 1);
    CHECK(out[0] == 'j');
}

static void test_lost_pointer_optional(void)
{
    uint8_t st[4];
    logbuf_t lb;
    logbuf_init(&lb, st, sizeof st);
    logbuf_write(&lb, "abcdef", 6);
    char out[4];
    uint64_t cur = 0;
    CHECK_EQ(logbuf_read(&lb, &cur, out, 4, NULL), 4);
    CHECK(memcmp(out, "cdef", 4) == 0);
}

static void test_cursor_ahead_and_zero_write(void)
{
    uint8_t st[8];
    logbuf_t lb;
    logbuf_init(&lb, st, sizeof st);
    logbuf_write(&lb, "abc", 3);
    logbuf_write(&lb, "", 0);
    CHECK_EQ(logbuf_total(&lb), 3);
    CHECK_EQ(lb.len, 3);
    CHECK_EQ(lb.head, 3);
    char out[8];
    uint64_t cur = 10, lost = 7;
    CHECK_EQ(logbuf_read(&lb, &cur, out, sizeof out, &lost), 0);
    CHECK_EQ(lost, 0);
    CHECK_EQ(cur, 3);
}

static void test_stale_cursor_midstream(void)
{
    uint8_t st[8];
    logbuf_t lb;
    logbuf_init(&lb, st, sizeof st);
    logbuf_write(&lb, "abcdefgh", 8);
    char out[8];
    uint64_t cur = 0, lost = 0;
    CHECK_EQ(logbuf_read(&lb, &cur, out, 3, &lost), 3);     /* cur = 3 */
    logbuf_write(&lb, "ijklmnop", 8);                        /* total 16, fenetre = 8..15 */
    CHECK_EQ(logbuf_read(&lb, &cur, out, 8, &lost), 8);
    CHECK_EQ(lost, 5);                                       /* 3..7 perdus */
    CHECK(memcmp(out, "ijklmnop", 8) == 0);
    CHECK_EQ(cur, 16);
}

int main(void)
{
    RUN(test_write_read);
    RUN(test_wrap);
    RUN(test_overflow_double);
    RUN(test_block_larger_than_cap);
    RUN(test_incremental_reads);
    RUN(test_lost_pointer_optional);
    RUN(test_cursor_ahead_and_zero_write);
    RUN(test_stale_cursor_midstream);
    T_END();
}
