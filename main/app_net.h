#ifndef APP_NET_H
#define APP_NET_H
#include <stdbool.h>

void app_net_start(void);
bool app_net_is_connected(void);          /* station avec adresse IP */
bool app_net_recovery_ap_active(void);
int  app_net_rssi(void);                  /* 0 si non connecte */
/* Appele par app_ota quand une OTA se termine sans reboot (arret differe du SoftAP). */
void app_net_ota_finished(void);

#endif
