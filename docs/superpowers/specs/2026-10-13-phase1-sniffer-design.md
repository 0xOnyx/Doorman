# Phase 1 : sniffer — design

Date : 2026-10-13. Cible : ESP32-S3 (voir l'amendement en tête de la spec de phase 0).
Validé par l'utilisateur le 2026-10-13. Livraison par OTA (version 0.1.8), sans toucher au matériel.

## Objectif

La réception et le décodage du bus existent depuis la phase 0 (une ligne de log par télégramme).
La phase 1 rend ces données exploitables : reconnaître les télégrammes qui concernent
l'appartement de l'utilisateur et suivre l'activité du bus à distance, pour relever le numéro
de série de la station intérieure et le télégramme d'ouverture exact.

Hors périmètre : toute émission sur le bus (phase 2), HomeKit (phase 3).

Critère de réussite (CdC §10, phase 1), sur le matériel installé : en sonnant depuis la platine
et en ouvrant depuis l'écran Koch, les deux télégrammes apparaissent avec une parité valide ;
une fois `TC_BUS_MY_SERIAL` réglé, ils portent le repère « pour moi ».

## 1. Réglages (`components/tc_bus/Kconfig`)

| Option | Type | Défaut | Rôle |
|---|---|---|---|
| `TC_BUS_MY_SERIAL` | hex | 0 | Numéro de série 20 bits de la station intérieure ; 0 = inconnu (aucun repère) |
| `TC_BUS_DOOR_ADDRESS` | int 0..63 | 0 | Adresse de la platine de rue ; sert au repère des ouvertures courtes |

Un numéro > 0xFFFFF est refusé à la compilation (`_Static_assert`).

## 2. « Pour moi » (logique pure, `tc_proto`)

```c
bool tc_proto_is_for_me(const tc_telegram_t *t, uint32_t my_serial, uint8_t door_address);
```

Faux si `my_serial == 0`. Vrai pour :
- `DOOR_CALL`, `INTERNAL_CALL`, `FLOOR_CALL`, `START_TALKING`, `CONTROL_FUNCTION`,
  `OPEN_DOOR_LONG`, `REQUEST_VERSION` dont `serial == my_serial` ;
- `OPEN_DOOR` (court) dont `address == door_address` **uniquement** si un `DOOR_CALL` pour moi a
  été vu dans les 3 minutes précédentes : un `OPEN_DOOR` court ne porte pas de numéro de série,
  et sans ce contexte toute ouverture de l'immeuble serait marquée. Ce contexte est géré par
  `tc_bus` (horodatage du dernier appel pour moi), la fonction pure reçoit un booléen
  `recent_call_for_me`.

Signature finale : `bool tc_proto_is_for_me(const tc_telegram_t *t, uint32_t my_serial, uint8_t door_address, bool recent_call_for_me);`

## 3. Conversion RMT testable (logique pure, `tc_proto`)

La conversion des symboles RMT en segments passe de `tc_bus.c` (non testée) à `tc_proto` :

```c
size_t tc_proto_rmt_words_to_segments(const uint32_t *words, size_t nwords, tc_segment_t *out, size_t max);
```

Disposition d'un mot (ESP-IDF `rmt_symbol_word_t`, `hal/include/hal/rmt_types.h`) :
`duration0` bits 0-14, `level0` bit 15, `duration1` bits 16-30, `level1` bit 31.
Une durée nulle termine la trame. `mark = (level == 0)` (bus bas = GPIO RX à 0).
`tc_bus.c` passe `&s_symbols[0].val` ; un `_Static_assert(sizeof(rmt_symbol_word_t) == 4)` le garde.

## 4. Statistiques et `/info`

`tc_bus_stats_t` gagne :
- `uint32_t by_type[TC_TYPE_COUNT]` (trames valides par type) ;
- `uint32_t for_me` ;
- `uint32_t last_raw`, `bool last_is_long`, `tc_type_t last_type` (dernière trame valide) ;
- `last_rx_us` existe déjà.

`tc_bus_get_stats` copie la structure sous un spinlock (`portMUX_TYPE`) : la tâche `tc_rx` écrit
sous le même verrou (corrige la lecture déchirée de `last_rx_us` relevée en phase 0).

`/info` ajoute un objet `"bus"` (tampon JSON porté à 1024 octets) :

```json
"bus":{"ok":42,"err":3,"for_me":2,"my_serial":"0xC30BA","door_call":5,"floor_call":1,
       "open_door":2,"open_door_long":0,"light":1,"unknown":4,
       "last_raw":"0x1100","last_type":"OPEN_DOOR","last_age_s":12}
```

`my_serial` vaut `"unset"` si 0. `last_*` sont absents (ou `null`) si aucune trame.
Les champs `bus_frames_ok` / `bus_frames_err` actuels sont conservés pour compatibilité.

## 5. Logs

La ligne `TC RX ...` existante se termine par ` << POUR MOI` quand `tc_proto_is_for_me` est vrai.
Au démarrage, `tc_bus` loggue `numero de serie configure : 0x...` ou
`numero de serie non configure : relever un DOOR_CALL (docs/utilisation.md, section 7)`.

## 6. Points restants de la phase 0 traités ici

1. `netlog` : keepalive TCP côté carte sur la socket du client (`SO_KEEPALIVE`, `TCP_KEEPIDLE` 10 s,
   `TCP_KEEPINTVL` 5 s, `TCP_KEEPCNT` 3) pour libérer la place d'un client mort ; vérifier que
   `CONFIG_LWIP_TCP_KEEPALIVE` (ou équivalent) est actif.
2. `/reboot` : si le timer de reboot n'existe pas ou ne démarre pas, redémarrer quand même
   (même repli que `ota_finish`) au lieu d'annoncer un reboot qui n'arrive pas.
3. `project.md` : note en tête indiquant que la cible est désormais l'ESP32-S3 (renvoi vers
   l'amendement de la spec phase 0), sans réécrire le cahier des charges.
4. `docs/utilisation.md` (section 7) : expliquer le repère `<< POUR MOI`, l'objet `bus` de `/info`
   et comment régler `TC_BUS_MY_SERIAL` puis livrer par OTA.

## 7. Tests

Tests PC (`test/host/test_tc_proto.c`) :
- `tc_proto_is_for_me` : chaque type listé avec bon / mauvais numéro ; `my_serial == 0` → faux ;
  `OPEN_DOOR` court : vrai seulement avec `recent_call_for_me` et la bonne adresse ; ACK, LIGHT,
  UNKNOWN → faux.
- `tc_proto_rmt_words_to_segments` : mots construits à la main (durées, niveaux), arrêt sur durée
  nulle en première ou seconde moitié, limite `max`, aller-retour avec `tc_proto_encode`
  (segments → mots → segments identiques, niveaux inversés pris en compte).

Build firmware sans warning ; image ≤ 1600 Ko. Validation matérielle : critère ci-dessus.
