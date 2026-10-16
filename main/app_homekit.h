#ifndef APP_HOMEKIT_H
#define APP_HOMEKIT_H
#include <stdbool.h>
#include "esp_err.h"
#include "tc_proto.h"

/* Accessoire HomeKit (spec phase 3) : sonnette (service Doorbell) et serrure (Lock Mechanism).
 * A appeler apres app_net_start() et tc_bus_start(), avant app_selftest_start(). Code d'appairage
 * ou setup ID invalide : erreur loggee (sans la valeur), HomeKit non demarre, le reste tourne ;
 * l'auto-test echoue alors (condition "HomeKit demarre") et une nouvelle image est annulee. */
esp_err_t app_homekit_start(void);

bool app_homekit_is_started(void);          /* hap_start() a reussi */
int  app_homekit_paired_count(void);        /* controleurs appaires, 0 si HomeKit non demarre */

/* Rappel tc_bus (tache de reception) : notifie la sonnette si quelqu'un sonne chez nous
 * (tc_proto_is_ring_for_me), au plus une fois toutes les 5 s. Ne bloque jamais. */
void app_homekit_on_telegram(const tc_telegram_t *t);

/* Efface les appairages (pas le Wi-Fi) puis redemarre (asynchrone, ~2 s). ESP_ERR_INVALID_STATE
 * si HomeKit n'est pas demarre. */
esp_err_t app_homekit_reset_pairings(void);

#endif
