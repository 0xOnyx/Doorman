#include <string.h>
#include "hk_code.h"
#include "t.h"

static void test_code_format(void)
{
    CHECK(hk_setup_code_valid("482-19-736"));
    CHECK(hk_setup_code_valid("102-03-405"));
    CHECK(hk_setup_code_valid("000-00-001"));
    CHECK(hk_setup_code_valid("123-45-679"));      /* proche d'un code trivial, mais pas trivial */

    CHECK(!hk_setup_code_valid(NULL));
    CHECK(!hk_setup_code_valid(""));
    CHECK(!hk_setup_code_valid("48219736"));       /* sans tirets */
    CHECK(!hk_setup_code_valid("482-19-73"));      /* trop court */
    CHECK(!hk_setup_code_valid("482-19-7366"));    /* trop long */
    CHECK(!hk_setup_code_valid("482-19-736 "));
    CHECK(!hk_setup_code_valid(" 482-19-736"));
    CHECK(!hk_setup_code_valid("48-219-736"));     /* tirets mal places */
    CHECK(!hk_setup_code_valid("482_19_736"));
    CHECK(!hk_setup_code_valid("482-1A-736"));     /* lettre */
    CHECK(!hk_setup_code_valid("482-19-73a"));
    CHECK(!hk_setup_code_valid("a82-19-736"));
    CHECK(!hk_setup_code_valid("482-19-7/6"));     /* '/' et ':' encadrent les chiffres */
    CHECK(!hk_setup_code_valid("482-19-7:6"));
    CHECK(!hk_setup_code_valid("482--9-736"));
}

static void test_code_trivial(void)
{
    char c[11];
    for (char d = '0'; d <= '9'; d++) {
        memcpy(c, "000-00-000", sizeof c);
        for (int i = 0; i < 10; i++) {
            if (i != 3 && i != 6) {
                c[i] = d;
            }
        }
        CHECK(!hk_setup_code_valid(c));
        c[9] = (d == '9') ? '8' : (char)(d + 1);  /* un seul chiffre different : accepte */
        CHECK(hk_setup_code_valid(c));
    }
    CHECK(!hk_setup_code_valid("123-45-678"));
    CHECK(!hk_setup_code_valid("876-54-321"));
    CHECK(hk_setup_code_valid("876-54-320"));
    CHECK(hk_setup_code_valid("012-34-567"));
}

static void test_setup_id(void)
{
    CHECK(hk_setup_id_valid("KX7P"));
    CHECK(hk_setup_id_valid("0000"));
    CHECK(hk_setup_id_valid("ZZ99"));
    CHECK(hk_setup_id_valid("A1B2"));

    CHECK(!hk_setup_id_valid(NULL));
    CHECK(!hk_setup_id_valid(""));
    CHECK(!hk_setup_id_valid("KX7"));
    CHECK(!hk_setup_id_valid("KX7PQ"));
    CHECK(!hk_setup_id_valid("kx7p"));             /* minuscules */
    CHECK(!hk_setup_id_valid("KX7p"));
    CHECK(!hk_setup_id_valid("KX-P"));
    CHECK(!hk_setup_id_valid("KX P"));
    CHECK(!hk_setup_id_valid("KX@P"));             /* '@' et '[' encadrent A-Z */
    CHECK(!hk_setup_id_valid("KX[P"));
}

int main(void)
{
    RUN(test_code_format);
    RUN(test_code_trivial);
    RUN(test_setup_id);
    T_END();
}
