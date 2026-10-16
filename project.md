# Interphone Koch TC:Bus → HomeKit (ESP32-C3, ESP-IDF, C)

> **Note (2026-10-13) : la cible est désormais l'ESP32-S3** (ESP32-S3FH4R2), et non plus l'ESP32-C3.
> Les écarts qui en découlent (target, GPIO, mémoire RMT, secure boot) sont décrits dans
> l'[amendement en tête de la spec de phase 0](docs/superpowers/specs/2026-10-13-phase0-socle-distant-design.md#amendement-2026-10-13--cible-esp32-s3).
> Le reste de ce cahier des charges n'est pas réécrit : là où il dit « C3 », lire « S3 » selon l'amendement.

> **Document de contexte pour Claude Code.**
> Il décrit le matériel, le protocole du bus et l'architecture attendue pour écrire,
> **from scratch et en C pur**, un firmware ESP-IDF qui relie un interphone Koch TC:Bus
> à Apple HomeKit via la bibliothèque C officielle `esp-homekit-sdk`.
> Lis-le en entier avant d'écrire du code, et respecte l'ordre des phases (section 10).

---

## 1. Objectif

Un accessoire HomeKit natif (HAP over IP, pas de Matter, pas de pont Home Assistant) qui :

1. **Sonnette** : quand quelqu'un sonne chez moi depuis la platine de rue, l'iPhone reçoit
   la notification native « Quelqu'un sonne ».
2. **Serrure** : depuis l'app Maison ou Siri, « ouvrir » envoie sur le bus la commande
   d'ouverture de la porte d'entrée de l'immeuble.

