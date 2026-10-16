# Guide d'utilisation

Ce guide explique comment se servir de l'interphone une fois le premier flash USB fait
(voir le README, section « Flasher la carte »). À partir de là, **tout passe par le réseau** :
logs, état, mises à jour et redémarrage.

État actuel : **phase 2**. La carte écoute le bus et journalise chaque télégramme ; une image
compilée avec l'émission (`TC_BUS_TX_ENABLE`) sait aussi ouvrir la porte de l'immeuble sur commande
(section 8). HomeKit n'est pas encore là (phase 3, livrée par OTA).

---

## 1. Préparer un terminal

À faire dans chaque nouveau terminal :

```bash
. ~/esp/esp-idf/export.sh          # ou : get_idf (si l'alias est installé)
cd ~/CLionProjects/Doorman
export OTA_TOKEN=$(sed -n 's/^CONFIG_APP_OTA_TOKEN="\(.*\)"$/\1/p' sdkconfig)
```

`OTA_TOKEN` est le token saisi dans `menuconfig`. Les commandes ci-dessous l'utilisent.
Ne le colle jamais dans un message ou un ticket : avec lui, on peut redémarrer la carte.

Adresse de la carte :

| Situation | Adresse |
|---|---|
| Normale (connectée à la box) | `interphone.local` |
| `interphone.local` ne répond pas | l'IP affichée dans les logs (`connecte, IP 192.168.x.y`) ou dans l'interface de la box |
| Point d'accès de secours | `192.168.4.1` (voir section 6) |

---

## 2. Lire les logs en direct

```bash
python3 tools/logs.py                    # interphone.local, port 3333
python3 tools/logs.py 192.168.1.42       # par IP
```

- À la connexion, tu reçois d'abord **l'historique récent** (environ 12 Ko, donc le démarrage
  si la carte vient de redémarrer), puis le flux en direct.
- Le script **se reconnecte tout seul** quand la carte redémarre (après une OTA par exemple).
- Un seul client à la fois : un second reçoit `busy: un seul client a la fois`. Un client disparu (PC en veille) libère la place en environ 25 s.
- `[netlog: N octets perdus]` : ton terminal n'a pas suivi le rythme, des lignes ont sauté.
  Ce n'est pas une panne.
- Sans Python : `nc interphone.local 3333`.

Ctrl+C pour quitter.

### Lignes importantes

