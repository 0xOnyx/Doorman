# Feuille de route

Interphone Koch TC:Bus → Apple HomeKit, ESP32-S3 (ESP32-S3FH4R2 : 2 cœurs, 4 Mo de flash, 2 Mo de PSRAM), ESP-IDF v5.5.5, C11.

Une phase = un design (`docs/superpowers/specs/`), un plan, une implémentation, une validation sur
le matériel réel par l'utilisateur. On ne commence pas la phase suivante sans cette validation.
La phase 0 est flashée par USB sur l'établi ; **toutes les autres arrivent par OTA** et se déboguent
par les logs réseau (`tools/logs.py`).

| Phase | Livrable | Ce qu'elle attend de la précédente | Critère de réussite |
|---|---|---|---|
| **0. Socle distant** | Wi-Fi + backoff, mDNS, logs TCP 3333, `POST /ota` signée sur 8080, rollback + auto-test, SoftAP de secours, sniffer RX, `tc_proto` testé sur PC. TX non compilé. | Rien. Clé de signature générée et sauvegardée. | Les 5 tests de §13.3 de la spec phase 0 passent sur l'établi. Puis installation près de l'interphone. |
| **1. Sniffer** | Options `TC_BUS_MY_SERIAL` et `TC_BUS_DOOR_ADDRESS`, marquage « pour moi » dans les logs, compteurs par type dans `/info`. Relevé des valeurs réelles. | Carte installée, logs réseau fonctionnels. | En sonnant depuis la platine et en ouvrant depuis l'écran Koch, les télégrammes apparaissent avec parité valide. **Numéro de série et télégramme d'ouverture exact relevés** (court `0x1100` ou long Koch 60/70). |
| **2. Émission** | RMT TX sur GPIO10, file bornée, garde-fous (liste blanche, 200 ms, bus au repos, limiteur 5/min, écho ignoré, blocage pendant OTA), endpoint temporaire `POST /bus/open-door` (token, aucun paramètre). | Télégramme d'ouverture exact de la phase 1. Validation explicite de l'utilisateur avant d'activer `TC_BUS_TX_ENABLE`. | La porte s'ouvre depuis `curl`. L'écho est ignoré. Un type hors liste blanche est refusé (test PC). |
| **3. HomeKit** | esp-homekit-sdk en sous-module figé, accessoire catégorie serrure avec services Doorbell (« 121 », créé à la main) et Lock Mechanism, appairage par setup code Kconfig, notification à la sonnerie, ouverture depuis Maison avec re-verrouillage après 5 s, auto-test étendu (HomeKit démarré), nom mDNS remis après `hap_start`. | Phase 2 validée. Budget flash vérifié (`idf.py size`, limite 1600 Ko). | Ajout dans l'app Maison ; notification à la sonnerie ; ouverture fonctionnelle ; OTA toujours OK ; appairage conservé après OTA. |
| **4. Durcissement** | Bouton BOOT GPIO0 (reset appairages à 10 s), watchdog de tâches, chiffrement NVS, retrait de `/bus/open-door`, revue mémoire et logs, README final. | Phase 3 validée. | 24 h sans reboot ; redémarrage propre après coupure de courant ; OTA OK. |

## Règles qui traversent toutes les phases

- GPIO10 = 0 en première ligne de `app_main`, et `eot_level = 0` dès que le RMT TX existe.
- Jamais d'émission hors liste blanche ; jamais de télégramme brut exposé sur le réseau.
- Jamais de Secure Boot matériel, de chiffrement flash ni d'anti-rollback (eFuses irréversibles).
- Aucun secret dans les logs ni dans Git (Wi-Fi, token OTA, setup code, clé de signature).
- Chaque OTA incrémente `version.txt`.
- Le code de la phase 0 (réseau, logs, OTA, auto-test) n'est plus modifié ensuite, sauf pour ajouter
  des conditions à l'auto-test.

## Valeurs à relever ou choisir (CdC §13)

| Paramètre | Quand | Où |
|---|---|---|
| SSID / mot de passe Wi-Fi, token OTA, mot de passe du SoftAP | Avant le premier flash | `menuconfig` |
| Clé de signature | Phase 0 | `keys/signing_key.pem`, sauvegardée hors du PC |
| Numéro de série, télégramme d'ouverture, adresse de porte | Phase 1 (sniffer) | `menuconfig`, puis OTA |
| Setup code et setup ID HomeKit | Phase 3 | `menuconfig` |
| Bouton BOOT (GPIO0 sur ESP32-S3) et broches sorties de la carte | Phase 4 | fiche de la carte |

## Références

- Cahier des charges de l'utilisateur (document de contexte, 2026-10-13).
- Spec phase 0 : `docs/superpowers/specs/2026-10-13-phase0-socle-distant-design.md`
  (sa section 15 liste les pièges du SDK HomeKit déjà identifiés pour la phase 3).

## Note pour la phase 3 (issue de la relecture de la phase 1)

La sonnette HomeKit ne doit pas utiliser `tc_proto_is_for_me` (repère de log, volontairement large :
`START_TALKING`, `CONTROL_FUNCTION`, `OPEN_DOOR_LONG` portent aussi mon numéro lors de mes propres actions).
Prévoir un prédicat strict `tc_proto_is_ring_for_me` : `DOOR_CALL` (et `FLOOR_CALL`, sonnette palière) avec mon
numéro ; `INTERNAL_CALL` seulement après avoir vérifié sur le bus si le numéro désigne l'appelé ou l'appelant ;
anti-rebond si un `DOOR_CALL` est répété.

## Décisions de l'utilisateur pour la phase 4 (2026-10-16)

- **Pas de bouton BOOT** : la carte est installée derrière l'interphone, le bouton n'est plus accessible.
  La remise à zéro de l'appairage passe par `POST /homekit/reset-pairings` (token), déjà en place depuis la phase 3.
- **Les outils de test restent** : `POST /bus/open-door`, `/info`, `/reboot` et les logs TCP sont conservés
  (protégés par le token ; l'utilisateur est seul sur son réseau). Ils ne sont pas retirés en phase 4.
- **Pas de chiffrement NVS ni de chiffrement flash** : irréversible (eFuses), protection partielle (les secrets du
  firmware resteraient lisibles), et un accès physique à la carte donne déjà accès au bus. En cas de vol : changer
  le mot de passe Wi-Fi et supprimer l'accessoire dans Maison.
- Restent pour la phase 4 : test de 24 h et revue mémoire.
