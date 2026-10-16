# Phase 1 : réception du bus sur la carte installée

## 2026-10-16 : passage au récepteur analogique (0.1.13)

Câblage réel : réception **GPIO9** (diviseur 1 MΩ / 147 kΩ depuis le bus), émission **GPIO8**
(base du transistor, jamais activée : émission non compilée).

### Niveaux mesurés sur la broche RX

| Mesure | Valeur |
|---|---|
| Repos | 2,94 à 2,98 V |
| Point le plus bas pendant un télégramme | 0,86 V |
| Durées des segments (ADC, seuils 1700 / 2100 mV) | ~2000 / 4000 / 5900 µs, ± 60 µs |

### Constat

- **L'entrée numérique ne lit pas ce bus** : avec le récepteur RMT (entrée logique de l'ESP32-S3),
  les bas à ~0,86 V ne sont pas vus de façon fiable et aucun télégramme n'est décodé.
- **L'ADC les lit** : un mode de diagnostic (lecture ADC en boucle, seuils avec hystérésis
  1700 mV / 2100 mV, `TC_BUS_ADC_PROBE`, retiré depuis) a décodé le télégramme réel
  `OPEN_DOOR_LONG raw=0x14DEF480 serial=0x4DEF4 addr=0`, parité correcte, suivi d'un court
  `0x0001`. Le numéro de série de la station intérieure est donc **0x4DEF4**
  (`TC_BUS_MY_SERIAL`).

### Conséquence

Le récepteur par défaut devient l'**ADC continu** (DMA, 20 kHz, `TC_BUS_RX_ADC`) : mêmes seuils
et même décodage que le diagnostic, sans boucle active (la tâche dort entre deux blocs de 5 ms,
le watchdog de tâches reste entièrement actif). Le décodage du signal échantillonné est testé
sur PC (`test/host/test_tc_slicer.c`) avec un signal synthétique reprenant les niveaux mesurés.
Le récepteur RMT reste disponible (`TC_BUS_RX_RMT`) pour un autre câblage.

### Restant

- **Sonnerie depuis la platine de rue non testée** : vérifier qu'un `DOOR_CALL` avec
  `serial=0x4DEF4` est lu et marqué `<< POUR MOI` avec le récepteur ADC.
- Relever l'`OPEN_DOOR` court et son `addr` après un appel (fenêtre de 3 minutes).
- Surveiller `bus_frames_err`, les lignes `bus repos` (tension et cadence réelle de l'ADC) et
  l'absence de `ADC : reserve pleine` en fonctionnement normal.
