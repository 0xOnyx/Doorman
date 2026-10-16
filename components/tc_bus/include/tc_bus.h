#ifndef TC_BUS_H
#define TC_BUS_H
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "tc_proto.h"
#include "tc_tx_policy.h"

typedef struct {
    uint32_t  frames_ok;
    uint32_t  frames_err;
    int64_t   last_rx_us;              /* esp_timer_get_time() de la derniere trame, 0 si aucune */
    uint32_t  by_type[TC_TYPE_COUNT];  /* trames valides par type */
    uint32_t  for_me;                  /* trames valides marquees "pour moi" */
    int64_t   last_ok_us;              /* esp_timer_get_time() de la derniere trame valide, 0 si aucune */
    uint32_t  last_raw;                /* derniere trame valide (sans objet si last_ok_us == 0) */
    bool      last_is_long;
    tc_type_t last_type;
    /* emission (restent a 0 sans TC_BUS_TX_ENABLE) */
    uint32_t  tx_sent;                 /* telegrammes emis */
    uint32_t  tx_echo;                 /* echos relus (notre telegramme vu par le recepteur) */
    uint32_t  tx_reply;                /* reponses de la centrale apres un echo */
    uint32_t  tx_refused;              /* demandes refusees (politique, bus occupe, bloque) */
    int64_t   last_tx_us;              /* esp_timer_get_time() de la derniere emission, 0 si aucune */
} tc_bus_stats_t;

typedef void (*tc_bus_rx_cb_t)(const tc_telegram_t *t, void *ctx);

esp_err_t tc_bus_start(tc_bus_rx_cb_t cb, void *ctx);
bool      tc_bus_is_running(void);
/* Copie coherente (sous verrou) des statistiques. */
void      tc_bus_get_stats(tc_bus_stats_t *s);
/* Derniere tension mesuree sur la broche RX (mV), -1 si inconnue (pas encore d'echantillon,
 * ou recepteur RMT sans mesure analogique). */
int       tc_bus_rx_mv(void);

/* Date (esp_timer_get_time) du dernier front vu sur le bus : recepteur ADC, derniere trame de
 * conversion contenant un front ou un bas ; recepteur RMT, derniere trame recue. 0 si aucun. */
int64_t   tc_bus_last_edge_us(void);

/* Emission bloquee tant qu'au moins un tc_bus_tx_block() n'a pas son tc_bus_tx_unblock() (OTA).
 * Sans TC_BUS_TX_ENABLE : no-op. */
void      tc_bus_tx_block(void);
void      tc_bus_tx_unblock(void);
/* Condition de blocage supplementaire evaluee avant chaque emission (SoftAP de secours). */
typedef bool (*tc_bus_tx_inhibit_fn_t)(void);
void      tc_bus_tx_set_inhibit(tc_bus_tx_inhibit_fn_t fn);

typedef struct {
    tc_tx_verdict_t verdict;           /* TC_TX_OK : la politique a autorise l'emission */
    esp_err_t       err;               /* ESP_OK, ou erreur de l'emetteur RMT (rien ou partie emis) */
    uint32_t        raw;               /* telegramme d'ouverture construit depuis menuconfig */
    bool            is_long;
    tc_type_t       type;
    bool            echo;              /* notre telegramme relu a l'identique dans la seconde */
    bool            reply;             /* premier telegramme non-echo dans les 500 ms apres l'echo */
    uint32_t        reply_raw;
    bool            reply_is_long;
} tc_bus_tx_result_t;

/* Seulement avec TC_BUS_TX_ENABLE. Emet le telegramme d'ouverture (TC_BUS_OPEN_FORM) apres la
 * politique (tc_tx_check), en attendant jusqu'a 2 s un bus calme ; puis attend l'echo (1 s) et la
 * reponse (500 ms apres l'echo). Bloquant (au plus ~3,7 s). ESP_OK si emis ; ESP_ERR_INVALID_STATE
 * si refuse (r->verdict dit pourquoi) ; autre erreur : echec RMT, emission coupee jusqu'au reboot. */
esp_err_t tc_bus_open_door(tc_bus_tx_result_t *r);
/* Telegramme d'ouverture configure (seulement avec TC_BUS_TX_ENABLE). */
uint32_t  tc_bus_open_door_raw(bool *is_long);

/* true si l emetteur est cree et pas en echec (toujours false sans TC_BUS_TX_ENABLE). */
bool tc_bus_tx_ready(void);

#endif
