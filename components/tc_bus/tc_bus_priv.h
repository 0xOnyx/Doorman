#ifndef TC_BUS_PRIV_H
#define TC_BUS_PRIV_H
/* Interface interne de tc_bus entre le module commun (tc_bus.c) et le recepteur actif
 * (tc_rx_adc.c ou tc_rx_rmt.c). */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "tc_proto.h"

/* Sous httpd (5), auto-test (5) et netlog (4) : du bruit sur le bus ne doit jamais affamer l'acces
   distant. Le recepteur bufferise (DMA ou memoire RMT), la latence de decodage importe peu. */
#define TC_RX_TASK_PRIO  3
#define TC_RX_TASK_STACK 4096

/* Traite une fenetre de segments bruts (de la premiere descente au dernier front avant un repos
 * de plus de 7 ms) : filtrage des parasites, decoupage en telegrammes enchaines, decodage,
 * statistiques, logs, rappel. segs est modifie. Appele par la tache du recepteur uniquement. */
void tc_bus_process_window(tc_segment_t *segs, size_t n, int64_t now_us);

/* Etat de la reception (auto-test, /info). */
void tc_bus_set_running(bool running);

/* Demarre le recepteur choisi dans menuconfig (une seule des deux implementations est compilee). */
esp_err_t tc_rx_start(void);

/* Derniere tension mesuree sur la broche RX (mV), -1 si inconnue ou recepteur sans ADC. */
int tc_rx_mv(void);

#endif
