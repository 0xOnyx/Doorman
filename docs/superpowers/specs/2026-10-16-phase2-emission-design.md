# Phase 2 : émission — design

Date : 2026-10-16. Cible : ESP32-S3, câblage réel RX GPIO9 (ADC), TX GPIO8 (base du transistor,
1 = bus tiré à 0 V). Demandée par l'utilisateur le 2026-10-16. Livraison par OTA (version 0.2.0).

## Objectif

Ouvrir la porte de l'immeuble depuis la carte, en réémettant exactement le télégramme d'ouverture
observé sur le bus, avec tous les garde-fous du CdC §3. Commande temporaire `POST /bus/open-door`
(token OTA, aucun paramètre), retirée en phase 4 au profit de HomeKit.

Critère de réussite : `curl -X POST .../bus/open-door` ouvre la porte ; la carte relit son propre
télégramme (écho) identique à l'émis ; la centrale répond (`0x0001` observé après une ouverture) ;
aucun type hors liste blanche ne peut être émis (tests PC).

## Faits relevés sur le bus (phase 1)

- Ouverture hors communication : `OPEN_DOOR_LONG 0x14DEF480` (série 0x4DEF4, adresse 0).
- Ouverture pendant une communication : `OPEN_DOOR 0x1100` (court, adresse 0).
- Chaque télégramme reçoit une réponse courte de la centrale : `0x0001` après une ouverture,
  `0x0003` après un `START_TALKING`.
- Niveaux : repos ~2,95 V sur GPIO9 ; l'écran Koch tire le bus vers ~6,7 V (0,86 V sur la broche) ;
  notre transistor tirera plus bas (~0 V), ce qui reste un « bas » pour la centrale et pour notre ADC.

## 1. Configuration (`components/tc_bus/Kconfig`)

| Option | Défaut | Rôle |
|---|---|---|
| `TC_BUS_TX_ENABLE` | n | Compile l'émission. Le `#error` actuel est remplacé par le vrai code. |
| `TC_BUS_OPEN_FORM` (choice) | `LONG` | `LONG` : `0x1<<28 \| serial<<8 \| 0x80 \| adresse` (= 0x14DEF480) ; `SHORT` : `0x1100 \| adresse` |

Avec `TX_ENABLE=y` et la forme `LONG`, `TC_BUS_MY_SERIAL` doit être non nul (`_Static_assert`).

## 2. Politique d'émission (logique pure, `tc_tx_policy.{c,h}`, testée sur PC)

```c
uint32_t tc_tx_open_raw(bool long_form, uint32_t serial, uint8_t address, bool *is_long);
typedef struct { int64_t sent_us[5]; size_t n; int64_t last_us; } tc_tx_limiter_t;
typedef enum { TC_TX_OK, TC_TX_REFUSED_TYPE, TC_TX_TOO_SOON, TC_TX_RATE_LIMIT,
               TC_TX_BUS_BUSY, TC_TX_BLOCKED } tc_tx_verdict_t;
tc_tx_verdict_t tc_tx_check(const tc_tx_limiter_t *l, const tc_telegram_t *t, int64_t now_us,
                            int64_t bus_quiet_since_us, bool blocked);
void tc_tx_record(tc_tx_limiter_t *l, int64_t now_us);
```

Règles, dans l'ordre : bloqué (OTA, SoftAP de secours) → `BLOCKED` ; `tc_proto_tx_allowed` faux →
`REFUSED_TYPE` ; moins de 200 ms depuis la dernière émission → `TOO_SOON` ; 5 émissions dans la
dernière minute → `RATE_LIMIT` ; bus actif depuis moins de 200 ms → `BUS_BUSY`.

## 3. Émetteur (`components/tc_bus/tc_tx.c`, compilé si `TC_BUS_TX_ENABLE`)

- RMT TX sur GPIO8, 1 MHz, `eot_level = 0`, sans porteuse, `invert_out = false`. Vérifier dans les
  sources que la création du canal ne fait jamais monter GPIO8 (GPIO8 est déjà forcé à 0 au boot).
- Segments de `tc_proto_encode` → symboles RMT (mark = niveau 1). Le dernier symbole se termine à 0.
- Une seule émission à la fois (mutex) ; l'appelant attend la fin (`rmt_tx_wait_all_done`).
- Avant d'émettre : attendre jusqu'à 2 s que le bus soit calme depuis 200 ms (dernier front vu par le
  récepteur ADC, exposé par une fonction `tc_bus_last_edge_us()`), sinon `BUS_BUSY`.
- `tc_bus_tx_block()` / `tc_bus_tx_unblock()` (appelés par l'OTA) deviennent effectifs ; pendant le
  SoftAP de secours l'émission est aussi refusée.

## 4. Écho et réponse

- Après émission, le récepteur voit notre télégramme. Un télégramme reçu égal (`raw`, `is_long`) au
  dernier émis dans la seconde qui suit est un écho : compté (`tx_echo`), loggé `TC RX ... (echo)`,
  **pas** transmis au callback ni compté « pour moi ».
- Le premier télégramme non-écho reçu dans les 500 ms après l'écho est la réponse de la centrale.

## 5. `POST /bus/open-door` (temporaire)

- Token OTA ; aucun paramètre accepté (corps ignoré). Compilé seulement si `TC_BUS_TX_ENABLE`.
- Construit le télégramme selon `TC_BUS_OPEN_FORM`, applique la politique, émet, attend écho et
  réponse (≤ 1,5 s au total).
- Réponses : 200 `{"sent":true,"raw":"0x14DEF480","echo":true,"reply":"0x0001"}` ;
  409 `{"sent":false,"reason":"too_soon|rate_limit"}` ; 503 `{"sent":false,"reason":"bus_busy|blocked"}` ;
  500 erreur RMT. Chaque appel est loggé (`TX OPEN_DOOR_LONG raw=... -> echo=... reply=...`).
- Jamais de télégramme brut fourni par le réseau.

## 6. `/info`

Objet `"tx"` : `{"enabled":true,"form":"long","raw":"0x14DEF480","sent":N,"echo":N,"refused":N,"last_age_s":..}`
(`"enabled":false` seul si non compilé).

## 7. Tests

PC : `tc_tx_open_raw` (0x14DEF480 pour 0x4DEF4/0 ; 0x1100 court ; adresse masquée sur 6 bits) ;
`tc_tx_check` pour chaque verdict et leurs bornes (199/200 ms, 5e/6e émission dans la minute,
glissement après 60 s, bus calme 199/200 ms, bloqué prioritaire) ; un télégramme interdit
(PROGRAMMING_MODE, WRITE_MEMORY, LIGHT long incohérent, raw > 0xFFFF court) toujours refusé.
Build sans warning avec `TX_ENABLE=n` et `=y`.

Matériel (avec l'utilisateur près de la porte) : un `curl` → porte ouverte, écho identique, réponse
`0x0001` ; un second `curl` immédiat → 409 ; aucune perturbation de l'interphone ni des voisins.
