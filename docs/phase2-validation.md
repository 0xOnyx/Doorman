# Phase 2 : validation sur l'interphone réel

Date : 2026-10-16. Carte ESP32-S3 installée derrière l'interphone Koch, alimentée par la ligne P.
Version : **0.2.0** (émission compilée, `TC_BUS_OPEN_FORM_LONG`), récepteur ADC sur GPIO9, émission RMT sur GPIO8.

## Premier essai d'ouverture

Commande (utilisateur devant la porte de l'immeuble) :

```bash
curl -X POST -H "Authorization: Bearer $OTA_TOKEN" http://192.168.1.173:8080/bus/open-door
```

Réponse : `200 {"sent":true,"raw":"0x14DEF480","echo":true,"reply":"0x0001"}`

Logs de la carte :

```
TC RX OPEN_DOOR_LONG raw=0x14DEF480 (echo)
TC RX UNKNOWN raw=0x0001 ... (reponse a notre emission)
TX OPEN_DOOR_LONG raw=0x14DEF480 -> echo=oui reply=0x0001 (emission 106 ms, attente bus 0 ms)
```

**Résultat : la porte s'est ouverte.** L'écho relu est identique au télégramme émis, et la centrale a
répondu `0x0001`, comme pour une ouverture depuis l'écran Koch.

Avant l'émission, au repos, GPIO9 restait à 2,94–3,01 V : le transistor de GPIO8 ne tirait pas le bus.

## Non vérifié à ce stade

- Absence de perturbation chez les voisins (aucune remarque de l'utilisateur sur son propre poste).
- Refus d'un second appel dans les 3 s et limite de 5 émissions par minute sur le matériel (couverts
  par les tests PC uniquement).
- Sonnerie depuis la platine de rue (`DOOR_CALL`) : toujours à relever.

Suite : test de la sonnerie, puis phase 3 (HomeKit).
