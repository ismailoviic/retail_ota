AI Retail Bot - Firmware (V14)

Ce dépôt contient le code source du firmware embarqué pour le dispositif IoT "AI Retail Bot", conçu par ESS Technologies. Ce dispositif est destiné à surveiller le niveau de grains de café dans les trémies de machines professionnelles, avec une autonomie sur batterie optimisée.

🌟 Fonctionnalités Principales

Ultra-Low Power (Deep Sleep) : Le microcontrôleur (ESP32) passe 98% de son temps en veille profonde (Deep Sleep) et ne se réveille que toutes les 10 minutes pour prendre une mesure, garantissant plusieurs semaines d'autonomie avec une seule cellule Li-Ion.

Capteur Time-of-Flight (VL53L0X) avec Filtre Médian : Mesure optique de haute précision. Le firmware (depuis la V14) intègre un filtre médian qui prend 3 mesures rapides et rejette automatiquement les anomalies (ex: un grain qui tombe) pour garantir une lecture parfaitement stable.

Télémétrie Complète & Calibrée : Mesure du niveau de café, de la tension exacte de la batterie (calibrée avec un offset logiciel et filtrée) via GPIO 35, et détection matérielle de la connexion au chargeur USB via GPIO 34.

Transmission Sécurisée : Envoi des données chiffrées via HTTPS POST vers une base de données Supabase.

Portail Captif (Provisioning Wi-Fi) : Configuration du Wi-Fi par l'utilisateur final sans aucune application tierce (via WiFiManager).

Auto-Recovery & Sécurité Énergétique (V14) : Si le mode de configuration Wi-Fi reste inactif pendant 3 minutes, l'appareil s'endort automatiquement. Il se réveillera toutes les 10 minutes pour scanner le réseau pendant 15 secondes. Si la connexion est rétablie (ex: redémarrage du routeur du café), il reprend son fonctionnement normal sans intervention humaine, tout en préservant sa batterie.

Mises à jour OTA (Over-The-Air) : Mise à jour automatique du firmware à distance via ce dépôt GitHub.

🛠️ Instructions pour les Développeurs : Mises à Jour OTA

L'appareil vérifie automatiquement les mises à jour à chaque fois qu'il se réveille et se connecte au Wi-Fi. Pour déployer une nouvelle version du firmware sur tous les appareils déployés :

Étape 1 : Compiler le nouveau Firmware

Ouvrez le fichier ai_retail_firmware.ino dans l'IDE Arduino.

TRÈS IMPORTANT : Modifiez la variable currentVersion et incrémentez le numéro (ex: passez de 14 à 15). Si vous oubliez cela, l'appareil téléchargera la mise à jour en boucle.

Allez dans le menu Croquis > Exporter les binaires compilés.

L'IDE génèrera un fichier .bin.

Étape 2 : Déployer sur GitHub

Renommez votre fichier .bin si nécessaire pour qu'il corresponde exactement au chemin attendu par l'appareil (ex: retail_ota.ino.bin).

Uploadez ce nouveau fichier .bin dans votre dépôt GitHub (dans le dossier build/esp32.esp32.esp32/).

Ouvrez le fichier version.txt situé à la racine du dépôt.

Modifiez le chiffre à l'intérieur pour qu'il corresponde à votre nouvelle version (ex: remplacez 14 par 15), puis validez (Commit).

👉 Lors de son prochain réveil (dans les 10 minutes maximum), l'appareil lira le fichier version.txt, détectera la nouvelle version, téléchargera le binaire, et redémarrera avec le nouveau code.

📡 Instructions Utilisateur : Configuration Wi-Fi

Si vous installez l'appareil pour la première fois ou si le mot de passe Wi-Fi du café a changé, suivez cette procédure simple (moins d'une minute) :

1. Comprendre la LED Bleue

L'appareil communique son état via une LED bleue :

🔵 Clignotement rapide : Recherche du réseau Wi-Fi connu (durée : 15 secondes).

🔵 Allumée Fixe : Mode Configuration activé (Attente du mot de passe).

⚫ Éteinte : Appareil connecté et en veille (Fonctionnement normal).

2. Connecter l'appareil

Allumez l'appareil avec l'interrupteur.

Attendez 15 secondes que la LED bleue devienne fixe.

Sur votre smartphone, connectez-vous au réseau Wi-Fi nommé : AI Coffee

Le mot de passe de ce réseau est : 12345678

Une page web de configuration s'ouvrira automatiquement (si ce n'est pas le cas, ouvrez votre navigateur et allez sur 192.168.4.1).

Cliquez sur Configure WiFi, sélectionnez le Wi-Fi de votre café, entrez son mot de passe, et sauvegardez.

La LED s'éteindra. L'appareil est opérationnel !

⚠️ Délai de Sécurité : Le mode configuration (LED fixe) ne dure que 3 minutes. Si vous dépassez ce délai, l'appareil s'endormira. Il se réveillera tout seul plus tard pour retenter sa chance. Pour recommencer immédiatement la configuration, éteignez puis rallumez l'interrupteur.

🔌 Câblage Matériel (Référence)

VL53L0X : SDA -> GPIO 21 | SCL -> GPIO 22 | VCC -> 3V3

Batterie (Diviseur 10k/10k) : Centre -> GPIO 35 (ADC1)

Détection Charge USB (Diviseur 10k/10k) : Centre -> GPIO 34 (ADC1)