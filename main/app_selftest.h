#ifndef APP_SELFTEST_H
#define APP_SELFTEST_H
/* Si l'image est en attente de validation (premier boot apres OTA), lance l'auto-test :
 * acces distant (station ou SoftAP), logs, HTTP, bus, HomeKit. Succes -> image validee ; echec -> rollback. */
void app_selftest_start(void);
#endif
