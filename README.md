# AI Retail Bot - Firmware (V13)

Ce dépôt contient le code source du firmware embarqué pour le dispositif IoT "AI Retail Bot", conçu par **ESS Technologies**. Ce dispositif est destiné à surveiller le niveau de grains de café dans les trémies de machines professionnelles, avec une autonomie sur batterie optimisée.

## 🌟 Fonctionnalités Principales

* **Ultra-Low Power (Deep Sleep) :** Le microcontrôleur (ESP32) passe 98% de son temps en veille profonde (Deep Sleep) et ne se réveille que toutes les 10 minutes pour prendre une mesure, garantissant plusieurs semaines d'autonomie avec une seule cellule Li-Ion.
* **Capteur Time-of-Flight (VL53L0X) :** Mesure optique de haute précision, insensible aux bruits acoustiques et aux formes irrégulières des grains de café.
* **Télémétrie Complète :** Mesure du niveau de café, de la tension exacte de la batterie (via GPIO 35), et détection matérielle de la connexion au chargeur USB (via GPIO 34).
* **Transmission Sécurisée :** Envoi des données chiffrées via HTTPS POST vers une base de données **Supabase**.
* **Portail Captif (Provisioning Wi-Fi) :** Configuration du Wi-Fi par l'utilisateur final sans aucune application tierce (via `WiFiManager`).
* **Sécurité Énergétique :** Si le mode de configuration Wi-Fi reste inactif pendant 5 minutes, l'appareil s'éteint complètement (veille infinie) pour ne pas détruire la batterie.
* **Mises à jour OTA (Over-The-Air) :** Mise à jour automatique du firmware à distance via ce dépôt GitHub.

---

## 🛠️ Instructions pour les Développeurs : Mises à Jour OTA

L'appareil vérifie automatiquement les mises à jour à chaque fois qu'il se réveille et se connecte au Wi-Fi. Pour déployer une nouvelle version du firmware sur tous les appareils déployés :

### Étape 1 : Compiler le nouveau Firmware
1. Ouvrez le fichier `ai_retail_firmware.ino` dans l'IDE Arduino.
2. **TRÈS IMPORTANT :** Modifiez la variable `currentVersion` et incrémentez le numéro (ex: passez de `13` à `14`). *Si vous oubliez cela, l'appareil téléchargera la mise à jour en boucle.*
3. Allez dans le menu **Croquis** > **Exporter les binaires compilés**.
4. L'IDE génèrera un fichier `.bin`.

### Étape 2 : Déployer sur GitHub
1. Renommez votre fichier `.bin` si nécessaire pour qu'il corresponde exactement au chemin attendu par l'appareil (ex: `retail_ota.ino.bin`).
2. Uploadez ce nouveau fichier `.bin` dans votre dépôt GitHub (dans le dossier `build/esp32.esp32.esp32/`).
3. Ouvrez le fichier `version.txt` situé à la racine du dépôt.
4. Modifiez le chiffre à l'intérieur pour qu'il corresponde à votre nouvelle version (ex: remplacez `13` par `14`), puis validez (Commit).

*👉 Lors de son prochain réveil (dans les 10 minutes maximum), l'appareil lira le fichier `version.txt`, détectera la nouvelle version, téléchargera le binaire, et redémarrera avec le nouveau code.*

---

## 📡 Instructions Utilisateur : Configuration Wi-Fi

Si vous installez l'appareil pour la première fois ou si le mot de passe Wi-Fi du café a changé, suivez cette procédure simple (moins d'une minute) :

### 1. Comprendre la LED Bleue
L'appareil communique son état via une LED bleue :
* 🔵 **Clignotement rapide :** Recherche du réseau Wi-Fi connu (durée : 15 secondes).
* 🔵 **Allumée Fixe :** Mode Configuration activé (Attente du mot de passe).
* ⚫ **Éteinte :** Appareil connecté et en veille (Fonctionnement normal) ou mis en sécurité après 5 minutes d'inactivité.

### 2. Connecter l'appareil
1. Allumez l'appareil avec l'interrupteur.
2. Attendez 15 secondes que la LED bleue devienne **fixe**.
3. Sur votre smartphone, connectez-vous au réseau Wi-Fi nommé : **`AI Coffee`**
4. Le mot de passe de ce réseau est : **`12345678`**
5. Une page web de configuration s'ouvrira automatiquement (si ce n'est pas le cas, ouvrez votre navigateur et allez sur `192.168.4.1`).
6. Cliquez sur **Configure WiFi**, sélectionnez le Wi-Fi de votre café, entrez son mot de passe, et sauvegardez.
7. La LED s'éteindra. L'appareil est opérationnel !

⚠️ **Délai de Sécurité :** Le mode configuration (LED fixe) ne dure que **5 minutes**. Si vous dépassez ce délai, l'appareil s'endormira définitivement pour protéger sa batterie. Pour recommencer, éteignez puis rallumez l'interrupteur.
