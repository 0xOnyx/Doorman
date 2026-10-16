# Interphone Koch TC:Bus → HomeKit

Firmware ESP-IDF pour ESP32-S3 (ESP32-S3FH4R2 : 2 cœurs, 4 Mo de flash, 2 Mo de PSRAM) qui écoute le bus TC:Bus d'un interphone Koch (sonnerie, ouverture de porte, éclairage) et l'exposera dans HomeKit. La carte est installée une fois, puis tout se fait à distance : logs par TCP, mises à jour OTA signées, rollback automatique.

Guide d'utilisation complet : `docs/utilisation.md`

**État : phase 2** (socle distant, sniffer du bus, ouverture de la porte par `POST /bus/open-door` quand l'émission est compilée). HomeKit arrivera par OTA dans les phases suivantes. Voir `docs/roadmap.md` et les spécifications dans `docs/superpowers/specs/`.

## Prérequis (Linux)

- ESP-IDF v5.5.5 dans `~/esp/esp-idf` (voir ci-dessous).
- Accès au port série : `sudo usermod -aG dialout $USER`, puis fermer et rouvrir la session (détail à l'étape 2 de « Flasher la carte »).

### Obtenir `idf.py`

`idf.py` est fourni par ESP-IDF v5.5.5, installé dans `~/esp/esp-idf`. Il n'est **pas** dans le `PATH` par défaut : dans **chaque nouveau terminal**, lancer

```bash
. ~/esp/esp-idf/export.sh
# ou, identique : source ~/esp/esp-idf/export.sh
```

(attention au point suivi d'une espace au début : le script doit modifier le terminal courant). Vérifier ensuite :

```bash
idf.py --version          # attendu : ESP-IDF v5.5.5
```

Raccourci conseillé par Espressif : un alias `get_idf`, à créer une seule fois.

```bash
echo "alias get_idf='. \$HOME/esp/esp-idf/export.sh'" >> ~/.bashrc && source ~/.bashrc
```

Ensuite, taper `get_idf` dans chaque terminal. Ne pas mettre directement `. ~/esp/esp-idf/export.sh` dans `~/.bashrc` : chaque terminal mettrait plusieurs secondes à s'ouvrir et aurait son `PATH` modifié, même hors de ce projet.

Si ESP-IDF n'est pas installé (nouvelle machine, Ubuntu) :

```bash
sudo apt install git wget flex bison gperf python3 python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
mkdir -p ~/esp && cd ~/esp && git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git && cd esp-idf && ./install.sh esp32s3
```

## Compiler le firmware

1. Activer ESP-IDF et aller dans le projet :
   ```bash
   . ~/esp/esp-idf/export.sh          # ou : get_idf
   cd ~/CLionProjects/Doorman
   ```
2. **Première fois seulement sur une machine** :
   ```bash
   idf.py set-target esp32s3
   ```
   Cette commande crée `sdkconfig` à partir de `sdkconfig.defaults`. Elle **remet `sdkconfig` à zéro** : ne plus jamais la relancer une fois les secrets saisis (sinon il faut tout ressaisir). Un `sdkconfig` créé pour l'ancienne cible ESP32-C3 se refait ainsi : sauvegarder d'abord ses lignes `CONFIG_APP_` et `CONFIG_TC_BUS_` (`mkdir -p build-check && grep -E '^CONFIG_(APP_|TC_BUS_)' sdkconfig > build-check/mes-secrets.defaults`, `build-check/` est ignoré par git), puis `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;build-check/mes-secrets.defaults" set-target esp32s3`.

   La clé de signature doit exister dans `keys/signing_key.pem` : la générer une seule fois (étape 5 de « Flasher la carte ») ou recopier la clé sauvegardée.
3. Configurer :
   ```bash
   idf.py menuconfig
   ```
   Menu **Interphone** : SSID et mot de passe du Wi-Fi 2,4 GHz, token OTA, mot de passe du SoftAP de secours. Navigation : flèches, Entrée pour ouvrir/modifier, Échap pour revenir ; `S` pour sauvegarder, `Q` pour quitter.

   **La compilation refuse de continuer tant que ces secrets ne sont pas renseignés**, avec par exemple :
   ```
   main/app_config_check.c:13:1: error: static assertion failed: "APP_WIFI_SSID vide : renseigner menuconfig > Interphone"
   main/app_config_check.c:15:1: error: static assertion failed: "APP_OTA_TOKEN < 32 caracteres : l'image refuserait toute OTA"
   main/app_config_check.c:18:1: error: static assertion failed: "APP_RECOVERY_AP_PASSWORD : 8 a 63 caracteres (ou 64 hexa)"
   ```
   C'est voulu : une telle image couperait l'accès distant une fois installée.
4. Compiler :
   ```bash
   idf.py build
   ```
   Réussite = les dernières lignes contiennent
   ```
   interphone-homekit.bin binary size 0x... bytes. Smallest app partition is 0x190000 bytes. 0x... bytes (NN%) free.
   Project build complete. To flash, run:
   ```
   L'image signée est `build/interphone-homekit.bin` : c'est elle qu'on envoie par OTA. **Ne jamais envoyer `build/interphone-homekit-unsigned.bin`** (la carte la refuserait). La première compilation prend plusieurs minutes (téléchargement du composant mdns), les suivantes environ une minute.
5. Vérifier la taille :
   ```bash
   idf.py size
   ```
   Limite : 1600 Ko par emplacement OTA.
6. Version : incrémenter `version.txt` avant chaque compilation destinée à une OTA.
7. En cas de problème :
   - `idf.py: command not found` → `export.sh` n'a pas été chargé dans ce terminal ;
   - erreur qui mentionne `keys/signing_key.pem` → la clé de signature manque ;
   - erreurs étranges après un changement de configuration → `idf.py fullclean` puis `idf.py build`.
8. Tests unitaires sur PC (sans carte) :
   ```bash
   make -C test/host test
   ```
   Trois lignes `N checks, 0 failures`.

## Flasher la carte (premier flash par USB)

Ce flash USB n'a lieu **qu'une fois**. Ensuite, toutes les mises à jour passent par le réseau (OTA).

1. **Matériel** : une carte de développement ESP32-S3 (puce ESP32-S3FH4R2 ; modèle exact de la carte à vérifier : quelles broches sont sorties, GPIO4 et GPIO10 compris, et où est le bouton BOOT, relié à GPIO0), **débranchée de la plaque d'interface** (il n'y a pas de diode entre l'USB et le Traco : ne jamais brancher l'USB quand la carte est sur la plaque alimentée). Un câble USB-C **data** (un câble de charge seule ne fait pas apparaître de port série).
2. **Une seule fois** : donner l'accès au port série à ton utilisateur.
   ```bash
   sudo usermod -aG dialout $USER
   ```
   Fermer la session et la rouvrir, puis vérifier que `dialout` apparaît dans :
   ```bash
   groups
   ```
3. Brancher la carte et trouver son port :
   ```bash
   ls /dev/ttyACM* /dev/ttyUSB*
   dmesg | tail
   ```
   L'USB natif de l'ESP32-S3 (USB-Serial-JTAG, GPIO19/20) apparaît en `/dev/ttyACM0`. C'est aussi la console : bootloader et application y écrivent, `idf.py monitor` y voit tout ; rien n'est envoyé sur UART0 (GPIO43/44). Une carte avec un second connecteur relié à une puce USB-série apparaît en `/dev/ttyUSB0` sur ce connecteur : il permet de flasher mais n'affiche pas les logs, utiliser le connecteur USB natif. `dmesg | tail` montre ce qui vient d'être détecté. Les commandes ci-dessous utilisent `/dev/ttyACM0` : adapter si besoin.
4. Activer ESP-IDF :
   ```bash
   . ~/esp/esp-idf/export.sh
   cd ~/CLionProjects/Doorman
   ```
5. **Clé de signature (une seule fois)**. D'abord vérifier si elle existe déjà :
   ```bash
   ls keys/signing_key.pem
   ```
   Si le fichier existe, **ne rien faire** : c'est la clé de la carte. Sinon seulement :
   ```bash
   idf.py secure-generate-signing-key --version 2 --scheme rsa3072 keys/signing_key.pem
   ```
   **La sauvegarder hors du PC** (gestionnaire de mots de passe, clé USB). **Ne jamais la régénérer ensuite** : la carte n'accepte que les images signées par cette clé ; avec une nouvelle clé, plus aucune OTA ne passe et il faut revenir à l'USB.
6. Configuration (voir « Compiler le firmware », étapes 2 et 3) : `idf.py menuconfig` → **Interphone** :
   - SSID et mot de passe du Wi-Fi **2,4 GHz** ;
   - token OTA d'au moins 32 caractères, généré par `openssl rand -hex 24` ;
   - mot de passe du SoftAP de secours (8 à 63 caractères).

   **Sauvegarder aussi `sdkconfig` (il contient le token et les mots de passe) avec la clé de signature, hors du PC.**
7. Compiler :
   ```bash
   idf.py build
   ```
   Doit finir par `Project build complete.` Une erreur `APP_OTA_TOKEN < 32 caracteres` (ou `APP_WIFI_SSID vide`, `APP_RECOVERY_AP_PASSWORD`) signifie que `menuconfig` n'est pas rempli.
8. Premier flash :
   ```bash
   idf.py -p /dev/ttyACM0 erase-flash flash monitor
   ```
   `erase-flash` efface **toute** la flash (NVS, deux emplacements OTA, etc.) : à faire **une seule fois**, la première fois. Le moniteur série s'ouvre ensuite ; **Ctrl+]** pour le quitter.
9. Si le flash ne démarre pas (`Failed to connect to ESP32-S3` / `No serial data received.`) : passer en mode téléchargement à la main. Maintenir **BOOT** (GPIO0 sur les cartes ESP32-S3), appuyer puis relâcher **RESET** (EN), relâcher **BOOT**, puis relancer la commande. Si ça échoue encore, essayer un débit plus bas : `idf.py -p /dev/ttyACM0 -b 115200 erase-flash flash monitor`.
10. Ce qu'on doit voir dans le moniteur au démarrage (dans cet ordre, entre autres lignes) :
    ```
    interphone-homekit 0.1.0 | partition ota_0 (valid) | reset usb
    station demarree, ssid '...', puissance max 52
    connecte, IP 192.168.x.y, rssi -NN
    serveur de logs sur le port 3333
    serveur HTTP sur le port 8080 (/info, /ota, /reboot)
    reception ADC sur GPIO9 (ADC1 canal 8, 20000 Hz) : bas < 1700 mV ... ; emission NON compilee (TX GPIO8 reste a 0)
    image deja validee, pas d'auto-test
    ```
    Après le premier flash, le bootloader marque lui-même `ota_0` comme `valid` : il n'y a pas d'auto-test. Le moniteur affiche aussi l'état de GPIO9 (réception, analogique : entrée numérique coupée, `Pullup: 0, Pulldown: 0`) et de GPIO8 (sortie).

    Puis tester depuis le PC :
    ```bash
    python3 tools/logs.py
    export OTA_TOKEN='le token saisi dans menuconfig'
    curl -H "Authorization: Bearer $OTA_TOKEN" http://interphone.local:8080/info
    ```
    Si `interphone.local` ne résout pas, utiliser l'IP affichée dans la ligne `connecte, IP ...`. Sous Linux, la résolution des noms `.local` demande le service Avahi : `sudo systemctl enable --now avahi-daemon` (paquet `avahi-daemon`, et `libnss-mdns` pour `curl`/`ping`).
11. **Reflash USB plus tard (récupération)** :
    ```bash
    idf.py -p /dev/ttyACM0 flash monitor
    ```
    Sans `erase-flash` : la NVS est conservée. `idf.py flash` écrit aussi `ota_data_initial.bin` sur la partition otadata (elle fait partie de `build/flash_args`), ce qui la réinitialise : la carte redémarre sur `ota_0`, l'image qu'on vient de flasher, même si elle tournait sur `ota_1` après des OTA. Pas besoin d'`erase-otadata`. Il faut la **même** clé de signature qu'au premier flash, sinon les OTA suivantes seront refusées.
12. Ensuite, les mises à jour se font par OTA (`tools/ota.sh`) : plus besoin d'USB.

## Ensuite, tout passe par le réseau

Le détail est dans `docs/utilisation.md`. L'essentiel :

```bash
export OTA_TOKEN=$(sed -n 's/^CONFIG_APP_OTA_TOKEN="\(.*\)"$/\1/p' sdkconfig)
python3 tools/logs.py                                                               # logs (port 3333)
curl -H "Authorization: Bearer $OTA_TOKEN" http://interphone.local:8080/info         # état
tools/ota.sh                                                                        # compile, envoie, suit les logs
curl -X POST -H "Authorization: Bearer $OTA_TOKEN" http://interphone.local:8080/reboot
```

`tools/ota.sh` lit `OTA_TOKEN` (sinon le token de `sdkconfig`) ; les ports se changent avec `HTTP_PORT` et `LOG_PORT` (défauts 8080 et 3333).

- Secours : si aucune connexion Wi-Fi n'est établie pendant 10 min, comptées depuis le démarrage ou depuis la perte d'une connexion établie, la carte ouvre le point d'accès `interphone-recovery`. S'y connecter, puis mêmes commandes sur `192.168.4.1` (`tools/ota.sh 192.168.4.1`, `python3 tools/logs.py 192.168.4.1`).
- Rollback automatique : une image qui ne passe pas son auto-test en 90 s revient seule à la précédente.
- `APP_BENCH_TEST` (menuconfig, OTA et auto-test) sert uniquement aux essais sur l'établi (délais courts du SoftAP) : ne jamais l'activer dans une image installée.

## Sécurité du bus

- Le bus est partagé par tout l'immeuble : une émission erronée dérange les voisins.
- GPIO8 (émission, `TC_BUS_TX_GPIO`) est forcé à 0 en toute première ligne de `app_main`.
- L'émission n'est compilée que si `TC_BUS_TX_ENABLE=y` (menuconfig, « Bus TC (tc_bus) » → « Compiler l'emission ») ; `sdkconfig.defaults` la laisse désactivée. Sans elle, aucun code d'émission n'est dans l'image et GPIO8 reste à 0.
- Seules les commandes `OPEN_DOOR`, `OPEN_DOOR_LONG` (Koch 60/70) et `LIGHT` sont autorisées (liste blanche dans `tc_proto.c`) ; `POST /bus/open-door` n'émet que le télégramme d'ouverture configuré, jamais un télégramme venu du réseau.
- Garde-fous à chaque émission (`tc_tx_policy.c`, testés sur PC) : 200 ms entre deux émissions, 5 par minute, bus calme depuis 200 ms, refus pendant une OTA et tant que le SoftAP de secours est actif. Le canal RMT a son niveau de repos à 0 avant d'être relié à GPIO8 ; en cas d'erreur, la broche est rendue au GPIO à 0 et l'émission coupée jusqu'au redémarrage.

## Crédits

Implémentation originale (MIT), fondée sur le travail de rétro-ingénierie de la communauté, utilisé comme référence technique :

- Doorman (AzonInc, GPL-3.0) : https://github.com/AzonInc/Doorman
- TCSintercomArduino (atc1441) : https://github.com/atc1441/TCSintercomArduino
- peteh/doorman (GPL-3.0) : https://github.com/peteh/doorman
- HomeKit : esp-homekit-sdk (Espressif) : https://github.com/espressif/esp-homekit-sdk

Licence : MIT (voir `LICENSE`).
