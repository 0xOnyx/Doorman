/*
 * Garde-fou de compilation du code d'appairage HomeKit (spec phase 3, section 5).
 *
 * En C, une chaine litterale ne peut pas etre lue dans un _Static_assert : ce fichier est en C++
 * pour evaluer a la compilation (constexpr) exactement la regle de hk_code.h, testee sur PC.
 * Le message d'erreur ne contient jamais la valeur du code. Aucun code n'est produit.
 */
#include "sdkconfig.h"
#include "hk_code.h"

static_assert(hk_setup_code_valid(CONFIG_APP_HK_SETUP_CODE),
              "APP_HK_SETUP_CODE invalide : format XXX-XX-XXX (chiffres), ni 8 chiffres identiques, "
              "ni 123-45-678, ni 876-54-321 (menuconfig > Interphone > HomeKit)");
static_assert(hk_setup_id_valid(CONFIG_APP_HK_SETUP_ID),
              "APP_HK_SETUP_ID invalide : 4 caracteres 0-9 ou A-Z (menuconfig > Interphone > HomeKit)");
