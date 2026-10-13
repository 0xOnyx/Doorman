# Phase 0 : socle distant — design

Date : 2026-10-13. Projet : interphone Koch TC:Bus → Apple HomeKit sur ESP32-C3, ESP-IDF v5.5.5, C11.

Ce document est le design validé de la **phase 0** uniquement. Il s'appuie sur le cahier des charges
fourni par l'utilisateur (« Interphone Koch TC:Bus → HomeKit », ci-après « le CdC ») et sur des faits
vérifiés dans les sources d'ESP-IDF v5.5.5, d'esp-homekit-sdk (commit `676fabac`) et des projets de
référence (Doorman, atc1441, peteh). Les phases 1 à 4 auront chacune leur propre design.

## 1. Objectif de la phase 0

Produire le **seul firmware flashé par USB**. Il doit contenir tout l'accès distant, de façon à ce que
toutes les évolutions suivantes arrivent par OTA et se déboguent par les logs réseau.

Contenu :

- GPIO10 (TX bus) forcé à 0 dès la première ligne de `app_main`, code TX **non compilé**.
- Wi-Fi station avec reconnexion à backoff, mDNS `interphone.local`, puissance d'émission limitée.
- Logs ESP-IDF dupliqués vers un tampon circulaire en RAM et un serveur TCP (port 3333).
- Serveur HTTP dédié (port 8080) : `POST /ota` (image signée), `GET /info`, `POST /reboot`.
- Rollback automatique avec auto-test au premier démarrage d'une nouvelle image.
- Point d'accès Wi-Fi de secours après 10 minutes sans connexion.
- Sniffer du bus : réception RMT sur GPIO4, décodage, une ligne de log par télégramme.
- `tc_proto` (logique pure du protocole) avec tests unitaires sur PC.

Hors phase 0 : HomeKit, émission sur le bus, bouton de reset, watchdog de tâches, chiffrement NVS.

Critères de réussite (CdC §10, phase 0), vérifiés sur l'établi avant installation :

1. Logs visibles par le réseau.
2. Une OTA signée valide est appliquée et la nouvelle version démarre.
3. Une OTA non signée est refusée (400), la carte continue sur l'image en place.
4. Une image qui échoue son auto-test revient seule à la version précédente.
5. Avec un mauvais mot de passe Wi-Fi, le SoftAP de secours apparaît et accepte une OTA
   (avec les délais de test de §13.3 ; avec les délais de production, le rollback agit avant).

## 2. Décisions d'architecture

| Décision | Choix | Raison |
|---|---|---|
| Serveur HTTP pour l'OTA | **Serveur dédié, permanent, port 8080** (option 1, validée par l'utilisateur) | Le socle distant ne dépend jamais de HomeKit. Le code de phase 0 ne sera plus modifié. Coût : une tâche httpd (8 Ko de pile). |
| Logs réseau | TCP brut sur 3333, lecture seule, un client | Simple, lisible avec `nc` ou un script Python de 10 lignes. |
| Langage | C11, ESP-IDF v5.5.5, pas de C++ | CdC. Tout l'écosystème (IDF, SDK HomeKit) est en C. |
| Licence du projet | MIT, implémentation originale | Doorman et peteh sont GPL-3.0, atc1441 sans licence. Aucun code n'est copié ; ils servent de référence technique et sont crédités dans le README. |
| Secrets | Kconfig (`sdkconfig` non commité) | CdC §6.4 / §13 pour la V1. |
| Signature des images | Images signées RSA-3072 sans Secure Boot matériel | Vérifiée à chaque OTA par `esp_ota_end`. Aucun eFuse n'est touché. |
| Tests PC | Harnais maison (macros `CHECK`), `gcc` + `mingw32-make` de CLion | Zéro dépendance. |

## 3. Matériel et broches (rappel du CdC)

- Carte Seeit ESP32-DEV-16P, ESP32-C3FN4, 4 Mo de flash, 400 Ko de SRAM, pas de PSRAM.
- `BUS_RX` = GPIO4 (entrée, bus bas ⇒ 0). `BUS_TX` = GPIO10 (sortie, 1 ⇒ bus tiré à 0).
- Interdits pour le bus : GPIO2, 8, 9 (strapping), GPIO20/21 (UART0, logs ROM), GPIO18/19 (USB).
- Alimentation par la ligne P via Traco TSR 1-2450 : puissance Wi-Fi limitée à 52 (13 dBm).
- Console : UART0 **et** USB-Serial-JTAG natif activés tous les deux, pour couvrir les deux
  câblages possibles de l'USB-C de la carte.

## 4. Structure du dépôt