| Ligne | Signification |
|---|---|
| `interphone-homekit 0.1.3 \| partition ota_1 (valid) \| reset software \| heap ...` | Démarrage : version, partition, état de l'image, cause du dernier redémarrage |
| `connecte, IP 192.168.1.42, rssi -61` | Wi-Fi connecté. Un RSSI en dessous de -80 indique un signal faible |
| `deconnecte (raison N), nouvel essai dans X s` | Wi-Fi perdu, nouvel essai automatique (délai qui double jusqu'à 60 s) |
| `HB up=...s heap=... rssi=... net=sta bus_ok=... bus_err=...` | Battement de cœur toutes les 5 min : la carte va bien |
| `HB pile libre (octets) ...` | Marge de pile de chaque tâche (doit rester au-dessus de 512) |
| `TC RX DOOR_CALL raw=0x0C30BA80 len=32 serial=0xC30BA addr=0 ...` | Télégramme lu sur le bus (voir section 7) ; ` << POUR MOI` en fin de ligne s'il concerne ton appartement |
| `numero de serie non configure : relever un DOOR_CALL ...` | Au démarrage : `TC_BUS_MY_SERIAL` n'est pas encore réglé (section 7) |
| `TC RX ERR parity ...` | Trame illisible (parasite, ou trame abîmée). Quelques-unes sont normales |
| `reception ADC sur GPIO9 (ADC1 canal 8, 20000 Hz) : bas < 1700 mV ...` | Au démarrage : le récepteur lit le bus en analogique (voir section 7) |
| `bus repos min=2930 max=2990 mV (20000 Hz)` | Toutes les 30 s quand le bus est resté au repos : tension mesurée sur la broche RX et cadence réelle de l'ADC. Rien de plus quand il y a eu du trafic (les lignes `TC RX` suffisent) |
| `ADC : reserve pleine, N trame(s) de conversion perdue(s)` | La tâche de réception a pris du retard (OTA en cours par exemple) : un télégramme a pu être manqué. Rare et sans gravité ; fréquent = à signaler |
| `relance de l'ADC (...)` | Le convertisseur ne donnait plus d'échantillons : la carte le relance seule. Une fois de temps en temps, sans gravité ; en boucle = à signaler |
| `emission ACTIVE sur GPIO8 (RMT 1 MHz, repos 0) : seul OPEN_DOOR_LONG raw=0x14DEF480 ...` | Au démarrage d'une image avec l'émission : le seul télégramme que la carte peut émettre (section 8) |
| `TX OPEN_DOOR_LONG raw=0x14DEF480 -> echo=oui reply=0x0001 ...` | Ouverture émise, relue sur le bus (écho) et acquittée par la centrale (section 8) |
| `TX ... refuse : too_soon` (ou `rate_limit`, `bus_busy`, `blocked`) | Ouverture refusée par un garde-fou, rien n'a été émis (section 8) |
| `nouvelle image en attente : auto-test (90 s max)` | Premier démarrage d'une nouvelle version |
| `SELFTEST OK, image validee` | La nouvelle version est gardée |
| `SELFTEST FAILED ... rollback` | La nouvelle version est rejetée, retour automatique à la précédente |
| `SoftAP de secours 'interphone-recovery' demarre` | Pas de Wi-Fi depuis 10 min : point d'accès de secours ouvert |
| `SANTE : ..., redemarrage` | Un service d'accès distant ne répond plus : la carte redémarre d'elle-même |

---

## 3. Consulter l'état de la carte

```bash
curl -H "Authorization: Bearer $OTA_TOKEN" http://interphone.local:8080/info
```

Exemple de réponse :

```json
{"version":"0.1.8","idf":"v5.5.5","partition":"ota_1","ota_state":"valid",
 "other_partition":"ota_0","other_state":"valid","uptime_s":86400,
 "heap_free":151234,"heap_min":140112,"rssi":-61,"reset_reason":"software",
 "net":"sta","bus_frames_ok":42,"bus_frames_err":3,
 "chip_temp_c":41,"rx_gpio":9,"rx_mv":2955,
 "bus":{"ok":42,"err":3,"for_me":2,"my_serial":"0xC30BA","door_call":5,"internal_call":0,
        "floor_call":1,"open_door":2,"open_door_long":0,"light":1,"unknown":4,
        "last_raw":"0x1100","last_type":"OPEN_DOOR","last_age_s":12},
 "tx":{"enabled":true,"form":"long","raw":"0x14DEF480","sent":1,"echo":1,"reply":1,"refused":0,
       "last_age_s":300}}
```

| Champ | À surveiller |
|---|---|
| `version` | La version en cours (celle de `version.txt` au moment du build) |
| `ota_state` | `valid` en temps normal ; `pending_verify` pendant les 90 s qui suivent une OTA |
| `other_state` | État de l'autre emplacement : `invalid` ou `aborted` après un rollback |
| `reset_reason` | `poweron` (coupure de courant), `software` (OTA ou reboot), `panic`/`task_wdt` (plantage, à signaler), `brownout` (alimentation trop faible) |
| `heap_min` | Mémoire libre minimale depuis le démarrage ; doit rester au-dessus de 50 000 |
| `net` | `sta` (box) ou `recovery` (point d'accès de secours) |
| `bus_frames_ok` / `bus_frames_err` | Trames lues / rejetées depuis le démarrage (repris dans `bus.ok` / `bus.err`) |
| `rx_gpio` / `rx_mv` | Broche de réception et dernière tension mesurée dessus, en mV : ~2950 au repos. `-1` si inconnue (récepteur RMT) |
| `bus` | Activité du bus depuis le démarrage, détaillée en section 7 |
| `tx` | Émission : `{"enabled":false}` si l'image ne la compile pas ; sinon détaillée en section 8 |

Réponses d'erreur : `401 unauthorized` (token absent ou faux), `503` (la carte a été compilée
sans token valide, ce que le build refuse désormais).

---

## 4. Mettre à jour le firmware (OTA)

1. Modifier le code ou la configuration (`idf.py menuconfig`).
2. **Incrémenter `version.txt`** (par exemple `0.1.3` → `0.1.4`). Sans ça, tu ne pourras pas
   voir dans `/info` que la nouvelle version tourne.
3. Lancer :

```bash
tools/ota.sh                     # compile, envoie, puis suit les logs
tools/ota.sh 192.168.4.1         # via le point d'accès de secours
```

Ce que tu dois voir :

```
{"status":"ok","reboot_in_ms":1000}
--- connecte a interphone.local:3333 ---
... interphone-homekit 0.1.4 | partition ota_0 (pending_verify) ...
... nouvelle image en attente : auto-test (90 s max)
... SELFTEST OK, image validee
```

Réponses possibles de la carte :

| Réponse | Cause | Que faire |
|---|---|---|
| `200 {"status":"ok"...}` | Image acceptée, redémarrage dans 1 s | Suivre les logs jusqu'à `SELFTEST OK` |
| `400 image invalid or unsigned` | Image non signée, abîmée, ou signée avec une autre clé | Vérifier que `keys/signing_key.pem` est **la même** clé que celle du premier flash |
| `400 not a firmware image` | Mauvais fichier envoyé | Envoyer `build/interphone-homekit.bin` |
| `401 unauthorized` | Token faux | Vérifier `OTA_TOKEN` |
| `409 self-test pending` | La version précédente est encore en auto-test | Attendre `SELFTEST OK` (90 s max) et réessayer |
| `409 ota already in progress` | Une OTA est déjà en cours | Attendre |
| `413` | Fichier trop gros (> 1600 Ko) ou envoyé sans taille | Vérifier le fichier |
| `500 receive timeout` / `transfer too slow` | Réseau trop lent ou coupé pendant l'envoi | Réessayer ; la carte n'a pas changé de version |

Une OTA ratée ne casse rien : la carte continue sur la version en place.

---

## 5. Redémarrer la carte à distance

```bash
curl -X POST -H "Authorization: Bearer $OTA_TOKEN" http://interphone.local:8080/reboot
```

Réponse `{"status":"rebooting","reboot_in_ms":1000}`. La carte revient en une dizaine de secondes.

---

## 6. Si la carte ne se connecte plus au Wi-Fi

Cas typiques : nouvelle box, mot de passe Wi-Fi changé.

1. **Attendre 10 minutes.** Sans Wi-Fi pendant 10 min (depuis le démarrage, ou depuis la perte
   d'une connexion établie), la carte ouvre un point d'accès de secours :
   - nom du réseau : `interphone-recovery`
   - mot de passe : celui saisi dans `menuconfig` (`APP_RECOVERY_AP_PASSWORD`)
2. Connecter le PC à ce réseau. La carte est alors à l'adresse `192.168.4.1` :
   ```bash
   python3 tools/logs.py 192.168.4.1
   curl -H "Authorization: Bearer $OTA_TOKEN" http://192.168.4.1:8080/info     # "net":"recovery"
   ```
3. Mettre les nouveaux identifiants dans `idf.py menuconfig`, incrémenter `version.txt`, puis :
   ```bash
   tools/ota.sh 192.168.4.1
   ```
4. La carte redémarre, rejoint la nouvelle box et coupe le point d'accès de secours.
   Reconnecter le PC à la box.

Pendant ce temps la carte continue d'essayer la box (au plus toutes les 60 s) : chaque essai
coupe la radio 1 à 2 s, une petite pause dans les logs est normale.

Si le point d'accès de secours n'apparaît jamais : il ne reste que l'USB (section 10).

---

## 7. Lire les télégrammes du bus (sniffer)

Chaque trame lue donne une ligne :

```
TC RX DOOR_CALL raw=0x0C30BA80 len=32 serial=0xC30BA addr=0 payload=0x1 chk=ok
```

| Type | Signification |
|---|---|
| `DOOR_CALL` | Quelqu'un sonne depuis la platine de rue ; `serial` = l'appartement appelé |
| `INTERNAL_CALL` | Appel interne entre appartements |
| `FLOOR_CALL` | Sonnette palière (devant la porte de l'appartement) |
| `OPEN_DOOR` | Ouverture de porte (forme courte, `raw=0x11xx`) |
| `OPEN_DOOR_LONG` | Ouverture de porte (forme longue des Koch 60/70, avec le `serial`) |
| `START_TALKING` / `STOP_TALKING` | Début / fin de communication |
| `LIGHT` | Lumière d'escalier |
| `ACK` (`rsp`) | Accusé de réception |
| `UNKNOWN` | Type non reconnu : noter le `raw`, c'est utile pour la suite |

### Relever tes valeurs (phase 1)

1. Lancer `python3 tools/logs.py`.
2. **Sonner chez toi depuis la platine de rue.** Noter la ligne `DOOR_CALL` : son `serial` est
   le numéro de ta station intérieure. (Les `DOOR_CALL` avec un autre `serial` sont les voisins.)
3. **Appuyer sur « ouvrir » sur ton écran Koch.** Noter la ligne `OPEN_DOOR` ou
   `OPEN_DOOR_LONG`, son `raw` complet et son `addr`.
4. Régler le numéro de série comme indiqué ci-dessous, et garder le `raw` et l'`addr` de l'ouverture : ils serviront à la phase 2 (émission).

### Régler ton numéro de série

1. `idf.py menuconfig` → menu **« Bus TC (tc_bus) »** :
   - **Numero de serie de la station interieure** (`TC_BUS_MY_SERIAL`) : le `serial` relevé
     sur ton `DOOR_CALL`, en hexadécimal (par exemple `0xC30BA`). `0` = non réglé.
   - **Adresse de la platine de rue** (`TC_BUS_DOOR_ADDRESS`) : l'`addr` relevé sur
     l'`OPEN_DOOR` court (souvent `0`).
2. **Incrémenter `version.txt`**, puis livrer par OTA : `tools/ota.sh` (section 4).
3. Au démarrage, les logs affichent `numero de serie configure : 0xC30BA, platine adresse 0`
   (au lieu de `numero de serie non configure ...`).

### Le repère `<< POUR MOI`

Une fois le numéro réglé, les lignes qui concernent ton appartement se terminent par ` << POUR MOI` :

```
TC RX DOOR_CALL raw=0x0C30BA80 len=32 serial=0xC30BA addr=0 payload=0x1 chk=ok << POUR MOI
TC RX OPEN_DOOR raw=0x1100 len=16 serial=0x00000 addr=0 payload=0x0 chk=ok << POUR MOI
```

- `DOOR_CALL`, `INTERNAL_CALL`, `FLOOR_CALL`, `START_TALKING`, `CONTROL_FUNCTION`,
  `OPEN_DOOR_LONG`, `REQUEST_VERSION` : marqués quand leur `serial` est le tien.
- `OPEN_DOOR` (forme courte) : il ne porte pas de numéro de série. Il n'est marqué que s'il vise
  l'adresse de ta platine **et** arrive moins de 3 minutes après un appel pour toi
  (`DOOR_CALL`, sonnerie depuis la platine de rue) ; sinon toute ouverture de l'immeuble serait marquée.

### L'objet `bus` de `/info`

| Champ | Signification |
|---|---|
| `ok` / `err` | Trames valides / rejetées depuis le démarrage |
| `for_me` | Trames marquées `<< POUR MOI` |
| `my_serial` | Numéro configuré, ou `"unset"` |
| `door_call`, `internal_call`, `floor_call`, `open_door`, `open_door_long`, `light`, `unknown` | Trames valides de chaque type |
| `last_raw` / `last_type` / `last_age_s` | Dernière trame valide et son âge en secondes ; `null` si aucune |

Pour vérifier à distance qu'une sonnerie a bien été vue : `for_me` et `door_call` augmentent,
`last_type` vaut `DOOR_CALL` avec un `last_age_s` faible.

Le compteur `bus_frames_err` qui monte un peu est normal. S'il monte vite (plusieurs par seconde
en continu), le câblage ou le niveau du bus est à vérifier (tension au repos d'environ 2,95 V sur
GPIO9 : `rx_mv` de `/info` ou ligne `bus repos` des logs).

### Le récepteur analogique

Le bus est lu **en analogique** : l'ADC échantillonne la broche RX (GPIO9, derrière le diviseur
1 MΩ / 147 kΩ) 20 000 fois par seconde. Sur la carte installée, la broche est à 2,94–2,98 V au
repos et ne descend qu'à ~0,86 V pendant un télégramme : l'entrée numérique ne voit pas ces bas
de façon fiable, d'où les seuils logiciels avec hystérésis :

- le bus passe « bas » quand la broche descend sous `TC_BUS_ADC_LOW_MV` (1700 mV par défaut) ;
- il redevient « haut » quand elle remonte à `TC_BUS_ADC_HIGH_MV` (2100 mV par défaut) ou plus.

À ajuster seulement si les niveaux changent (autre câblage, autre bus) : relever le repos (ligne
`bus repos ... max=`) et le bas d'un télégramme, puis placer les deux seuils entre les deux,
nettement à l'écart de chacun (`idf.py menuconfig` → **« Bus TC (tc_bus) »**, puis incrémenter
`version.txt` et livrer par OTA). Symptômes : aucun `TC RX` alors que le bus est actif (seuil bas
trop bas, ou repos sous le seuil haut), ou beaucoup de `TC RX ERR` (seuils trop près du bruit).
L'ancien récepteur numérique reste disponible (**Recepteur du bus** → RMT) pour un autre câblage.

