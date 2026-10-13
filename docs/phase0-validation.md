# Phase 0 : validation sur l'établi

Date : 2026-10-13. Carte : ESP32-S3FH4R2 (4 Mo flash, 2 Mo PSRAM), USB natif `/dev/ttyACM0`,
débranchée de la plaque d'interface, alimentée par l'USB. Wi-Fi : box Zyxel, WPA3, canal 2.
Version installée en fin de validation : **0.1.6** sur `ota_1`, valide.

## Résultats

| Test (plan, tâche 15) | Résultat | Détail |
|---|---|---|
| 1. Premier flash USB, logs réseau, `/info` | OK | `erase-flash flash monitor` ; IP 192.168.1.173 ; logs TCP 3333 (historique puis direct) ; `/info` 401 sans token, JSON avec token ; mDNS `interphone.local` répond (le PC n'avait pas Avahi) ; GPIO4 entrée sans pull, GPIO10 sortie |
| 2. OTA valide | OK | 0.1.0 → 0.1.1 → 0.1.2 → 0.1.4 → 0.1.6, chaque fois `SELFTEST OK` en ~3 s |
| 3. Images refusées | OK après correction | corps modifié : 400 ; image tronquée : **acceptée en 0.1.1**, 400 en 0.1.2 (voir défaut 1) ; octets aléatoires : 400 ; sans token : 401 |
| 4. Rollback automatique | OK | 0.1.3, 0.1.5, 0.1.7 compilées avec `APP_SELFTEST_FORCE_FAIL` : `SELFTEST FAILED`, retour seul à la version précédente, `other_state` = `invalid` |
| 5. SoftAP de secours | **non testé** | test écarté par l'utilisateur ; le code a été relu (tâche 11 et relecture finale) mais n'a pas tourné sur la carte |
| Secrets dans les logs | OK | le token n'apparaît dans aucune capture réseau |

Mesures : mémoire libre ~234 Ko (minimum ~212 Ko) ; pile httpd libre minimale pendant une OTA
3 864 octets sur 8 192 ; image 856 Ko pour 1 600 Ko par emplacement ; RSSI -65 à -90 sur l'établi.

## Défauts trouvés et corrigés pendant la validation

1. **Image tronquée complétée par une ancienne signature** (`8dae13f`). L'OTA n'effaçait que la
   taille reçue ; un bloc de signature laissé par un envoi rejeté juste avant pouvait compléter
   une image sans signature. Aucun code non signé ne pouvait tourner (le contenu devait
   correspondre à une vraie signature), mais le comportement n'était pas acceptable.
   Correction : effacer aussi le secteur qui suit l'image.
2. **`tools/logs.py` bloqué après un redémarrage de la carte** (`db072b0`) : keepalive TCP,
   reconnexion en 1 s.
3. **Échec d'auto-test invisible par le réseau** (`db072b0`) : la version ratée ne vivait que ~4 s ;
   délai avant rollback porté à 10 s.
4. **`httpd_sess_delete: error enabling SO_LINGER (109)`** (`db072b0`) : `CONFIG_LWIP_SO_LINGER=y`.

## Restant avant ou pendant l'installation

- SoftAP de secours non vérifié sur matériel : en cas de changement de box, le chemin de secours
  est celui du code relu, pas d'un essai réel. À défaut, l'USB reste le dernier recours.
- Signal Wi-Fi faible (jusqu'à -90 dBm) : vérifier le RSSI à l'emplacement définitif (`/info`).
- À l'installation : tension de repos ~2,9 V sur GPIO4 ; GPIO10 jamais haut (multimètre) ;
  câblage passif d'abord, puis suivre `bus_frames_ok` / `bus_frames_err` et vérifier que `/info`
  et l'OTA restent réactifs.
- Prochaine version : **0.1.8** ou plus (0.1.3, 0.1.5 et 0.1.7 ont servi aux tests de rollback).

Ensuite : phase 1 (sniffer), relever le numéro de série de la station et le télégramme
d'ouverture exact (`docs/utilisation.md`, section 7).
