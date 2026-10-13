#include "t.h"
#include "ota_auth.h"

#define TOK "0123456789abcdef0123456789abcdef"   /* 32 caracteres */

static void test_header_exact(void)
{
    CHECK(ota_auth_check("Bearer " TOK, TOK));
}

static void test_refus(void)
{
    CHECK(!ota_auth_check("Bearer 0123456789abcdef0123456789abcdeX", TOK)); /* dernier char faux */
    CHECK(!ota_auth_check("Bearer 0123456789abcdef", TOK));                 /* tronque */
    CHECK(!ota_auth_check("Bearer " TOK "0", TOK));                          /* trop long */
    CHECK(!ota_auth_check(TOK, TOK));                                        /* sans Bearer */
    CHECK(!ota_auth_check("bearer " TOK, TOK));                              /* casse */
    CHECK(!ota_auth_check("Bearer ", TOK));
    CHECK(!ota_auth_check("", TOK));
    CHECK(!ota_auth_check(NULL, TOK));
    CHECK(!ota_auth_check("Bearer " TOK, NULL));
    CHECK(!ota_auth_check("Bearer ", ""));                                   /* token vide refuse */
    CHECK(!ota_auth_check("Bearer X123456789abcdef0123456789abcdef", TOK));  /* premier char faux */
    CHECK(!ota_auth_check("Bearer " TOK " ", TOK));                          /* espace final */
    CHECK(!ota_auth_check("Bearer" TOK, TOK));                               /* pas d'espace */
}

static void test_size(void)
{
    CHECK(!ota_size_ok(0, 1638400));
    CHECK(!ota_size_ok(1638401, 1638400));
    CHECK(ota_size_ok(1638400, 1638400));
    CHECK(ota_size_ok(1, 1638400));
    CHECK(!ota_size_ok(1, 0));
}

int main(void)
{
    RUN(test_header_exact);
    RUN(test_refus);
    RUN(test_size);
    T_END();
}