---

## 8. Ouvrir la porte depuis la carte (phase 2)

Commande **temporaire** (elle sera remplacée par HomeKit en phase 4) : elle n'existe que dans une
image compilée avec l'émission (`idf.py menuconfig` → **« Bus TC (tc_bus) »** → **« Compiler
l'emission »**). Sans elle, `/bus/open-door` répond `404` et `/info` montre `"tx":{"enabled":false}`.

```bash
curl -X POST -H "Authorization: Bearer $OTA_TOKEN" http://interphone.local:8080/bus/open-door
```

Aucun paramètre : le corps de la requête est ignoré, la carte n'émet **que** le télégramme
d'ouverture choisi dans menuconfig (**« Telegramme d'ouverture emis »**) :

- **Long** (par défaut) : `OPEN_DOOR_LONG` avec ton numéro de série, `0x14DEF480` pour la série
  `0x4DEF4` et la platine 0. C'est ce que ton poste envoie hors communication. Le build refuse
  cette forme si `TC_BUS_MY_SERIAL` vaut 0.
- **Court** : `OPEN_DOOR` `0x1100` (+ adresse de la platine), ce que le poste envoie pendant une
  communication.

La commande est synchrone (quelques secondes au plus) : la carte attend que le bus soit calme,
émet, puis relit son propre télégramme (l'**écho**) et la réponse de la centrale.

| Réponse | Signification |
|---|---|
| `200 {"sent":true,"raw":"0x14DEF480","echo":true,"reply":"0x0001"}` | Émis. `echo` : relu à l'identique sur le bus ; `reply` : réponse de la centrale (`0x0001` après une ouverture), `null` si aucune dans les 500 ms |
| `409 {"sent":false,"reason":"too_soon"}` | Une ouverture a déjà été émise il y a moins de 3 s (un `curl` répété ne rouvre pas la porte) |
| `409 {"sent":false,"reason":"rate_limit"}` | Déjà 5 émissions dans la dernière minute : attendre |
| `503 {"sent":false,"reason":"bus_busy"}` | Le bus n'a pas été calme 200 ms de suite pendant 2 s (une communication est en cours) |
| `503 {"sent":false,"reason":"blocked"}` | OTA en cours, point d'accès de secours actif, ou réception du bus arrêtée |
| `500 {"sent":false,"reason":"rmt_error",...}` | L'émetteur a échoué : il est coupé (GPIO8 remis à 0) jusqu'au prochain redémarrage |
| `401 unauthorized` | Token absent ou faux |

`"sent":true` avec `"echo":false` : le télégramme est parti mais la carte ne l'a pas relu ; si la
porte ne s'est pas ouverte, regarder les logs (`TC RX ERR` juste après la ligne `TX`).

**Garde-fous** (le bus est partagé par tout l'immeuble) :

- seuls `OPEN_DOOR`, `OPEN_DOOR_LONG` et `LIGHT` peuvent être émis (liste blanche, testée sur PC),
  et la commande HTTP ne construit que le télégramme d'ouverture configuré ;
- 200 ms au moins entre deux émissions, 5 au plus par minute glissante ;
- bus calme depuis 200 ms avant d'émettre (attente de 2 s au plus) ;
- émission refusée pendant une OTA et tant que le point d'accès de secours est actif ;
- GPIO8 reste à 0 en dehors des 150 ms d'un télégramme ; en cas d'erreur de l'émetteur, la broche
  est rendue à 0 et l'émission coupée jusqu'au redémarrage.

Dans `/info`, l'objet `tx` :

| Champ | Signification |
|---|---|
| `enabled` | `true` si l'image compile l'émission |
| `form` / `raw` | Forme (`long` ou `short`) et télégramme d'ouverture configuré |
| `sent` / `echo` / `reply` / `refused` | Émissions, échos relus, réponses de la centrale, demandes refusées depuis le démarrage |
| `last_age_s` | Âge de la dernière émission en secondes, `null` si aucune |

Dans les logs, l'écho est marqué `(echo)` et n'est ni compté « pour moi » ni transmis plus loin ;
la réponse de la centrale est suivie de `(reponse a notre emission)`.

---

## 9. Ce que la carte fait toute seule

- **Rollback** : une nouvelle version qui n'obtient pas le réseau, le serveur de logs, le
  serveur HTTP et la réception du bus en 90 s est rejetée ; la carte redémarre sur la précédente.
  Même chose si elle plante ou si le courant est coupé avant sa validation.
- **Reconnexion Wi-Fi** : automatique, sans jamais redémarrer pour ça.
- **Santé** : si le serveur HTTP ou le serveur de logs meurt, ou s'il n'y a plus aucun accès
  réseau (ni box ni secours) pendant 15 min, la carte redémarre d'elle-même.
- **Bus** : la carte n'émet jamais rien d'elle-même. Sans l'émission compilée, la sortie GPIO8
  reste à 0 ; avec, elle n'émet que sur `POST /bus/open-door` (section 8).

---

## 10. Dernier recours : l'USB

Si plus rien ne répond (ni `interphone.local`, ni le point d'accès de secours après 10 min) :

1. Débrancher la carte de la plaque d'interface, la brancher au PC en USB.
2. `idf.py -p /dev/ttyACM0 monitor` pour lire ce qui se passe (Ctrl+] pour quitter).
3. Reflasher : `idf.py -p /dev/ttyACM0 flash monitor` (garde la configuration de la carte).
   Il faut **la même clé** `keys/signing_key.pem` et un `sdkconfig` correct.

---

## 11. À sauvegarder hors du PC

| Fichier | Pourquoi |
|---|---|
| `keys/signing_key.pem` | Sans elle, plus aucune OTA n'est acceptée : retour obligatoire à l'USB |
| `sdkconfig` | Contient le token OTA et les mots de passe ; sans lui il faut tout ressaisir |

Ne les committe jamais dans Git (ils sont dans `.gitignore`).
