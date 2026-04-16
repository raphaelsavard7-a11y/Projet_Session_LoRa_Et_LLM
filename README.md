# 🦄 Projet Session — LoRa & LLM

> **Système IoT bidirectionnel** utilisant deux LilyGO T-Beam S3 Supreme communicant par **LoRa**, avec intelligence artificielle embarquée (**LLM**) et publication **MQTT** en temps réel.

```
┌─────────────────┐         868 MHz LoRa          ┌─────────────────────────┐
│   ÉMETTEUR      │  ─────────────────────────►   │      RÉCEPTEUR          │
│                 │    {"id":1,"pot":2048,"pct":50}│                         │
│  Potentiomètre  │                                │  WiFi ──► LLM (NanoGPT)│
│     (IO2)       │                                │             │           │
│                 │  ◄─────────────────────────    │  MQTT pub ◄─┤           │
│  LED Rouge (IO3)│    {"msg":"...","led":"BLINK"} │             │           │
│  LED Bleue (IO2)│                                │  LED Bleue (IO2)        │
│                 │                                │                         │
│  Bouton (IO0)   │                                │  Écran OLED SH1106      │
│  Écran OLED     │                                │  (texte LLM complet)    │
└─────────────────┘                                └─────────────────────────┘
```

## ✨ Fonctionnalités

| Fonctionnalité | Description |
|---|---|
| **LoRa Bidirectionnel** | Communication 868 MHz via SX1262 (RadioLib), protocole JSON, Sync Word `0x67` |
| **LLM Intégré** | Appel API NanoGPT — personnalité « Stella la licorne » qui réagit au potentiomètre |
| **MQTT en temps réel** | Publication WebSocket sur broker MQTT avec état complet du système |
| **Contrôle LED intelligent** | LEDs PWM sur les deux boards avec effet de respiration sinusoïdal |
| **Écran OLED** | Dashboard sur émetteur (pot + statut) et texte LLM complet sur récepteur |
| **WiFi WPA2 Entreprise** | Support EAP-PEAP pour réseaux institutionnels |

## 🧠 Comment ça marche

1. **L'utilisateur** appuie sur le bouton (IO0) du **sender**
2. Le **sender** lit le potentiomètre et envoie la valeur par **LoRa**
3. Le **receiver** reçoit, appelle le **LLM** avec la valeur du potentiomètre
4. Le LLM (Stella 🦄) décide de l'état de la LED (`ON` / `OFF` / `BLINK`) et génère un message poétique
5. Le **receiver** publie le résultat sur **MQTT** et renvoie la décision par **LoRa**
6. Le **sender** reçoit et contrôle sa LED en conséquence
7. Les deux LEDs (sender + receiver) reflètent la décision de l'IA

## 📁 Structure du projet

```
├── LoRaSender_Final/          # Code Arduino — Émetteur
│   └── LoRaSender_Final.ino
│
├── LoRaReceiver_Final/        # Code Arduino — Récepteur
│   ├── LoRaReceiver_Final.ino
│   ├── auth.h.example         # ⬅ Copier en auth.h et remplir
│   └── auth.h                 # (ignoré par git)
│
├── LoRaSender_Example/        # Code exemple de référence
├── LoRaReceiver_Example/      # Code exemple de référence
└── WifiAndLLMAndPotentiometer/# Prototype WiFi + LLM + Pot
```

## 🚀 Installation

### Prérequis

- **Arduino IDE** ou **arduino-cli**
- **Board** : ESP32 par Espressif (`esp32:esp32` v3.2.0+)
- **FQBN** : `esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi`

### Librairies requises

| Librairie | Version | Usage |
|---|---|---|
| RadioLib | 7.6.0+ | Communication LoRa SX1262 |
| ArduinoJson | 7.4.3+ | Sérialisation JSON |
| U8g2 | 2.35.30+ | Affichage OLED SH1106 |
| XPowersLib | 0.3.3+ | Gestion PMU AXP2101 |

### Configuration

1. **Cloner le repo**
   ```bash
   git clone https://github.com/raphaelsavard7-a11y/Projet_Session_LoRa_Et_LLM.git
   ```

2. **Configurer les identifiants**
   ```bash
   cd LoRaReceiver_Final
   cp auth.h.example auth.h
   ```
   Éditer `auth.h` avec vos identifiants WiFi, API LLM et MQTT.

3. **Flasher l'émetteur** (brancher le premier T-Beam)
   ```bash
   arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi LoRaSender_Final
   arduino-cli upload --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi -p COMxx LoRaSender_Final
   ```

4. **Flasher le récepteur** (brancher le deuxième T-Beam)
   ```bash
   arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi LoRaReceiver_Final
   arduino-cli upload --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi -p COMxx LoRaReceiver_Final
   ```

## 🔧 Configuration matérielle

### LilyGO T-Beam S3 Supreme

| Composant | Broche |
|---|---|
| Potentiomètre | IO2 (sender) |
| LED Rouge | IO3 (sender) |
| LED Bleue | IO2 (receiver) |
| Bouton | IO0 (les deux) |
| OLED SDA | IO17 |
| OLED SCL | IO18 |
| SX1262 CS | IO10 |
| SX1262 DIO1 | IO1 |
| SX1262 RST | IO5 |
| SX1262 BUSY | IO4 |

### Paramètres LoRa

| Paramètre | Valeur |
|---|---|
| Fréquence | 868.0 MHz |
| Bande passante | 125 kHz |
| Spreading Factor | 10 |
| Coding Rate | 4/7 |
| Sync Word | 0x67 |
| Puissance TX | 17 dBm |
| Préambule | 16 symboles |

## 📡 Format MQTT

Le récepteur publie sur le topic configuré un message lisible :

```
Potentiometre: 2048 (50%) | LED: BLINK | Licorne: Je contemple les étoiles depuis ma corne...
```

## 🛠️ Technologies

- **ESP32-S3** — Microcontrôleur double cœur
- **SX1262** — Transceiver LoRa longue portée
- **RadioLib** — Abstraction radio multi-plateforme
- **NanoGPT** — API LLM (format OpenAI)
- **MQTT WebSocket** — Publication temps réel via ESP-IDF natif
- **U8g2** — Rendu graphique OLED
- **ArduinoJson** — Parsing et sérialisation JSON

## 📝 Licence

Projet académique — session 2026.
