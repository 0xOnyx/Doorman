#ifndef APP_OTA_H
#define APP_OTA_H
#include <stdbool.h>

void app_ota_start(void);
bool app_ota_is_running(void);     /* serveur HTTP demarre */
bool app_ota_in_progress(void);    /* un transfert OTA est en cours */
bool app_ota_token_ok(void);       /* token OTA assez long pour autoriser /ota et /info */

#endif
