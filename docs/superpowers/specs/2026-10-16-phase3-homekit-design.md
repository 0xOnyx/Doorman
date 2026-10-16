# Phase 3 : HomeKit — design

Date : 2026-10-16. Cible : ESP32-S3 installée sur l'interphone (phases 0-2 validées : récepteur ADC,
émission `OPEN_DOOR_LONG 0x14DEF480`). Livraison par OTA (version 0.3.0).

## Objectif

Accessoire HomeKit natif (HAP sur IP, sans Matter ni pont) :
1. **Sonnette** : quand on sonne chez l'utilisateur depuis la platine de rue, l'iPhone reçoit la
   notification native « Quelqu'un sonne ».
2. **Serrure** : « ouvrir » dans l'app Maison ou avec Siri envoie l'ouverture sur le bus.

Critère de réussite : ajout dans l'app Maison avec le code d'appairage ; notification à la sonnerie ;
ouverture depuis Maison ; OTA toujours fonctionnelle ; appairage conservé après une OTA.

## 1. Dépendance

`esp-homekit-sdk` officiel (Espressif) en sous-module Git `components/esp-homekit-sdk`, figé sur
`676fabac` (HEAD au 2026-10-16). `EXTRA_COMPONENT_DIRS` = `components/esp-homekit-sdk/components`
et `.../components/homekit`. Composants : `esp_hap_core`, `esp_hap_apple_profiles`, `esp_hap_extras`
(si requis par les autres), `esp_hap_platform`, `hkdf-sha`, `mu_srp` ; dépendances du Component
Manager (`json_generator`, `json_parser`, `libsodium`, `mdns`). Licence : « Espressif MIT », usage
personnel non commercial (attendu : l'app Maison affiche « Accessoire non certifié »).

## 2. Accessoire

- Autonome, catégorie `HAP_CID_LOCK` (pas de catégorie « sonnette » sans caméra).
- `hap_acc_cfg_t` : nom `Interphone`, fabricant `DIY`, modèle `Koch TC:Bus`, numéro de série = MAC,
  `fw_rev` = `esp_app_get_description()->version`.
- Services :
  1. Accessory Information (automatique).
  2. **Doorbell** : `hap_serv_create("121")`, marqué primaire (`hap_serv_mark_primary`), avec
     `hap_char_programmable_switch_event_create(0)` et `hap_char_add_valid_vals(hc, {0}, 1)`
     (ne **pas** utiliser `hap_char_add_valid_vals_range`, débordement d'un octet dans le SDK).
  3. **Lock Mechanism** : `hap_serv_lock_mechanism_create(1, 1)` (verrouillée au démarrage).

## 3. Sonnette

Nouveau prédicat pur, testé sur PC :

```c
bool tc_proto_is_ring_for_me(const tc_telegram_t *t, uint32_t my_serial);
```

Vrai uniquement pour `DOOR_CALL` (platine de rue) et `FLOOR_CALL` (sonnette palière) dont
`serial == my_serial`, `my_serial != 0`, non réponse. Jamais pour `START_TALKING`, `CONTROL_FUNCTION`,
`OPEN_DOOR_LONG`, `REQUEST_VERSION`, `INTERNAL_CALL` (sens du numéro non vérifié), ni pour l'écho de
nos propres émissions.

Anti-rebond : au plus une notification toutes les 5 s (un appel peut être répété sur le bus).
Action : `hap_char_update_val` sur `Programmable Switch Event` = 0 (appui simple), pour les deux
types. Le format exact du `DOOR_CALL` de cet interphone n'a pas encore été relevé (test de sonnerie
à faire) : le prédicat suit le format documenté (`0x0SSSSSxx`, bit 6 = 0) et sera vérifié sur le bus.

## 4. Serrure

- L'écriture `Lock Target State = 0` (déverrouiller) depuis l'iPhone ne doit pas bloquer le serveur
  HAP : le callback poste une demande dans une file (profondeur 1) et répond aussitôt.
- Une tâche `hk_lock` (pile 4 Ko) traite la demande : `tc_bus_open_door()` (garde-fous phase 2).
  - Succès (`sent`) : `Current = 0` (déverrouillée), puis après 5 s `Target = 1` et `Current = 1`.
  - Refus ou erreur : `Current = 2` (bloquée) pendant 3 s, puis `Target = 1`, `Current = 1` ; log.
- `Target = 1` écrit par l'iPhone : accepté, rien n'est émis (la porte se reverrouille seule).
- Une ouverture vue sur le bus (écran Koch, `OPEN_DOOR*` non écho) : option, non faite en phase 3.

## 5. Appairage et secrets

- `APP_HK_SETUP_CODE` (format `XXX-XX-XXX`) et `APP_HK_SETUP_ID` (4 caractères alphanumériques
  majuscules) dans Kconfig, vides par défaut. Contrôles à la compilation : format exact, et refus des
  codes triviaux (`000-00-000` … `999-99-999`, `123-45-678`, `876-54-321`) — le SDK ne les refuse pas.
- Le code n'apparaît jamais dans les logs ni dans `/info`. Les logs disent seulement « appairé (N) »
  ou « non appairé ». `/info` ajoute `"homekit":{"started":true,"paired":N}`.
- Mode appairage expiré (10 min sans appairage, `HAP_EVENT_PAIRING_MODE_TIMED_OUT`) : réactivé
  automatiquement (`hap_pair_setup_re_enable()`) toutes les 10 min tant que non appairé.
- `POST /homekit/reset-pairings` (token OTA) : `hap_reset_pairings()` puis redémarrage — pour pouvoir
  ré-appairer sans accès au bouton BOOT (le bouton arrive en phase 4). Ne touche pas au Wi-Fi.
- Les appairages sont en NVS (`hap_ctrl`, `hap_main`) et survivent aux OTA. Chiffrement NVS : phase 4.

## 6. Intégration

- Ordre de démarrage : inchangé jusqu'au bus, puis `app_homekit_start()` après `tc_bus_start()` et
  avant `app_selftest_start()`. `hap_init` / `hap_start` après le Wi-Fi (le SDK lit la MAC pour son
  identifiant).
- Serveur HTTP HomeKit sur le port 80 (`CONFIG_HAP_HTTP_SERVER_PORT`), indépendant du serveur
  OTA/info sur 8080 (décision de phase 0, inchangée).
- mDNS : `hap_start()` force le nom `MyHost` ; rappeler `mdns_hostname_set(CONFIG_APP_HOSTNAME)`
  juste après. Vérifier que `mdns_init()` déjà appelé par `app_net` ne fait pas échouer `hap_start`.
- Sockets : `CONFIG_LWIP_MAX_SOCKETS` porté à 24 (HAP 8 + OTA 4 + logs 2 + mDNS/DHCP/marge).
- Auto-test : condition supplémentaire « HomeKit démarré » (`hap_start` OK).
- Mémoire : vérifier tas libre après démarrage (> 100 Ko visé) et taille d'image (< 1600 Ko).
- Le callback `tc_bus` (actuellement vide dans `main.c`) appelle la sonnette.

## 7. Tests

PC : `tc_proto_is_ring_for_me` (chaque type, bon / mauvais numéro, numéro 0, réponse) ; validation du
code d'appairage (fonction pure utilisée par les `_Static_assert` ou par un test sur la même règle).
Build sans warning ; image ≤ 1600 Ko.

Matériel (avec l'utilisateur) : ajout dans Maison ; ouverture depuis Maison ; notification à la
sonnerie depuis la rue ; OTA suivante qui conserve l'appairage.
