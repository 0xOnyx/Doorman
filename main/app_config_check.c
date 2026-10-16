/*
 * Garde-fous de compilation : refuser de produire une image qui couperait l'acces distant.
 *
 * La carte n'est flashee en USB qu'une fois, ensuite uniquement par OTA. Une image sans
 * identifiants Wi-Fi, sans token OTA utilisable ou sans SoftAP de secours valide ne pourrait
 * plus etre remplacee a distance : la compilation echoue tant que ces secrets ne sont pas
 * renseignes dans menuconfig > Interphone.
 *
 * Ce fichier ne contient que des verifications ; il ne produit aucun code.
 */
#include "sdkconfig.h"

_Static_assert(sizeof(CONFIG_APP_WIFI_SSID) > 1, "APP_WIFI_SSID vide : renseigner menuconfig > Interphone");

_Static_assert(sizeof(CONFIG_APP_OTA_TOKEN) - 1 >= 32, "APP_OTA_TOKEN < 32 caracteres : l'image refuserait toute OTA");

/* 64 caracteres : uniquement en hexa, verifie a l'execution (app_net.c, ap_password_valid). */
_Static_assert(sizeof(CONFIG_APP_RECOVERY_AP_PASSWORD) - 1 >= 8 && sizeof(CONFIG_APP_RECOVERY_AP_PASSWORD) - 1 <= 64,
               "APP_RECOVERY_AP_PASSWORD : 8 a 63 caracteres (ou 64 hexa)");

#if !CONFIG_APP_BENCH_TEST
/* Sinon une image dont la station ne se connecte jamais verrait le SoftAP demarrer pendant
 * l'auto-test, qui la validerait : plus de rollback vers l'image qui fonctionnait. */
_Static_assert(CONFIG_APP_SELFTEST_TIMEOUT_S < CONFIG_APP_RECOVERY_AP_DELAY_S,
               "auto-test >= delai SoftAP : une image sans Wi-Fi serait validee par le SoftAP (APP_BENCH_TEST pour l'etabli)");
#endif

/* HomeKit (spec phase 3, section 5) : vides par defaut, l'image refuse de se construire sans eux.
 * Le format exact et le refus des codes triviaux sont verifies par hk_config_check.cpp. */
_Static_assert(sizeof(CONFIG_APP_HK_SETUP_CODE) > 1, "APP_HK_SETUP_CODE vide : renseigner menuconfig > Interphone > HomeKit");
_Static_assert(sizeof(CONFIG_APP_HK_SETUP_CODE) - 1 == 10, "APP_HK_SETUP_CODE : format XXX-XX-XXX (10 caracteres)");
_Static_assert(sizeof(CONFIG_APP_HK_SETUP_ID) - 1 == 4, "APP_HK_SETUP_ID : 4 caracteres 0-9 ou A-Z");
_Static_assert(sizeof(CONFIG_APP_HK_NAME) > 1 && sizeof(CONFIG_APP_HK_NAME) - 1 <= 64,
               "APP_HK_NAME : 1 a 64 caracteres");
