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

/* Dernier front vu (voir tc_bus_last_edge_us), 0 si aucun. */
int64_t tc_rx_last_edge_us(void);
/* Date du dernier lot d'echantillons traite (ADC) : une valeur ancienne signifie que le recepteur
 * est en retard et que le bus n'est pas vraiment observe. Recepteur RMT : maintenant. */
int64_t tc_rx_last_sample_us(void);

/* --- emission (tc_tx.c, compile avec TC_BUS_TX_ENABLE) --- */
#define TC_TX_ECHO_WINDOW_US   1000000LL   /* echo accepte jusqu'a 1 s apres la fin de l'emission */
#define TC_TX_REPLY_WINDOW_US  500000LL    /* reponse attendue jusqu'a 500 ms apres l'echo */

esp_err_t tc_tx_start(void);

/* Surveillance de l'echo et de la reponse, partagee avec la tache de reception (tc_bus.c). */
typedef struct {
    bool     armed;
    uint32_t raw;          /* telegramme emis */
    bool     is_long;
    int64_t  end_us;       /* fin de l'emission, 0 tant qu'elle dure */
    bool     echo;
    int64_t  echo_us;
    bool     reply;
    uint32_t reply_raw;
    bool     reply_is_long;
} tc_tx_watch_t;
void tc_bus_watch_arm(uint32_t raw, bool is_long);     /* avant l'emission */
void tc_bus_watch_sent(int64_t end_us);                /* fin de l'emission */
void tc_bus_watch_get(tc_tx_watch_t *w);               /* copie coherente */
void tc_bus_watch_disarm(void);
/* Compteurs d'emission : sent = emis (date now_us), sinon refus. */
void tc_bus_count_tx(bool sent, int64_t now_us);

#endif
