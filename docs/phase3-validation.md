# Phase 3 : validation HomeKit sur l'interphone réel

Date : 2026-10-16. Version **0.3.0** installée par OTA, auto-test OK, HomeKit démarré (serveur HAP port 80).
Mémoire libre après démarrage : ~176 Ko (minimum ~164 Ko).

| Test | Résultat |
|---|---|
| Ajout dans l'app Maison (« Autres options », code d'appairage, « Ajouter quand même ») | OK : 2 contrôleurs appairés (iPhone + concentrateur) |
| Ouverture depuis la tuile « Porte » de Maison | **OK : la porte s'ouvre** |
| OTA et logs réseau pendant HomeKit | OK (8080 et 3333 inchangés) |
| Notification de sonnette depuis la platine de rue | **à tester** (le `DOOR_CALL` de cet interphone n'a pas encore été relevé) |
| Appairage conservé après une OTA | à tester à la prochaine mise à jour |