```
DoormanJerdos/
├── CMakeLists.txt              # project(interphone-homekit), version via version.txt
├── version.txt                 # "0.1.0" ; incrémenté à chaque OTA
├── sdkconfig.defaults
├── partitions.csv              # reprise de partitions_hap.csv du SDK HomeKit
├── .gitignore                  # build*/, sdkconfig, sdkconfig.old, keys/, managed_components/, dependencies.lock
├── README.md                   # usage, crédits (Doorman, atc1441, peteh, esp-homekit-sdk)
├── keys/
│   └── README.md               # explique que signing_key.pem doit être sauvegardée hors du PC
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml       # espressif/mdns
│   ├── Kconfig.projbuild
│   ├── main.c                  # séquence de démarrage, battement de cœur
│   ├── app_net.c / app_net.h   # STA, backoff, mDNS, SoftAP de secours
│   ├── app_ota.c / app_ota.h   # httpd 8080 : /ota, /info, /reboot
│   ├── ota_auth.c / ota_auth.h # logique pure : comparaison de token en temps constant, contrôle de taille
│   └── app_selftest.c / .h     # validation ou rollback de l'image
├── components/
│   ├── netlog/
│   │   ├── CMakeLists.txt
│   │   ├── include/logbuf.h    # tampon circulaire pur
│   │   ├── include/netlog.h
│   │   ├── logbuf.c
│   │   └── netlog.c            # hook vprintf + serveur TCP
│   └── tc_bus/
│       ├── CMakeLists.txt
│       ├── include/tc_proto.h  # logique pure du protocole
│       ├── include/tc_bus.h
│       ├── tc_proto.c
│       └── tc_bus.c            # RMT RX, tâche, compteurs ; TX sous #if CONFIG_TC_BUS_TX_ENABLE
├── test/host/
│   ├── Makefile                # gcc, cible `test`
│   ├── t.h                     # harnais : CHECK(), RUN(), compteur d'échecs
│   ├── test_tc_proto.c
│   ├── test_logbuf.c
│   └── test_ota_auth.c
├── tools/
│   ├── ota.sh                  # build + POST /ota + suivi des logs
│   └── logs.py                 # client TCP 3333 (portable Windows, pas besoin de nc)
└── docs/superpowers/specs/     # ce document
```

Fichiers **sans aucune dépendance ESP-IDF** (compilés sur PC) : `tc_proto.c`, `logbuf.c`, `ota_auth.c`.
Ils n'incluent que `<stdint.h>`, `<stdbool.h>`, `<stddef.h>`, `<string.h>`.

## 5. Configuration

### 5.1 `Kconfig.projbuild` (menu « Interphone »)

| Option | Type | Défaut | Rôle |
|---|---|---|---|
| `APP_WIFI_SSID` | string | "" | SSID 2,4 GHz |
| `APP_WIFI_PASSWORD` | string | "" | Mot de passe |
| `APP_HOSTNAME` | string | "interphone" | Nom mDNS (`interphone.local`) |
| `APP_WIFI_TX_POWER` | int | 52 | Unités de 0,25 dBm (52 = 13 dBm) |
| `APP_RECOVERY_AP_PASSWORD` | string | "" | WPA2 du SoftAP `interphone-recovery` (≥ 8 caractères, 12 recommandés) |
| `APP_RECOVERY_AP_DELAY_S` | int | 600 | Délai sans connexion avant SoftAP (réduire à 60 pour les tests) |
| `APP_NETLOG_ENABLE` | bool | y | Serveur de logs TCP |
| `APP_NETLOG_PORT` | int | 3333 | |
| `APP_NETLOG_BUFFER_KB` | int | 12 | Taille du tampon circulaire |
| `APP_HTTP_PORT` | int | 8080 | Serveur OTA / info |
| `APP_OTA_TOKEN` | string | "" | ≥ 32 caractères, `openssl rand -hex 24` |
| `APP_SELFTEST_TIMEOUT_S` | int | 90 | Délai maximal de l'auto-test |
| `APP_SELFTEST_FORCE_FAIL` | bool | n | **Test uniquement** : force l'échec de l'auto-test |
| `APP_HEARTBEAT_S` | int | 300 | Période du battement de cœur dans les logs (0 = désactivé) |
| `TC_BUS_RX_GPIO` | int | 4 | |
| `TC_BUS_TX_GPIO` | int | 10 | |
| `TC_BUS_TX_ENABLE` | bool | n | **Doit rester `n` en phase 0** ; le code TX n'est pas compilé |
| `TC_BUS_GLITCH_US` | int | 1000 | Segments plus courts fusionnés comme parasites |

`TC_BUS_MY_SERIAL` et `TC_BUS_DOOR_ADDRESS` ne sont ajoutés qu'en phase 1, quand ils servent.

