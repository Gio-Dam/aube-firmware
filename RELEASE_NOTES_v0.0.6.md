# Release Notes - Firmware AUBE v0.0.6 (Hotfix)

## Corrections Majeures (OTA)
Cette version finale de la v0.0.6 contient tous les correctifs nécessaires pour assurer des mises à jour à distance (OTA) 100% fiables et autonomes :

- **Correction de la "Boucle Infinie" au démarrage (Hotfix)** : La lampe informe désormais de manière proactive l'API Cloud de sa version interne *exacte* (ex: `&version=v0.0.6`) lors de la vérification OTA au démarrage. Cela empêche la lampe de se mettre à jour en boucle si la base de données n'avait pas encore eu le temps d'enregistrer la nouvelle version via MQTT.
- **Fix Erreur HTTP 400 (S3)** : L'API `aube-cloud` pré-résout désormais les liens de téléchargement GitHub vers les serveurs Amazon S3, évitant ainsi à l'ESP32 de gérer les redirections complexes.
- **Refacto `HTTPUpdate`** : Intégration de la librairie officielle Espressif pour l'OTA, rendant le téléchargement et l'écriture en mémoire beaucoup plus stables.

## Déploiement
**Action requise pour la v0.0.6** : 
Veuillez vous assurer que le fichier `.bin` uploadé dans cette release a bien été compilé *après* l'intégration de ces correctifs. Si une lampe est actuellement coincée dans une boucle de mise à jour, elle en sortira automatiquement dès que le nouveau binaire (contenant l'envoi de sa version dans l'URL) sera publié ici.
