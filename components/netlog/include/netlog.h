#ifndef NETLOG_H
#define NETLOG_H
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Alloue le tampon circulaire et redirige esp_log vers console + tampon. A appeler tres tot. */
esp_err_t netlog_init(size_t buffer_bytes);

/* Demarre la tache serveur TCP (un client a la fois) sur le port donne. */
esp_err_t netlog_server_start(int port);

/* true des que le socket d'ecoute est pret (pour l'auto-test). */
bool netlog_server_is_listening(void);

#endif