Hors périmètre (pour l'instant) : audio, vidéo, réglages des stations intérieures,
programmation de la centrale.

### Contrainte de déploiement : un seul flash USB

Le matériel est soudé et installé **une seule fois** près de l'interphone, puis on n'y touche plus.

- Le **tout premier firmware**, flashé une fois par USB sur l'établi, doit déjà contenir
  **tout l'accès distant** : Wi-Fi, **logs par le réseau**, **mise à jour OTA**, rollback automatique
  et point d'accès de secours (section 7).
- Ensuite, **toutes** les évolutions (sniffer, émission, HomeKit…) arrivent **par OTA**,
  et le débogage se fait **uniquement par les logs réseau**.
- Conséquence : **aucun firmware ne doit pouvoir casser l'accès distant**. Le socle réseau/OTA
  est la partie la plus critique du projet, et elle est testée avant tout le reste.

### Contraintes techniques

- **Langage : C** (C11), **framework : ESP-IDF v5.5** (plage supportée par le SDK HomeKit : v5.1 à v5.5).
- **Pas d'Arduino, pas d'ESPHome, pas de C++** dans le firmware.
- HomeKit : **`esp-homekit-sdk`** (C), dépôt officiel d'Espressif (voir §6.1).
- Pilote du bus : périphérique **RMT** d'ESP-IDF (API `driver/rmt_rx.h` / `driver/rmt_tx.h`).

---

## 2. Matériel

### 2.1 Carte

**Seeit ESP32-DEV-16P**, puce **ESP32-C3FN4** :
1 cœur RISC-V 160 MHz, 400 Ko de SRAM, **4 Mo de flash intégrée**, Wi-Fi 2,4 GHz + BLE 5,
USB-C, pas de PSRAM.

Broches exposées : `+5V`, `GND`, `+3.3V`, `GP0`…`GP10`, `GP20`, `GP21`.

| Usage | Broche | Remarque |
|---|---|---|
| **BUS_RX** (entrée) | **GPIO4** | Sortie du diviseur, ~2,9 V au repos |
| **BUS_TX** (sortie) | **GPIO10** | Commande la base du transistor via 1 kΩ. Aucun pull-up au reset |
| Bouton reset HomeKit | GPIO9 | Bouton « BOOT » de la carte (à confirmer). Lisible comme entrée **après** le démarrage |
| LED | à confirmer | Souvent GPIO8 sur les cartes C3. Ne pas l'utiliser comme signal bus |

**Broches interdites pour le bus** :
- GPIO2, GPIO8, GPIO9 : broches de démarrage (strapping).
- GPIO20/21 : UART0. **GPIO21 émet les logs du ROM au démarrage** : branché sur TX, il activerait
  le transistor et perturberait le bus à chaque reboot.
- GPIO18/19 : USB-Serial-JTAG (non exposées).

Console série (uniquement pour le premier flash) : vérifier si l'USB-C de la carte est l'USB natif
du C3 (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`) ou une puce USB-série sur UART0. Dans tous les cas,
les logs doivent **aussi** partir sur le réseau (§7.2).

### 2.2 Circuit d'interface (plaque à bandes)

```
Interphone a ──┐                         +24V_BUS ──┬──────────────┬─────────────┐
               ├─ Pont B250C800G ─ (+) ─────────────┘              │             │
Interphone b ──┘                   (−) ── GND                      R1 1 MΩ       R4 1,2 Ω
                                                                   │             R5 1,2 Ω
                                                     GPIO4 ◄───────┤ (RX)        │
                                                                   R2 147 kΩ     C
                                                                   │      GPIO10 ─ R3 1 kΩ ─ B  BD139 (NPN)
                                                                  GND            E
                                                                                 │
                                                       R6 47 kΩ : base → GND    GND

Interphone P (~25 V) ─ Traco TSR 1-2450 ─ 5 V ─ C1 1000 µF ─ D2 1N5819 ─ +5V de la carte
GND du Traco = GND commune (sortie − du pont)
```

- **Lecture** : le diviseur 1 MΩ / 147 kΩ ramène le bus (~24 V) à ~2,9 V sur GPIO4.
  Quand un appareil émet, le bus chute vers 0 V par segments → GPIO4 voit des alternances haut/bas.
- **Émission** : GPIO10 = 1 → le BD139 conduit → le bus est tiré vers 0 V (« mark »).
  GPIO10 = 0 → transistor bloqué → le bus revient à ~24 V (« space »).
  **Donc : niveau logique 1 sur TX = bus bas.** Sur RX, l'état est inversé (bus bas = GPIO4 à 0).
- La centrale (Koch BVS25) limite le courant : ~40 mA sur `a`, ~60 mA sur `P`.
- **Alimentation** : par la ligne P via le Traco. Les pics Wi-Fi peuvent dépasser ce que P fournit
  → **limiter la puissance d'émission Wi-Fi** (`esp_wifi_set_max_tx_power(52)`, soit ~13 dBm,
  en unités de 0,25 dBm), et la remonter seulement si la connexion est trop faible.
  Le téléchargement OTA est le moment où la consommation est la plus forte.
- **Diode D2 (1N5819)** entre la sortie 5 V du Traco et le +5V de la carte : elle permet de brancher
  l'USB à tout moment (récupération) sans renvoyer le 5 V de l'USB dans le Traco.

---

## 3. Règles de sécurité sur le bus (impératives)

Le bus est **partagé par tout un immeuble** (colonne de plusieurs appartements, une centrale commune).
Une erreur côté firmware peut gêner les voisins.

1. **GPIO10 doit être configuré en sortie à 0 le plus tôt possible** au démarrage (première ligne
   de `app_main`, avant NVS, Wi-Fi, HomeKit), et revenir à 0 en cas d'erreur, de panic, d'OTA ou de reboot.
   Le niveau de repos du RMT TX (`eot_level`) doit être **0**.
2. **N'émettre que des télégrammes explicitement autorisés** par une liste blanche dans le code :
   pour la V1, uniquement `OPEN_DOOR` (et éventuellement `LIGHT`).
   **Jamais** : `PROGRAMMING_MODE`, `WRITE_MEMORY`, `READ_MEMORY_BLOCK`, `SELECT_MEMORY_PAGE`,
   `RESET`, `INITIALIZE_DOOR_STATION`, `SEARCH_DEVICES`.
3. **Pas d'émission en boucle** : file d'attente bornée, au moins **200 ms** entre deux télégrammes
   émis, et un limiteur global (ex. max 5 télégrammes par minute).
4. **Ne pas émettre si le bus est occupé** : attendre que RX soit au repos depuis au moins 200 ms.
5. **Ignorer l'écho** : le montage reçoit ses propres télégrammes sur RX. Un télégramme reçu
   identique au dernier émis (dans une fenêtre courte) ne doit pas être retraité.
6. **Aucune interface réseau ne permet d'envoyer un télégramme brut.** Seules des actions nommées
   et authentifiées existent (HomeKit « ouvrir », et l'endpoint temporaire de la phase 2).
7. **Pendant une mise à jour OTA**, la file d'émission est vidée et bloquée, TX reste à 0.

---

## 4. Protocole TC:Bus

Koch TC:Bus = même protocole que le **TCS:Bus** (TCS AG). Le protocole est propriétaire et a été
rétro-ingénié par la communauté. Les valeurs ci-dessous proviennent de l'étude des projets cités en §12.

### 4.1 Couche physique

- 2 fils (`a`, `b`), ~24 V DC au repos. Le son analogique est superposé sur les mêmes fils
  (ignoré ici). La vidéo passe sur une paire séparée (T1/T2, ignorée ici).
- Un télégramme = une suite de **segments** dont le niveau **alterne** (bus bas / bus haut),
  et dont la **durée** code l'information. Le niveau lui-même n'a pas de sens, seule la durée compte.

### 4.2 Durées (en microsecondes)

| Segment | Émission | Fenêtre de réception |
|---|---|---|
| Départ d'un télégramme « commande » | 5985 µs | 5700 à 6900 µs |
| Départ d'un télégramme « réponse / ACK » | 6200 µs | 6090 à 6900 µs |
| Bit **1** | 4000 µs | 3000 à 4999 µs |
| Bit **0** | 2000 µs | 1000 à 2999 µs |

Fin de trame : bus au repos (haut) plus de ~7 ms. Segments < ~1 ms : parasites à ignorer.

> Attention : dans le code de référence, ces constantes s'appellent `*_MS` mais ce sont bien des **µs**.

### 4.3 Structure d'une trame

```
[START] [LEN] [b(n-1)] [b(n-2)] ... [b0] [CHK]
```

- `START` : segment de départ (5985 µs pour une commande, 6200 µs pour un ACK).
- `LEN` : 1 bit. `1` = télégramme **long** (32 bits de données), `0` = **court** (16 bits).
  Exception : un ACK court a **4 bits** de données.
- Données : **MSB en premier**.
- `CHK` : bit de parité. `CHK = 1 XOR b(n-1) XOR ... XOR b0`
  (initialisé à 1, XOR de tous les bits de données ; **le bit LEN n'est pas inclus**).

Ordre des niveaux à l'émission (TX=1 ⇒ « mark » = bus bas) :
- `START` : mark
- `LEN` : space
- puis pour chaque bit `i` de `n-1` à `0` : **mark si `i` est impair, space si `i` est pair**
- `CHK` : mark
- puis repos (TX=0).

Comme `n` vaut 16 ou 32, le premier bit de données (`i = n-1`, impair) est toujours un mark,
et les niveaux alternent strictement sur toute la trame.

À la réception, se baser **uniquement sur la durée** de chaque segment (valeur absolue), pas sur son niveau.

### 4.4 Format des télégrammes utiles

Notation : `S` = numéro de série 20 bits de la station intérieure (gravé à l'usine),
`A` = adresse 6 bits de la platine de rue (0 pour la première entrée).

| Type | Longueur | Encodage | Exemple |
|---|---|---|---|
| `DOOR_CALL` (sonnerie depuis la platine de rue) | 32 bits | `0x0` en bits 28-31, `S` en bits 8-27, bit 6 = 0, bits 0-5 = `A` (bit 7 = drapeau) | `0x0C30BA80` → série `0xC30BA` |
| `INTERNAL_CALL` | 32 bits | comme `DOOR_CALL` mais bit 6 = 1 | |
| `FLOOR_CALL` (sonnette palière) | 32 bits | `0x1` en bits 28-31, `S` en bits 8-27, octet bas = `0x41` | `0x1C30BA41` |
| `START_TALKING` | 32 bits | `0x3` en bits 28-31, `S` en bits 8-27 | |
| `CONTROL_FUNCTION` | 32 bits | `0x6` en bits 28-31, `S` en bits 8-27, octet bas = fonction | |
| **`OPEN_DOOR`** | **16 bits** | `0x1` en bits 12-15, `0x1` en bits 8-11, bit 7 = « porte prête », bits 0-5 = `A` | **`0x1100`** (porte 0) |
| `LIGHT` (lumière d'escalier) | 16 bits | `0x1200` | `0x1200` |
| `STOP_TALKING` | 16 bits | `0x3` en bits 12-15 + drapeaux + `A` | |
| `ACK` | 4 bits | valeur 1 à 15, départ 6200 µs | |

Règle de décodage simple :
- donnée ≤ `0xF` sur une trame courte avec départ ACK → `ACK` ;
- trame longue → `type = (raw >> 28) & 0xF`, `serial = (raw >> 8) & 0xFFFFF` ;
- trame courte → type selon les quartets hauts (`0x11xx` = ouverture, `0x12xx` = lumière, `0x3xxx` = fin de communication…).

Types non listés : décoder en `UNKNOWN` et **logger le brut en hexadécimal** (utile pour la recherche).

### 4.5 Ce que le firmware doit en tirer

- **Sonnerie pour moi** = `DOOR_CALL` dont `serial == MON_NUMERO_DE_SERIE` (Kconfig).
  `FLOOR_CALL` pour mon numéro = sonnette palière (option : second événement HomeKit).
- **Ouvrir la porte** = émettre `OPEN_DOOR` avec `A = ADRESSE_PORTE` (Kconfig, défaut 0).

> Le numéro de série et le code exact d'ouverture **doivent être relevés en phase 1** avec le sniffer
> (sonner chez soi depuis la platine, appuyer sur le bouton d'ouverture de l'écran Koch, lire les
> télégrammes dans les logs réseau). Koch et TCS peuvent présenter de légères différences :
> **on réémet exactement ce qu'on a observé**.

---

## 5. Pilote du bus (ESP-IDF, RMT, ESP32-C3)

### 5.1 Réception (`rmt_rx`)

- Canal RMT RX sur GPIO4, résolution **1 MHz** (1 tick = 1 µs).
- `signal_range_max_ns` ≈ **7 000 000 ns** (7 ms) : fin de trame.
- **Filtre de parasites en logiciel** : le filtre matériel du RMT sur ESP32-C3 est limité à quelques µs.
  Ignorer en logiciel les segments < 1000 µs.
- Mémoire : un canal RX du C3 a 48 symboles (2 segments par symbole). Une trame longue fait
  35 segments (~18 symboles) → suffisant, sans DMA (le C3 n'a pas de DMA RMT).
- Le callback `on_recv_done` (contexte ISR) copie les symboles dans une `xQueue` ; le décodage se fait
  dans une **tâche FreeRTOS** dédiée, qui relance ensuite `rmt_receive()`.
- Décodage : machine à états sur la liste des durées (départ → LEN → bits → CHK), vérification de parité,
  puis callback vers l'application :

```c
typedef enum { TC_TYPE_UNKNOWN, TC_TYPE_ACK, TC_TYPE_DOOR_CALL, TC_TYPE_INTERNAL_CALL,
               TC_TYPE_FLOOR_CALL, TC_TYPE_START_TALKING, TC_TYPE_STOP_TALKING,
               TC_TYPE_OPEN_DOOR, TC_TYPE_LIGHT, TC_TYPE_CONTROL_FUNCTION /* ... */ } tc_type_t;

typedef struct {
    uint32_t  raw;
    bool      is_long;
    bool      is_response;   /* départ ACK */
    tc_type_t type;
    uint32_t  serial;        /* 20 bits, si présent */
    uint8_t   address;       /* 6 bits, si présent */
    uint32_t  payload;
} tc_telegram_t;
```

### 5.2 Émission (`rmt_tx`)

- Canal RMT TX sur GPIO10, résolution 1 MHz, `eot_level = 0`, **pas de porteuse**.
- Construire les `rmt_symbol_word_t` à partir de la séquence de §4.3 (niveau 1 = mark = bus bas).
- File d'émission bornée (ex. 8 éléments), tâche d'émission qui applique les règles de §3.
- Mémoriser `last_sent_raw` + horodatage pour ignorer l'écho.
- API de blocage/déblocage de l'émission (utilisée pendant l'OTA, §7.3).

### 5.3 Séparation logique / matériel

- `tc_proto.c/.h` : **logique pure, sans aucune dépendance ESP-IDF** (encodage, décodage, parité,
  classification des durées, liste blanche). Compilée et testée **sur PC** (§9).
- `tc_bus.c/.h` : RMT, tâches, files, GPIO, appels à `tc_proto`.

---

## 6. HomeKit (`esp-homekit-sdk`)

### 6.1 Dépendance

Dépôt **officiel** **`https://github.com/espressif/esp-homekit-sdk`**, branche `master`.
Maintenu par Espressif, testé en CI avec ESP-IDF **v5.1 à v5.5**, supporte explicitement l'ESP32-C3.

> Le projet Doorman utilise un fork (`rednblkx/esp-homekit-sdk`). Il est plus ancien que l'officiel et
> n'ajoute que le service NFC Access (clés « Home Key »), inutile ici : **ne pas l'utiliser**.

Intégration : sous-module Git dans `components/esp-homekit-sdk`, **figé sur un commit précis**,
puis dans le `CMakeLists.txt` racine :

```cmake
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/components/esp-homekit-sdk/components
    ${CMAKE_CURRENT_LIST_DIR}/components/esp-homekit-sdk/components/homekit)
```

Composants utilisés : `esp_hap_core`, `esp_hap_apple_profiles`, `esp_hap_extras`, `esp_hap_platform`,
`hkdf-sha`, `mu_srp`. Les composants JSON sont récupérés par l'IDF Component Manager au premier build.

Partir des `sdkconfig.defaults` et de `partitions_hap.csv` des exemples du SDK (§7.4).

### 6.2 Modèle d'accessoire

Un **accessoire autonome** (pas un pont) :

- Catégorie : `HAP_CID_DOOR_LOCK` (ou une catégorie sonnette si elle existe dans `hap.h`, à vérifier).
- Services :
    1. **Accessory Information** (obligatoire) : nom, fabricant, modèle, numéro de série, version du firmware
       (la même que `esp_app_get_description()->version`).
    2. **Doorbell** (`HAP_SERV_UUID_DOORBELL`), marqué service primaire, avec la caractéristique
       `Programmable Switch Event` (valeurs valides : 0 = appui simple ; 1 et 2 optionnelles).
    3. **Lock Mechanism** (`hap_serv_lock_mechanism_create()`) avec
       `Lock Current State` et `Lock Target State`
       (0 = déverrouillé, 1 = verrouillé, 2 = bloqué, 3 = inconnu).

### 6.3 Comportement

| Événement | Action |
|---|---|
| `DOOR_CALL` reçu avec mon numéro de série | `hap_char_update_val()` sur `Programmable Switch Event` = 0 → notification iPhone |
| (option) `FLOOR_CALL` pour mon numéro | même caractéristique, valeur 1 (double appui) ou service séparé |
| L'iPhone écrit `Lock Target State = 0` (ouvrir) | poster une demande `OPEN_DOOR` dans la file TX, passer `Current State = 0`, puis **après ~5 s** remettre `Target = 1` et `Current = 1` |
| `OPEN_DOOR` vu sur le bus (ouverture par quelqu'un d'autre) | option : refléter brièvement `Current = 0` puis revenir à 1 |
| Échec d'émission (file pleine, bus occupé, OTA en cours) | erreur au callback d'écriture HAP ou `Current State = 2` brièvement, et log |

Le callback d'écriture HAP **ne doit jamais bloquer** : il poste dans une file, la tâche TX émet.

### 6.4 Appairage et secrets

- Le **setup code** (8 chiffres, format `XXX-XX-XXX`) et le **setup ID** (4 caractères) ne sont **pas**
  dans le code versionné : Kconfig (`sdkconfig` non commité) ou partition `factory_nvs`.
  Refuser les codes triviaux interdits par la spécification (`111-11-111`, `123-45-678`, etc.).
- **Ne jamais envoyer le setup code dans les logs réseau.** L'utilisateur le connaît (il l'a saisi dans
  `menuconfig`) et génère lui-même le QR code. Les logs réseau indiquent seulement « appairé / non appairé ».
- **Reset des appairages** : appui long (≥ 10 s) sur GPIO9 après le démarrage → `hap_reset_pairings()`.
- Les appairages sont dans la NVS, **conservée par les mises à jour OTA** : pas besoin de ré-appairer.
- Utiliser le chiffrement NVS prévu par la table de partitions du SDK (`nvs_keys`).

---

## 7. Accès distant : Wi-Fi, logs réseau, OTA, secours

**C'est le socle du projet. Il doit être présent et validé dès le premier firmware (phase 0).**

### 7.1 Wi-Fi et nom réseau

- Wi-Fi station 2,4 GHz, identifiants via Kconfig (pour le premier flash).
- Reconnexion automatique avec backoff (1 s → 60 s max), sans jamais redémarrer en boucle.
- Nom d'hôte mDNS : `interphone.local` (Kconfig). Le SDK HomeKit utilise déjà mDNS : réutiliser la même instance.
- Puissance d'émission limitée (§2.2).

### 7.2 Logs par le réseau (lecture seule)

- Rediriger les logs ESP-IDF avec `esp_log_set_vprintf()` vers :
    1. la console série (comme d'habitude) ;
    2. un **tampon circulaire** en RAM (8 à 16 Ko, la RAM du C3 est limitée).
- **Serveur TCP de logs** sur le port **3333** (Kconfig) : à la connexion d'un client, envoyer d'abord le
  contenu du tampon (l'historique récent, y compris le démarrage), puis le flux en direct.
  Lecture avec `nc interphone.local 3333`.
- Un seul client à la fois, sockets non bloquants. Le chemin de log **ne doit jamais bloquer** :
  si le client est lent, on perd des lignes plutôt que de ralentir le firmware.
- Chaque télégramme décodé produit une ligne lisible, par exemple :
  `TC RX DOOR_CALL raw=0x0C30BA80 serial=0xC30BA addr=0 len=32 chk=ok`.
- Au démarrage, logger : version du firmware, partition active, raison du dernier reset
  (`esp_reset_reason()`), état du rollback, IP, RSSI Wi-Fi, mémoire libre.
- Le flux n'est ni chiffré ni authentifié : il reste sur le réseau local et peut être désactivé dans Kconfig.
  Il ne contient **aucun secret** (pas de mot de passe Wi-Fi, pas de setup code, pas de token OTA).

### 7.3 Mise à jour OTA (push HTTP, firmware signé)

- Greffer les endpoints sur le **serveur HTTP déjà démarré par HomeKit** via
  `hap_platform_httpd_get_handle()` (évite un second serveur sur un C3 à 400 Ko de RAM).
  Vérifier que `CONFIG_HAP_HTTP_MAX_URI_HANDLERS` laisse assez de places.
  En phase 0 (avant HomeKit), démarrer un `esp_http_server` minimal sur le même port, à remplacer en phase 3.
- **`POST /ota`** :
    - En-tête obligatoire `Authorization: Bearer <OTA_TOKEN>` (Kconfig, ≥ 32 caractères aléatoires),
      comparaison en temps constant, sinon 401.
    - Refus si une OTA est déjà en cours (409) ou si la taille dépasse la partition (413).
    - Bloquer et vider la file TX du bus (§3 règle 7).
    - Écrire le flux reçu par blocs avec `esp_ota_begin()` / `esp_ota_write()` / `esp_ota_end()`
      dans `esp_ota_get_next_update_partition()`.
    - `esp_ota_end()` vérifie l'image (et sa **signature**, voir ci-dessous) : si elle échoue → 400,
      on ne change pas de partition, on débloque le bus.
    - Sinon `esp_ota_set_boot_partition()`, réponse 200, redémarrage ~1 s plus tard.
    - Commande côté PC :
      ```bash
      curl -X POST -H "Authorization: Bearer $OTA_TOKEN" \
           --data-binary @build/interphone-homekit.bin http://interphone.local/ota
      ```
- **`GET /info`** (même token) : version, partition active, uptime, mémoire libre, RSSI, raison du dernier reset,
  état HomeKit (appairé ou non), compteurs bus (trames OK, erreurs de parité).
- **Firmware signé (obligatoire)** : seules les images signées avec la clé privée de l'utilisateur sont acceptées.
    - Activer « signed app images » **sans** secure boot matériel (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`,
      schéma de signature RSA « Secure Boot V2 »). Vérifier les noms exacts des options dans la doc ESP-IDF v5.5.
    - Générer la clé : `idf.py secure-generate-signing-key --version 2 --scheme rsa3072 keys/signing_key.pem`.
    - `keys/` est dans `.gitignore`. **Prévenir l'utilisateur de sauvegarder la clé** : si elle est perdue,
      plus aucune OTA n'est possible, il faudra revenir à l'USB.
- **Rollback automatique (obligatoire)** : `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.
  Au démarrage, si l'image est `ESP_OTA_IMG_PENDING_VERIFY`, lancer un **auto-test** :
  Wi-Fi connecté en moins de 90 s, serveur de logs et endpoint `/ota` opérationnels, tâche RX du bus lancée
  (et HomeKit démarré à partir de la phase 3).
  Succès → `esp_ota_mark_app_valid_cancel_rollback()`. Échec → `esp_ota_mark_app_invalid_rollback_and_reboot()`.
  **Un firmware qui casse le Wi-Fi ou l'OTA doit donc revenir tout seul à la version précédente.**
- Secure Boot matériel et chiffrement de la flash : **hors périmètre**. Ils gravent des eFuses de façon
  **irréversible** : ne jamais les activer sans demande explicite de l'utilisateur.

### 7.4 Partitions

Reprendre `partitions_hap.csv` des exemples du SDK (4 Mo) : `sec_cert`, `nvs`, `otadata`, `phy_init`,
`ota_0` (1600 Ko), `ota_1` (1600 Ko), `factory_nvs`, `nvs_keys`.
L'application doit tenir dans **1600 Ko** : surveiller avec `idf.py size` à chaque phase.

### 7.5 Point d'accès de secours

Si la connexion Wi-Fi station échoue pendant **10 minutes** (box changée, mot de passe modifié…) :

- démarrer un **SoftAP WPA2** `interphone-recovery` (mot de passe dans Kconfig) en parallèle des tentatives
  de reconnexion ;
- n'y exposer **que** les logs (port 3333), `GET /info` et `POST /ota` (même token), à l'adresse `192.168.4.1` ;
- émission bus et HomeKit inactifs tant que la station n'est pas reconnectée ;
- arrêter le SoftAP dès que la station est reconnectée.

Ainsi, même sans réseau, l'utilisateur peut se connecter au point d'accès et envoyer un firmware corrigé
(par exemple avec de nouveaux identifiants Wi-Fi).

---

## 8. Structure du projet attendue

```
interphone-homekit/
├── CMakeLists.txt
├── sdkconfig.defaults          # cible esp32c3, rollback, signed apps, Wi-Fi TX power, HAP...
├── partitions.csv              # repris de partitions_hap.csv du SDK
├── .gitignore                  # build/, sdkconfig, keys/
├── keys/                       # clé de signature (jamais commitée)
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild       # Wi-Fi, hostname, OTA_TOKEN, AP de secours, setup code/ID,
│   │                           # MON_NUMERO_DE_SERIE, ADRESSE_PORTE, phase/fonctions activées
│   ├── main.c                  # 1) GPIO10=0  2) NVS  3) réseau+logs+OTA  4) auto-test  5) bus  6) HomeKit
│   ├── app_net.c/.h            # Wi-Fi station, backoff, mDNS, SoftAP de secours
│   ├── app_netlog.c/.h         # vprintf → tampon circulaire + serveur TCP 3333
│   ├── app_ota.c/.h            # POST /ota, GET /info, token, blocage du bus
│   ├── app_selftest.c/.h       # auto-test et validation/rollback de l'image
│   ├── app_homekit.c/.h        # accessoire, services, callbacks (phase 3)
│   └── app_button.c/.h         # reset appairage (GPIO9)
├── components/
│   ├── tc_bus/
│   │   ├── CMakeLists.txt
│   │   ├── include/tc_bus.h
│   │   ├── include/tc_proto.h
│   │   ├── tc_bus.c            # RMT RX/TX, tâches, files, garde-fous §3
│   │   └── tc_proto.c          # logique pure (testée sur PC)
│   └── esp-homekit-sdk/        # sous-module Git (espressif/esp-homekit-sdk, commit figé)
├── tools/
│   └── ota.sh                  # build + curl POST /ota + suivi des logs (nc)
└── test/
    └── host/                   # tests unitaires de tc_proto.c compilés avec gcc sur PC
```

Commandes :

```bash
idf.py set-target esp32c3
idf.py menuconfig                         # Wi-Fi, token OTA, setup code, numéro de série...
idf.py build flash monitor                # UNE SEULE FOIS, par USB, sur l'établi
./tools/ota.sh                            # toutes les mises à jour suivantes
nc interphone.local 3333                  # logs
```

---

## 9. Tests unitaires (sur PC, sans matériel)

À écrire **avant** le pilote RMT. Cas minimum pour `tc_proto` :

- Encodage → décodage aller-retour pour : `OPEN_DOOR` adresse 0 (`0x1100`), `LIGHT` (`0x1200`),
  `DOOR_CALL` série `0xC30BA` adresse 0, un `ACK` 4 bits.
- La séquence de durées générée pour `0x1100` respecte §4.3 : départ 5985, LEN 2000, 16 bits MSB
  en premier, parité correcte, alternance mark/space.
- Décodage robuste : durées dans les fenêtres de §4.2 acceptées ; durée hors fenêtre → trame rejetée ;
  mauvaise parité → trame rejetée ; parasites < 1000 µs ignorés.
- Classification des types (`raw >> 28` pour les longs, quartets hauts pour les courts).
- Liste blanche : une demande d'émission `PROGRAMMING_MODE` est refusée.

Si possible, tester aussi sur PC la logique pure du serveur OTA (vérification du token en temps constant,
contrôle de taille) et le tampon circulaire des logs.

---

## 10. Phases (dans cet ordre, une à la fois)

Chaque phase doit compiler, être testable, et se terminer par un commit.
**Phase 0 sur l'établi par USB ; toutes les suivantes uniquement par OTA et logs réseau.**

| Phase | Contenu | Critère de réussite |
|---|---|---|
| **0 : Socle distant** (flash USB unique) | Squelette ESP-IDF C3, GPIO10=0 au boot, Wi-Fi, mDNS, logs TCP, `POST /ota` signé, `GET /info`, rollback + auto-test, SoftAP de secours, `tc_proto` + tests PC. **Sniffer RX déjà inclus** (§phase 1), **émission TX désactivée en dur**. | Sur l'établi : 1) logs visibles avec `nc` ; 2) une OTA valide passe ; 3) une OTA **non signée** est refusée ; 4) une image volontairement cassée (Wi-Fi désactivé) **revient seule** à la version précédente ; 5) avec un mauvais mot de passe Wi-Fi, le SoftAP apparaît après 10 min et accepte une OTA. Seulement ensuite : installation près de l'interphone. |
| **1 : Sniffer** | Décodage complet et logs de chaque télégramme (type, hex, série, adresse, parité) | En sonnant depuis la platine et en ouvrant depuis l'écran Koch, les télégrammes apparaissent dans `nc` avec une parité valide |
| **2 : Émission** | RMT TX + garde-fous §3, endpoint **temporaire** `POST /bus/open-door` (même token, aucun paramètre libre) | La porte s'ouvre ; l'écho est ignoré ; impossible d'émettre un type hors liste blanche |
| **3 : HomeKit** | Accessoire Doorbell + Lock, appairage, endpoints OTA déplacés sur le serveur HTTP du HAP | Ajout dans l'app Maison ; notification à la sonnerie ; ouverture fonctionnelle ; OTA toujours OK ; appairage conservé après une OTA |
| **4 : Durcissement** | Bouton reset, watchdog de tâches, retrait de `/bus/open-door`, compteurs dans `/info`, revue mémoire | 24 h sans reboot ; redémarrage propre après coupure de courant ; OTA OK |

Ne pas passer à la phase suivante sans validation sur le matériel réel par l'utilisateur.

---

## 11. Licence et origine du code

- Le **protocole** (durées, format) est documenté publiquement par plusieurs projets ; on s'en sert comme
  référence technique.
- **Ne pas copier ni traduire ligne à ligne** le code de Doorman (GPL-3.0) : écrire une implémentation
  originale à partir de la description ci-dessus. Si du code GPL est repris, le projet entier doit être
  publié sous **GPL-3.0** avec crédit aux auteurs.
- `esp-homekit-sdk` : vérifier sa licence et l'usage non commercial de HAP (pas de certification MFi,
  l'app Maison affichera « Accessoire non certifié », ce qui est attendu pour un usage personnel).

---

## 12. Références

- Doorman (AzonInc), passerelle TC:Bus ESP32, composant `tc_bus` (protocole) et `homekit` (usage du SDK) :
  https://github.com/AzonInc/Doorman
- Documentation du composant TC:BUS : https://doorman.azon.ai/reference/esphome-component
- Rétro-ingénierie d'origine du bus TCS (atc1441) : https://github.com/atc1441/TCSintercomArduino
- Passerelle MQTT historique (peteh) : https://github.com/peteh/doorman
- esp-homekit-sdk (Espressif, dépôt officiel utilisé) : https://github.com/espressif/esp-homekit-sdk
- ESP-IDF OTA : https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/ota.html
- ESP-IDF signature des applications / Secure Boot V2 :
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/security/secure-boot-v2.html
- ESP-IDF RMT (ESP32-C3) : https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/peripherals/rmt.html
- ESP-IDF GPIO (ESP32-C3) : https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/peripherals/gpio.html
- FAQ Koch TC:Bus (câblage, tensions) : https://www.kochag.ch/fr/company/service/faq/tc-bus/

---

## 13. Valeurs à compléter par l'utilisateur

| Paramètre | Valeur | Comment l'obtenir |
|---|---|---|
| SSID / mot de passe Wi-Fi | _menuconfig_ | Avant le premier flash |
| `OTA_TOKEN` | _menuconfig_ | `openssl rand -hex 24` |
| Mot de passe du SoftAP de secours | _menuconfig_ | Au moins 12 caractères |
| Clé de signature | `keys/signing_key.pem` | Générée en phase 0. **À sauvegarder hors du PC** |
| Setup code / setup ID HomeKit | _menuconfig_ | Choisis par l'utilisateur (codes triviaux interdits) |
| `MON_NUMERO_DE_SERIE` | _à relever_ | Phase 1 : sonner chez soi depuis la platine, lire `serial` du `DOOR_CALL` |
| `ADRESSE_PORTE` | 0 (probable) | Phase 1 : appuyer sur « ouvrir » sur l'écran Koch, lire l'adresse de l'`OPEN_DOOR` |
| Télégramme d'ouverture exact | `0x1100` (probable) | Phase 1, même manipulation |
| Tension a/b au repos | _à mesurer_ | Multimètre ; si < 22 V, R1 passera à 820 kΩ (aucun impact firmware) |
| Broche du bouton BOOT | GPIO9 (probable) | Fiche de la carte Seeit |