Validation au démarrage (log d'erreur explicite) : SSID vide, token de moins de 32 caractères
(l'OTA refuse alors toute requête avec 503 et le log le répète toutes les 60 s), mot de passe
du SoftAP de moins de 8 caractères (le SoftAP est alors désactivé, et le log le dit).

### 5.2 `sdkconfig.defaults` (options clés)

```
CONFIG_IDF_TARGET="esp32c3"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_APP_PROJECT_VER_FROM_CONFIG=n          # version depuis version.txt
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
# Signature des images sans Secure Boot matériel
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y
CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y
CONFIG_SECURE_BOOT_SIGNING_KEY="keys/signing_key.pem"
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
# Rollback (pas d'anti-rollback : il écrit des eFuses)
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
# Console sur les deux chemins possibles
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y
# Réseau
CONFIG_LWIP_MAX_SOCKETS=16
CONFIG_LWIP_SO_REUSE=y
CONFIG_MBEDTLS_HARDWARE_MPI=y
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
```

Les noms exacts ont été vérifiés dans `components/bootloader/Kconfig.projbuild` d'ESP-IDF v5.5.5.
`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` est `y` par défaut dès que les images signées
sont demandées ; il est écrit explicitement par sécurité. `ESP32C3_REV_MIN` reste à sa valeur par
défaut (rev 3), compatible avec RSA.

Interdits (ne jamais activer) : `CONFIG_SECURE_BOOT`, `CONFIG_SECURE_FLASH_ENC_ENABLED`,
`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`.

### 5.3 Partitions (`partitions.csv`, 4 Mo)

Reprise à l'identique de `partitions_hap.csv` des exemples du SDK HomeKit, pour ne pas avoir à
re-partitionner en phase 3 :

```
sec_cert,    0x3F, ,         0xd000,   0x3000,
nvs,         data, nvs,      0x10000,  0x6000,
otadata,     data, ota,      ,         0x2000,
phy_init,    data, phy,      ,         0x1000,
ota_0,       app,  ota_0,    0x20000,  1600K,
ota_1,       app,  ota_1,    ,         1600K,
factory_nvs, data, nvs,      0x340000, 0x6000,
nvs_keys,    data, nvs_keys, 0x346000, 0x1000,
```

Pas de partition `factory` : le premier flash USB écrit `ota_0`. L'application doit tenir dans
1600 Ko ; `idf.py size` est exécuté à la fin de chaque phase.

### 5.4 Clé de signature

Générée une fois avec `idf.py secure-generate-signing-key --version 2 --scheme rsa3072 keys/signing_key.pem`.
`keys/` est dans `.gitignore`. **L'utilisateur doit la sauvegarder hors du PC** : sans elle, plus
aucune OTA n'est possible et il faut revenir à l'USB. Le premier flash USB doit déjà être une image
signée, sinon la clé publique n'est pas embarquée et les OTA suivantes seront rejetées.

## 6. Séquence de démarrage (`main.c`)

1. `gpio_config` : GPIO10 sortie, niveau 0, sans pull-up ; GPIO4 entrée sans pull. **Première ligne.**
2. Lecture de `esp_reset_reason()`, de la partition active, de l'état OTA de l'image
   (`esp_ota_get_state_partition`) et de `esp_app_get_description()->version`. Mémorisés pour les logs et `/info`.
3. `netlog_init()` : tampon circulaire + `esp_log_set_vprintf`. Avant le Wi-Fi pour capturer le démarrage.
4. `nvs_flash_init()` ; sur `ESP_ERR_NVS_NO_FREE_PAGES` ou `ESP_ERR_NVS_NEW_VERSION_FOUND` : effacement et nouvel essai.
5. Log de démarrage : version, partition, état de l'image, raison du reset, mémoire libre.
6. `app_net_start()`.
7. `netlog_server_start()` (si activé) et `app_ota_start()`. Les deux écoutent sur `0.0.0.0` :
   ils répondent aussi bien sur la station que sur le SoftAP de secours.
8. `tc_bus_start()`.
9. `app_selftest_start()`.
10. Boucle de battement de cœur toutes les `APP_HEARTBEAT_S` secondes :
    `HB up=… heap=… min_heap=… rssi=… bus_ok=… bus_err=…`.

Aucune de ces étapes ne fait de `abort()` sur erreur réseau : l'erreur est loggée et le démarrage
continue, afin que le maximum de chemins d'accès distant restent disponibles.

## 7. Module `app_net`

### 7.1 API

```c
void   app_net_start(void);
bool   app_net_is_connected(void);        /* station avec adresse IP */
bool   app_net_recovery_ap_active(void);
int    app_net_rssi(void);                /* 0 si non connecté */
```

### 7.2 Comportement

- `esp_netif_init`, boucle d'événements par défaut, netif STA **et** AP créés au démarrage,
  Wi-Fi démarré en `WIFI_MODE_STA`, `esp_wifi_set_max_tx_power(APP_WIFI_TX_POWER)` après `esp_wifi_start`.
- `WIFI_EVENT_STA_START` → `esp_wifi_connect()`.
- `WIFI_EVENT_STA_DISCONNECTED` → état « déconnecté », `esp_timer` one-shot de reconnexion.
  Délai 1 s, doublé à chaque échec, plafonné à 60 s. Remis à 1 s sur `IP_EVENT_STA_GOT_IP`.
  **Jamais de reboot.**
- `IP_EVENT_STA_GOT_IP` → log de l'IP et du RSSI, état « connecté », annulation de la minuterie
  de secours, arrêt du SoftAP s'il est actif (retour en `WIFI_MODE_STA`). Si une OTA est en cours
  (`app_ota_in_progress()`), l'arrêt du SoftAP est différé jusqu'à sa fin pour ne pas couper le transfert.
- mDNS : `mdns_init`, `mdns_hostname_set(APP_HOSTNAME)`, `mdns_instance_name_set`. Composant
  `espressif/mdns` du Component Manager (déclaré dans `main/idf_component.yml`).
- Minuterie de secours (`APP_RECOVERY_AP_DELAY_S`, one-shot) : armée au boot, et armée de nouveau
  quand une connexion **établie** est perdue, **uniquement si elle ne tourne pas déjà**. Les échecs
  de connexion successifs (`STA_DISCONNECTED` répétés pendant le backoff) **ne la réarment jamais** :
  sinon elle ne pourrait pas expirer. Elle est annulée sur `GOT_IP`.
  Si elle expire alors que la station n'a pas d'IP : passage en `WIFI_MODE_APSTA`, SoftAP
  `interphone-recovery`, WPA2-PSK, canal 1, 2 clients max, IP `192.168.4.1`. Les tentatives
  de reconnexion de la station continuent pendant ce temps.
- Sur le SoftAP, seuls le port 3333 et le port 8080 répondent (rien d'autre n'écoute).
- Tant que le SoftAP est actif (y compris pendant un arrêt différé), `app_net_recovery_ap_active()`
  est vrai et `/info` indique `net = recovery`.

## 8. Composant `netlog`

### 8.1 `logbuf` (pur)

```c
typedef struct { uint8_t *buf; size_t cap; size_t head; size_t len; uint64_t total; } logbuf_t;
void     logbuf_init(logbuf_t *lb, uint8_t *storage, size_t cap);
void     logbuf_write(logbuf_t *lb, const void *data, size_t n);   /* écrase le plus ancien */
size_t   logbuf_read(const logbuf_t *lb, uint64_t *cursor, void *out, size_t n, uint64_t *lost);
         /* copie depuis *cursor (position absolue), avance le curseur ; si le curseur est
            plus ancien que la fenêtre disponible, il saute au plus ancien et *lost reçoit
            le nombre d'octets perdus (0 sinon) */
uint64_t logbuf_total(const logbuf_t *lb);                           /* octets écrits depuis le début */
```

Le compteur `total` (64 bits, octets écrits depuis le démarrage) sert de curseur absolu aux lecteurs :
un client garde son curseur, et la différence avec `total` dit combien reste à lire ou combien a été perdu.

### 8.2 `netlog`

- `netlog_init()` : alloue `APP_NETLOG_BUFFER_KB` Ko, installe le hook. Le hook :
  `vsnprintf` dans un tampon local de 256 octets (ligne tronquée au-delà, jamais de dépassement),
  écriture sur la console avec la fonction `vprintf` d'origine, puis `logbuf_write` sous
  `portENTER_CRITICAL(&lock)` avec un `portMUX_TYPE` statique de `netlog.c` (section critique de
  quelques microsecondes). Il **ne bloque jamais**.
- `netlog_server_start()` : tâche (pile 3 Ko) avec un socket d'écoute sur `APP_NETLOG_PORT`,
  **un client à la fois** (un second client est accepté puis fermé avec le message `busy`).
  À la connexion : envoi de tout l'historique disponible, puis boucle toutes les 50 ms : copie
  des nouveaux octets et `send` non bloquant. Si `send` renvoie `EAGAIN`, les octets non envoyés
  restent à lire au tour suivant ; si entre-temps le tampon a écrasé la position du client, le client
  reçoit `[netlog: N octets perdus]` et reprend au plus ancien disponible.
- `netlog_server_is_listening()` pour l'auto-test.
- Aucun secret n'est loggé : le code ne logge jamais `APP_WIFI_PASSWORD`, `APP_OTA_TOKEN`,
  `APP_RECOVERY_AP_PASSWORD`. Une revue le vérifie à chaque phase.

## 9. Module `app_ota`

### 9.1 `ota_auth` (pur)

```c
bool ota_auth_check(const char *authorization_header, const char *token);
     /* true si header == "Bearer " + token, comparaison en temps constant sur la longueur du token */
bool ota_size_ok(size_t content_length, size_t partition_size);
     /* false si 0, ou > partition_size */
```

### 9.2 Serveur HTTP

`esp_http_server` dédié, port `APP_HTTP_PORT`, pile 8 Ko (`esp_ota_end` vérifie le SHA-256 et la
signature RSA dans cette tâche), 4 sockets max, `lru_purge_enable`.

| Route | Auth | Réponse |
|---|---|---|
| `GET /info` | Bearer | 200, JSON : `version`, `idf`, `partition`, `ota_state`, `other_partition`, `other_state`, `uptime_s`, `heap_free`, `heap_min`, `rssi`, `reset_reason`, `net` (`sta` / `recovery`), `bus_frames_ok`, `bus_frames_err` |
| `POST /ota` | Bearer | voir ci-dessous |
| `POST /reboot` | Bearer | 200 puis `esp_restart` après 1 s |

Codes : 401 token absent ou faux ; 503 token mal configuré (< 32 caractères) ; 404 autre route.
`app_ota_is_running()` pour l'auto-test, `app_ota_in_progress()` pour `app_net`.

Valeurs de `ota_state` et `other_state` : `new`, `pending_verify`, `valid`, `invalid`, `aborted`,
`undefined`, et `none` si `esp_ota_get_state_partition` renvoie `ESP_ERR_NOT_FOUND` (slot jamais
écrit, cas du premier flash). `other_state` est lu dans le handler à chaque requête, pas mis en cache.

### 9.3 `POST /ota`

1. Token (401), puis vérification qu'aucune OTA n'est en cours (drapeau atomique, sinon 409).
2. `Content-Length` obligatoire et `ota_size_ok` contre la taille de `esp_ota_get_next_update_partition(NULL)`, sinon 413.
3. `tc_bus_tx_block()` (no-op en phase 0, mais l'appel est en place).
4. `esp_ota_begin(part, content_length, &h)`. `ESP_ERR_OTA_ROLLBACK_INVALID_STATE` (image actuelle
   encore en auto-test) → 409 « self-test pending », bus débloqué. Toute autre erreur → 500, bus débloqué.
5. Boucle `httpd_req_recv` par blocs de 4 Ko → `esp_ota_write`. Timeout socket 10 s. Toute erreur →
   `esp_ota_abort`, bus débloqué, 500 (réseau) ou 400 (premier bloc sans magic).
6. `esp_ota_end` : `ESP_ERR_OTA_VALIDATE_FAILED` → 400 « image invalid or unsigned », partition de
   boot inchangée, bus débloqué.
7. `esp_ota_set_boot_partition`, réponse 200 JSON `{ "status": "ok", "reboot_in_ms": 1000 }`,
   `esp_timer` one-shot 1 s → `esp_restart()`.

Le téléchargement est le pic de consommation : la puissance Wi-Fi reste à 52 pendant l'OTA.

## 10. Module `app_selftest`

- Au démarrage : si `esp_ota_get_state_partition(running)` ≠ `ESP_OTA_IMG_PENDING_VERIFY`, log
  de l'état et rien d'autre.
- Sinon, tâche (pile 8 Ko, car le rollback vérifie la signature de l'autre image) qui attend au
  plus `APP_SELFTEST_TIMEOUT_S` que **toutes** ces conditions soient vraies :
  - **accès distant assuré** : `app_net_is_connected()` **ou** `app_net_recovery_ap_active()` ;
  - `netlog_server_is_listening()` (ou netlog désactivé) ;
  - `app_ota_is_running()` ;
  - `tc_bus_is_running()`.
  `APP_SELFTEST_FORCE_FAIL` force l'échec.
- Succès → `esp_ota_mark_app_valid_cancel_rollback()`, log `SELFTEST OK, image validée`.
- Échec → log `SELFTEST FAILED (raison)`, `esp_ota_mark_app_invalid_rollback_and_reboot()` :
  l'image courante est marquée `ESP_OTA_IMG_INVALID` et le bootloader redémarre l'autre.
  Si cet appel échoue (aucune autre image valide), on logge, puis on **valide** quand même l'image
  (`esp_ota_mark_app_valid_cancel_rollback`) pour que l'OTA reste disponible : une image en attente
  refuserait toute OTA (409) jusqu'au prochain reboot.
- Premier flash USB : avec `otadata` vierge, le bootloader marque lui-même `ota_0` valide, l'image
  n'est jamais en attente et l'auto-test est sauté. C'est attendu.
- **Invariant de production** : `APP_SELFTEST_TIMEOUT_S` (90) doit rester inférieur à
  `APP_RECOVERY_AP_DELAY_S` (600). Ainsi une OTA avec un mauvais Wi-Fi est annulée par le rollback
  avant que le SoftAP n'apparaisse. Le SoftAP ne valide une image que dans le cas inverse, voulu
  pour le test 5 ou si l'utilisateur l'a configuré ainsi. Si l'invariant n'est pas respecté, un
  avertissement est loggé au démarrage.
- Les phases suivantes ajoutent leurs conditions (HomeKit démarré en phase 3).

## 11. Composant `tc_bus`

### 11.1 `tc_proto` (pur)

Types et structures (CdC §5.1, complétés) :

```c
typedef enum {
    TC_TYPE_UNKNOWN, TC_TYPE_ACK, TC_TYPE_DOOR_CALL, TC_TYPE_INTERNAL_CALL, TC_TYPE_FLOOR_CALL,
    TC_TYPE_START_TALKING, TC_TYPE_STOP_TALKING, TC_TYPE_OPEN_DOOR, TC_TYPE_OPEN_DOOR_LONG,
    TC_TYPE_LIGHT, TC_TYPE_CONTROL_FUNCTION, TC_TYPE_DOOR_OPENED, TC_TYPE_DOOR_CLOSED,
    TC_TYPE_END_OF_RINGTONE, TC_TYPE_END_OF_DOOR_READINESS, TC_TYPE_INIT_DOOR_STATION,
    TC_TYPE_PROGRAMMING_MODE, TC_TYPE_READ_MEMORY_BLOCK, TC_TYPE_WRITE_MEMORY,
    TC_TYPE_SELECT_MEMORY_PAGE, TC_TYPE_RESET, TC_TYPE_SEARCH_DEVICES, TC_TYPE_REQUEST_VERSION,
} tc_type_t;

typedef struct {
    uint32_t  raw;
    bool      is_long;      /* 32 bits ; false = 16 bits ou ACK 4 bits */
    bool      is_response;  /* départ 6090..6900 µs (ACK) */
    tc_type_t type;
    uint32_t  serial;       /* 20 bits, 0 si absent */
    uint8_t   address;      /* 6 bits, 0 si absent */
    uint32_t  payload;
} tc_telegram_t;

typedef struct { uint16_t duration_us; bool mark; } tc_segment_t;   /* mark = bus tiré bas = TX 1 */
#define TC_MAX_SEGMENTS 96

typedef enum { TC_ERR_NONE, TC_ERR_LENGTH, TC_ERR_START, TC_ERR_LEN_BIT, TC_ERR_TIMING, TC_ERR_PARITY } tc_decode_error_t;
```

Constantes (µs) : `TC_START_CMD_TX 5985`, `TC_START_ACK_TX 6200`, `TC_BIT1_TX 4000`,
`TC_BIT0_TX 2000` ; fenêtres RX : bit 0 `[1000, 2999]`, bit 1 `[3000, 4999]`,
départ commande `[5700, 6089]`, départ ACK `[6090, 6900]`. Tout le reste est invalide.

API :

```c
/* Encodage : construit la liste de segments de §4.3 du CdC. Renvoie le nombre de segments, 0 si refusé. */
size_t tc_proto_encode(uint32_t raw, bool is_long, bool is_ack, tc_segment_t *out, size_t max);

/* Décodage d'une trame complète (segments déjà fusionnés). true si trame valide. */
bool tc_proto_decode(const tc_segment_t *segs, size_t n, tc_telegram_t *out, tc_decode_error_t *err);

/* Fusion des parasites : supprime les segments < glitch_us en les fondant dans leurs voisins. */
size_t tc_proto_merge_glitches(tc_segment_t *segs, size_t n, uint16_t glitch_us);

/* Classification (remplit type, serial, address, payload). */
void tc_proto_classify(tc_telegram_t *t);

/* Liste blanche d'émission. */
bool tc_proto_tx_allowed(const tc_telegram_t *t);

/* Parité : 1 XOR tous les bits de données. */
uint8_t tc_proto_parity(uint32_t raw, unsigned nbits);

const char *tc_type_name(tc_type_t t);
```

**Encodage** (CdC §4.3) : `START` mark (5985, ou 6200 si ACK) ; `LEN` space (4000 si long, 2000 sinon) ;
puis pour `i` de `n-1` à 0 (`n` = 32, 16, ou 4 pour un ACK) : durée 4000 si bit `i` = 1 sinon 2000,
mark si `i` impair, space si pair ; `CHK` mark (4000 si parité = 1 sinon 2000). Pour un ACK (`n` = 4),
`i = 3` est impair : l'alternance est respectée aussi.

**Décodage** : la trame complète est livrée par le RMT (fin de trame > 7 ms). Après fusion des parasites :
1. `n` doit valoir 7 (ACK), 19 (court) ou 35 (long) ; sinon `TC_ERR_LENGTH`.
2. Segment 0 dans une fenêtre de départ, sinon `TC_ERR_START` ; `is_response` selon la fenêtre.
3. Segment 1 = `LEN` ; doit valoir 1 pour `n = 35`, 0 pour `n = 19` et `n = 7`, sinon `TC_ERR_LEN_BIT`.
4. Segments 2 à `n-2` : bits de données MSB en premier ; durée hors fenêtres → `TC_ERR_TIMING`.
5. Dernier segment : `CHK` ; doit être égal à `tc_proto_parity`, sinon `TC_ERR_PARITY`.
6. Les niveaux sont ignorés (seule la durée compte).

**Fusion des parasites** : un segment plus court que `glitch_us` est retiré ; ses deux voisins,
qui sont nécessairement du même niveau, sont fusionnés en un seul segment dont la durée est la somme
des trois. En tête ou en queue de trame, le parasite est simplement supprimé. Répété jusqu'à stabilité.

**Classification** (sources : CdC §4.4 et Doorman `protocol.cpp`) :

| Condition | Type | serial / address / payload |
|---|---|---|
| `is_response` et `raw ≤ 0xF` | ACK | payload = raw |
| long, type `(raw>>28)` = 0 | DOOR_CALL si bit 6 = 0, INTERNAL_CALL sinon | serial = bits 8-27, address = bits 0-5 |
| long, type 1, octet bas = 0x41 | FLOOR_CALL | serial |
| long, type 1, bit 7 = 1 | OPEN_DOOR_LONG | serial, address = bits 0-5, payload = bit 6 |
| long, type 3 | START_TALKING | serial, address, payload = bits 6-7 |
| long, type 5, octet bas = 0xC0 | REQUEST_VERSION | serial |
| long, type 6 | CONTROL_FUNCTION | serial, payload = octet bas |
| long, `raw>>24` = 0x82 | WRITE_MEMORY | payload = raw & 0xFFFFFF |
| long, `raw>>24` = 0x81 ou 0x89 | SELECT_MEMORY_PAGE | serial = bits 0-19 |
| court, `raw>>8` = 0x11 | OPEN_DOOR | address = bits 0-5, payload = bit 7 |
| court, `raw>>8` = 0x12 | LIGHT | |
| court, `raw>>12` = 0x3 | STOP_TALKING | address = bits 0-5, payload = bit 6 |
| court, `raw>>8` = 0x21 | DOOR_CLOSED si bit 7, sinon DOOR_OPENED | address |
| court, `raw>>8` = 0x22 / 0x24 / 0x28 | END_OF_RINGTONE / END_OF_DOOR_READINESS / INIT_DOOR_STATION | address |
| court, `raw>>8` = 0x50 | PROGRAMMING_MODE | payload = bit 0 |
| court, `raw>>8` = 0x51 / 0x52 | RESET / SEARCH_DEVICES | |
| court, `raw>>8` = 0x84 | READ_MEMORY_BLOCK | payload |
| sinon | UNKNOWN | payload = raw (loggé en hexadécimal) |

**Liste blanche** (`tc_proto_tx_allowed`) : vrai uniquement pour `OPEN_DOOR`, `OPEN_DOOR_LONG`
et `LIGHT`. Les types `PROGRAMMING_MODE`, `WRITE_MEMORY`, `READ_MEMORY_BLOCK`, `SELECT_MEMORY_PAGE`,
`RESET`, `INIT_DOOR_STATION`, `SEARCH_DEVICES` sont refusés, de même que `UNKNOWN` et tout le reste.
En phase 0 cette fonction est seulement testée sur PC ; elle sera branchée sur la file TX en phase 2.

### 11.2 `tc_bus` (ESP-IDF)

```c
typedef struct { uint32_t frames_ok; uint32_t frames_err; int64_t last_rx_us; } tc_bus_stats_t;
typedef void (*tc_bus_rx_cb_t)(const tc_telegram_t *t, void *ctx);
esp_err_t tc_bus_start(tc_bus_rx_cb_t cb, void *ctx);
bool      tc_bus_is_running(void);
void      tc_bus_get_stats(tc_bus_stats_t *s);
void      tc_bus_tx_block(void);                 /* no-op en phase 0, utilisé par l'OTA */
void      tc_bus_tx_unblock(void);
```

- Canal RMT RX : `gpio_num = TC_BUS_RX_GPIO`, `clk_src = RMT_CLK_SRC_DEFAULT`, `resolution_hz = 1 000 000`,
  `mem_block_symbols = 48`, sans DMA (le C3 n'en a pas). Vérifié : le C3 a 2 canaux RX de 48 mots.
- `rmt_receive_config_t` : `signal_range_min_ns = 2500` (limite matérielle ≈ 3,2 µs, filtre à 255 ticks
  d'horloge), `signal_range_max_ns = 7 000 000` (limite 32 767 ticks à 1 MHz).
- Tampon de réception : 48 `rmt_symbol_word_t` (96 segments) en SRAM, taille suffisante pour une trame
  longue de 35 segments même avec parasites.
- Callback `on_recv_done` (ISR) : `xQueueSendFromISR` de la structure `{num_symbols}` (les symboles
  restent dans le tampon) ; retourne `pdTRUE` si une tâche de plus haute priorité est réveillée.
- Tâche `tc_rx` (pile 4 Ko, priorité 10) : reçoit, convertit chaque symbole en deux `tc_segment_t`
  (un segment de durée 0 marque la fin et est ignoré), `tc_proto_merge_glitches`, `tc_proto_decode`,
  `tc_proto_classify`, log, compteurs, callback, puis réarme `rmt_receive`. Le réarmement se fait
  dans la tâche après copie, ce qui est plus simple que dans l'ISR ; une trame qui arriverait pendant
  le décodage (quelques dizaines de µs) serait perdue, ce qui est acceptable : le bus impose
  plusieurs centaines de ms entre télégrammes.
- Ligne de log, format fixe : `TC RX DOOR_CALL raw=0x0C30BA80 len=32 serial=0xC30BA addr=0 chk=ok`
  et pour une trame rejetée : `TC RX ERR parity raw=0x... n=35`.
- `last_rx_us` est mis à jour à chaque trame (valide ou non) pour le test « bus au repos » de la phase 2.
- Code TX : tout ce qui concerne `rmt_tx`, la file et les garde-fous est sous `#if CONFIG_TC_BUS_TX_ENABLE`
  et **n'est pas écrit en phase 0**. GPIO10 n'est jamais confié au RMT en phase 0.

## 12. Versioning

`version.txt` à la racine contient `0.1.0`. ESP-IDF l'injecte dans `esp_app_desc_t.version`.
Chaque OTA incrémente la version (patch pour un correctif, mineure pour une phase). Elle apparaît
dans le log de démarrage, dans `/info` et, en phase 3, dans la caractéristique Firmware Revision.

## 13. Tests

### 13.1 Tests PC (`test/host`, `mingw32-make test` ou `make test`)

Harnais `t.h` : `CHECK(cond)`, `CHECK_EQ(a, b)`, `RUN(test_fn)`, code de retour ≠ 0 si un échec.
Compilation : `gcc -std=c11 -Wall -Wextra -Werror -I../../components/tc_bus/include -I../../components/netlog/include -I../../main`.

`test_tc_proto.c` :
- Aller-retour encode → decode → classify pour `0x1100` (OPEN_DOOR, addr 0), `0x1200` (LIGHT),
  `0x0C30BA80` (DOOR_CALL, serial 0xC30BA, addr 0), `0x1C30BA41` (FLOOR_CALL), `0x1C30BA80`
  (OPEN_DOOR_LONG), ACK valeur 1.
- Séquence exacte pour `0x1100` : 19 segments ; `[0]` = 5985 mark ; `[1]` = 2000 space ;
  bits MSB en premier (`0001 0001 0000 0000` → 2000,2000,2000,4000,2000,2000,2000,4000, puis 8 × 2000) ;
  alternance stricte mark/space ; `CHK` = 1 XOR 1 XOR 1 = 1 → 4000 mark.
- Tolérance : les mêmes trames avec les segments LEN, données et CHK à ±20 % (4000 → 3200 et 4800,
  2000 → 1600 et 2400) et le départ à ±100 µs (5885 et 6085 ; 6100 et 6300 pour un ACK) sont décodées.
- Rejets : durée de bit à 5500 → `TC_ERR_TIMING` ; départ à 5000 → `TC_ERR_START` ; parité
  inversée → `TC_ERR_PARITY` ; 20 segments → `TC_ERR_LENGTH`.
- Parasites : insertion d'un segment de 300 µs au milieu d'un bit → fusion, trame décodée ;
  parasite en tête de trame → supprimé.
- Classification : table de §11.1, y compris `UNKNOWN` pour `0x7FFF` et `0x9ABCDEF0`.
- Liste blanche : `OPEN_DOOR`, `OPEN_DOOR_LONG`, `LIGHT` acceptés ; `PROGRAMMING_MODE`, `WRITE_MEMORY`,
  `RESET`, `UNKNOWN`, `DOOR_CALL` refusés.

`test_logbuf.c` : écriture puis lecture identique ; débordement (écriture de 2 × cap) ne conserve
que les `cap` derniers octets ; curseur en retard → saut au plus ancien et nombre d'octets perdus correct ;
écriture d'un bloc plus grand que `cap` ne garde que la fin.

`test_ota_auth.c` : header exact accepté ; mauvais token, token tronqué, token trop long, header
sans `Bearer `, `NULL` refusés ; `ota_size_ok(0)`, `ota_size_ok(part+1)` faux, `ota_size_ok(part)` vrai.

### 13.2 Build firmware

`idf.py set-target esp32c3 && idf.py build` sans warning dans le code du projet
(`-Wall -Wextra` sur `main/` et `components/`), puis `idf.py size` : taille loggée dans le commit.

### 13.3 Validation sur l'établi (par l'utilisateur, avec mon assistance)

| # | Manipulation | Attendu |
|---|---|---|
| 1 | Flash USB de 0.1.0, `python tools/logs.py interphone.local` | Historique de démarrage puis flux en direct ; `GET /info` répond |
| 2 | `version.txt` → 0.1.1, `tools/ota.sh` | 200, reboot, `SELFTEST OK`, `/info` affiche 0.1.1 |
| 3 | Build dans `build-unsigned/` avec `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`, POST | 400 `image invalid or unsigned`, carte toujours en 0.1.1 |
| 4 | Build 0.1.2 avec `APP_SELFTEST_FORCE_FAIL=y`, POST | Reboot, `SELFTEST FAILED`, second reboot, `/info` affiche `version` 0.1.1 et `other_state` = `invalid` |
| 5 | Build 0.1.3 avec mauvais mot de passe Wi-Fi, `APP_RECOVERY_AP_PASSWORD` renseigné (≥ 8 caractères), `APP_RECOVERY_AP_DELAY_S=60` et `APP_SELFTEST_TIMEOUT_S=180`, POST | L'avertissement d'invariant de §10 apparaît au boot ; après 60 s, SSID `interphone-recovery` visible ; l'auto-test passe grâce au SoftAP (`SELFTEST OK`) ; `/info` sur `192.168.4.1:8080` ; POST d'une 0.1.4 corrigée → retour sur la box |

Note sur le cas 5 : avec les délais par défaut (auto-test 90 s, SoftAP 600 s), une image sans Wi-Fi
est annulée par le rollback **avant** l'apparition du SoftAP. C'est le comportement voulu en production :
le rollback couvre « mauvais Wi-Fi par erreur dans une OTA », le SoftAP couvre « la box a changé »
sur une image déjà validée. Le test 5 inverse volontairement les délais pour observer le SoftAP et
vérifier qu'il accepte une OTA (ce qui exige que l'image soit validée, d'où la condition « ou SoftAP
actif » de l'auto-test, §10).

## 14. Gestion des erreurs et robustesse

- Aucun `ESP_ERROR_CHECK` sur les chemins réseau et OTA : log + poursuite.
- Le hook de log et le callback ISR du RMT n'allouent pas et ne bloquent pas.
- Toutes les tâches ont une pile dimensionnée et sont nommées (`netlog`, `tc_rx`, `selftest`).
- Les tampons de réception (OTA 4 Ko, RMT 48 symboles) sont alloués une fois au démarrage.
- Budget RAM estimé pour les ajouts de la phase 0 : tampon de logs 12 Ko, piles 23 Ko (netlog 3,
  httpd 8, tc_rx 4, selftest 8), sockets et tampons ~10 Ko. Mémoire libre attendue après démarrage avec Wi-Fi : > 150 Ko (vérifiée via `/info`).

## 15. Points réservés pour les phases suivantes (ne pas traiter en phase 0)

Faits vérifiés dans esp-homekit-sdk qui influenceront la phase 3 :

- Pas de `hap_serv_doorbell_create` : créer le service `"121"` à la main avec
  `hap_char_programmable_switch_event_create(0)` et `hap_char_add_valid_vals(hc, {0}, 1)`
  (ne pas utiliser `hap_char_add_valid_vals_range`, qui a un débordement de 1 octet).
- Catégorie : `HAP_CID_LOCK` (6) ; `HAP_CID_VIDEO_DOORBELL` existe mais suppose une caméra.
- `hap_start()` force le nom mDNS à `MyHost` : rappeler `mdns_hostname_set(APP_HOSTNAME)` après.
- Le serveur HTTP du SDK écoute sur le port 80 (`CONFIG_HAP_HTTP_SERVER_PORT`) : pas de conflit avec 8080.
- Le SDK ne refuse pas les setup codes triviaux : liste noire dans le firmware (`000-00-000` … `999-99-999`, `123-45-678`, `876-54-321`).
- `hap_reset_to_factory` efface **toute** la partition `nvs` ; préférer `hap_reset_pairings`.
- Mode appairage expiré après 10 min sans appairage : `hap_pair_setup_re_enable()`.
- `esp_hap_core/CMakeLists.txt` appelle `bash` : Git Bash est présent sur ce poste.
- Chiffrement NVS : le SDK lit `nvs_keys` si `CONFIG_NVS_ENCRYPTION=y` ; à activer en phase 3 avec
  la génération des clés, pas avant.
- Doorman : les Koch série 60/70 envoient `OPEN_DOOR_LONG` avec le numéro de série de la station
  intérieure ; la phase 1 relèvera la forme exacte émise par l'écran Koch de l'utilisateur.

## 16. Références

- CdC de l'utilisateur (document de contexte, version du 2026-10-13).
- ESP-IDF v5.5.5 : `components/bootloader/Kconfig.projbuild` (signature), `components/soc/esp32c3/include/soc/soc_caps.h`
  (RMT), `components/hal/esp32c3/include/hal/rmt_ll.h` (limites filtre/idle), doc OTA et RMT en ligne.
- esp-homekit-sdk `676fabac` : `hap.h`, `hap_apple_servs.h`, `hap_apple_chars.h`, `hap_platform_httpd.h`,
  `examples/*/partitions_hap.csv`, `esp_hap_mdns.c`.
- Doorman (AzonInc, GPL-3.0) `components/tc_bus/{tc_bus.h,tc_bus.cpp,protocol.cpp}` : référence technique.
- atc1441/TCSintercomArduino (sans licence), peteh/doorman (GPL-3.0) : référence technique.
