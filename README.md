# Interphone Koch TC:Bus → HomeKit

Firmware ESP-IDF pour ESP32-C3 qui écoute le bus TC:Bus d'un interphone Koch (sonnerie, ouverture de porte, éclairage) et l'exposera dans HomeKit. La carte est installée une fois, puis tout se fait à distance : logs par TCP, mises à jour OTA signées, rollback automatique.

**État : phase 0** (socle distant + sniffer du bus). L'émission sur le bus et HomeKit arriveront par OTA dans les phases suivantes. Voir `docs/roadmap.md` et les spécifications dans `docs/superpowers/specs/`.

## Prérequis (Linux)

- ESP-IDF v5.5.5 dans `~/esp/esp-idf`, activé dans chaque terminal par `. ~/esp/esp-idf/export.sh`.
- Accès au port série : `sudo usermod -aG dialout $USER`, puis se déconnecter et se reconnecter.

## Avant le premier flash (une seule fois, par USB)

1. Générer la clé de signature des images :
   ```bash
   idf.py secure-generate-signing-key --version 2 --scheme rsa3072 keys/signing_key.pem
   ```
   **La sauvegarder hors du PC.** Sans elle, plus aucune OTA n'est possible : la carte refuse toute image signée par une autre clé.
2. Configurer (`idf.py set-target esp32c3` seulement s'il n'y a pas encore de `sdkconfig`), puis `idf.py menuconfig`, menu **Interphone** :
   - SSID et mot de passe du Wi-Fi 2,4 GHz ;
   - token OTA d'au moins 32 caractères (`openssl rand -hex 24`) ;
   - mot de passe du SoftAP de secours (8 à 63 caractères). Sans lui, pas de secours si la box change.
3. Carte débranchée de la plaque d'interface :
   ```bash
   idf.py -p /dev/ttyACM0 erase-flash flash monitor
   ```
   `erase-flash` ne sert qu'au tout premier flash. Pour reflasher plus tard par USB après des OTA, utiliser `idf.py -p /dev/ttyACM0 erase-otadata flash` : sinon le bootloader continue de démarrer l'autre slot OTA et ignore l'image fraîchement flashée.

## Ensuite, tout passe par le réseau

Le token est celui de `CONFIG_APP_OTA_TOKEN` (`export TOKEN=...`).

- Logs : `python3 tools/logs.py` (se reconnecte seul après un redémarrage), ou `nc interphone.local 3333`.
- État : `curl -H "Authorization: Bearer $TOKEN" http://interphone.local:8080/info`
- Mise à jour : incrémenter `version.txt`, puis `tools/ota.sh` (compile, envoie, suit les logs).
- Redémarrage : `curl -X POST -H "Authorization: Bearer $TOKEN" http://interphone.local:8080/reboot`
- Secours : après 10 min sans Wi-Fi, la carte ouvre le point d'accès `interphone-recovery`. S'y connecter, puis mêmes commandes sur `192.168.4.1` (`tools/ota.sh 192.168.4.1`, `python3 tools/logs.py 192.168.4.1`).
- Rollback automatique : une image qui ne passe pas son auto-test en 90 s revient seule à la précédente.

## Tests sur PC

```bash
make -C test/host test
```

## Sécurité du bus

- Le bus est partagé par tout l'immeuble : une émission erronée dérange les voisins.
- GPIO10 (émission) est forcé à 0 en toute première ligne de `app_main`.
- L'émission n'est pas compilée en phase 0.
- Seules les commandes `OPEN_DOOR` et `LIGHT` seront autorisées (liste blanche dans `tc_proto.c`).

## Crédits

Implémentation originale (MIT), fondée sur le travail de rétro-ingénierie de la communauté, utilisé comme référence technique :

- Doorman (AzonInc, GPL-3.0) : https://github.com/AzonInc/Doorman
- TCSintercomArduino (atc1441) : https://github.com/atc1441/TCSintercomArduino
- peteh/doorman (GPL-3.0) : https://github.com/peteh/doorman
- HomeKit : esp-homekit-sdk (Espressif) : https://github.com/espressif/esp-homekit-sdk

Licence : MIT (voir `LICENSE`).
