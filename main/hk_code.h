#ifndef HK_CODE_H
#define HK_CODE_H
/*
 * Regles du code d'appairage et du setup ID HomeKit : logique pure, sans ESP-IDF (testee sur PC).
 *
 * Header seul, ecrit pour etre a la fois du C11 et du C++14 constexpr : la meme regle sert au
 * controle a l'execution (app_homekit.c) et au controle a la compilation des chaines Kconfig
 * (hk_config_check.cpp, static_assert). Le SDK HomeKit ne refuse pas les codes triviaux.
 */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
#define HK_CONSTEXPR constexpr
#else
#define HK_CONSTEXPR
#endif

static inline HK_CONSTEXPR bool hk_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Longueur de s bornee a max + 1 : ne lit jamais apres le NUL. */
static inline HK_CONSTEXPR int hk_len_upto(const char *s, int max)
{
    int n = 0;
    while (n <= max && s[n] != '\0') {
        n++;
    }
    return n;
}

/* k-ieme chiffre (0..7) d'un code XXX-XX-XXX */
static inline HK_CONSTEXPR char hk_code_digit(const char *s, int k)
{
    return s[k < 3 ? k : (k < 5 ? k + 1 : k + 2)];
}

static inline HK_CONSTEXPR bool hk_code_digits_are(const char *s, const char *digits8)
{
    for (int k = 0; k < 8; k++) {
        if (hk_code_digit(s, k) != digits8[k]) {
            return false;
        }
    }
    return true;
}

/* Format exact XXX-XX-XXX (chiffres) et code non trivial : refuse les 8 chiffres identiques
 * (000-00-000 ... 999-99-999), 123-45-678 et 876-54-321 (liste de la specification HAP). */
static inline HK_CONSTEXPR bool hk_setup_code_valid(const char *s)
{
    if (s == NULL || hk_len_upto(s, 10) != 10) {
        return false;
    }
    for (int i = 0; i < 10; i++) {
        if (i == 3 || i == 6) {
            if (s[i] != '-') {
                return false;
            }
        } else if (!hk_is_digit(s[i])) {
            return false;
        }
    }
    bool same = true;
    for (int k = 1; k < 8; k++) {
        if (hk_code_digit(s, k) != hk_code_digit(s, 0)) {
            same = false;
        }
    }
    return !same && !hk_code_digits_are(s, "12345678") && !hk_code_digits_are(s, "87654321");
}

/* Exactement 4 caracteres 0-9 ou A-Z (majuscules). */
static inline HK_CONSTEXPR bool hk_setup_id_valid(const char *s)
{
    if (s == NULL || hk_len_upto(s, 4) != 4) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (!hk_is_digit(s[i]) && !(s[i] >= 'A' && s[i] <= 'Z')) {
            return false;
        }
    }
    return true;
}

#endif
