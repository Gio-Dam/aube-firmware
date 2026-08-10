# Règles de développement pour le projet AUBE

Ce fichier définit les règles d'organisation de code et de propreté que tout agent IA doit impérativement respecter sur ce projet.

## 📂 Organisation Générale du Code

Le projet est divisé en deux parties principales :
1. **`aube-cloud/`** : L'application web de contrôle IoT (Next.js, Prisma, MQTT).
2. **`aube-firmware/`** : Le micrologiciel pour les lampes basées sur ESP32 (PlatformIO, C++ Arduino).

---

## ☁️ Règles pour `aube-cloud` (Next.js)

### 📌 Architecture & Structure
- **Actions Serveur (Server Actions)** : Ne JAMAIS créer ou laisser de fichier `actions.ts` ou `auth-actions.ts` directement sous `src/app/`. Toutes les server actions doivent être centralisées dans `src/actions/` par domaine (ex: `src/actions/lamp.ts`, `src/actions/auth.ts`, `src/actions/members.ts`).
- **Composants Métier (Lamps)** : Les composants spécifiques à la logique et au contrôle des lampes (ex: tableaux de bord, contrôleurs par modèle, gestion des membres de la lampe) doivent être placés dans `src/components/lamps/` (et non à la racine de `src/components/`).
- **Sélecteur de modèle (Factory Pattern)** : Le contrôleur principal est `src/components/lamps/LampController.tsx`. Il sert de routeur pour charger le bon composant selon le modèle de lampe (`hardwareModel`). Tout nouveau modèle doit y être enregistré.

---

## 🔌 Règles pour `aube-firmware` (PlatformIO)

### 📌 Compilation et Modèles
- **Organisation par dossiers** : Chaque modèle de lampe doit avoir son propre dossier source sous `src/` (ex: `src/c8_alpha/main.cpp`). Il ne doit plus y avoir de fichier `src/main.cpp` global.
- **Configuration Multi-environnements** : Tout nouveau modèle de lampe doit être configuré sous forme d'environnement distinct dans `platformio.ini` à l'aide de la directive `build_src_filter` (ex: `build_src_filter = +<nom_du_modele/>`).

---

## 🧹 Propreté & Scripts de Test (CRITIQUE)

Pour éviter de polluer le dépôt Git :
1. **Fichiers temporaires/tests** : Si vous écrivez des scripts de test temporaires (ex: scripts Node, utilitaires de test de connexion MQTT, etc.), placez-les exclusivement dans le dossier temporaire prévu pour la conversation dans les artefacts (`<appDataDir>/brain/<conversation-id>/scratch/`).
2. **Nettoyage automatique** : Une fois le test réussi et validé, vous devez **IMPERATIVEMENT supprimer** le script de test créé. Aucun script de test temporaire ou fichier de démonstration "à usage unique" ne doit être commité ou laissé dans les dossiers sources (`aube-cloud` ou `aube-firmware`).
3. **Fichiers de logs** : Ne jamais ajouter de fichiers de logs ou d'environnements locaux (`.env.local`, `.log`, etc.) dans les dossiers sources du dépôt Git.

*Vérifiez toujours que `bun run build` passe sans aucune erreur TypeScript après chaque modification.*
