#ifndef APP_STATUS_H
#define APP_STATUS_H
#include "esp_system.h"
#include "esp_ota_ops.h"

const char *app_status_reset_reason_name(esp_reset_reason_t r);
const char *app_status_ota_state_name(esp_ota_img_states_t s);
/* Etat d'une partition : "none" si jamais ecrite, "error" si lecture impossible. */
const char *app_status_partition_state(const esp_partition_t *p);
/* Temperature interne de la puce en degres (arrondie), ou -1000 si le capteur est indisponible. */
int app_status_chip_temp_c(void);

#endif
