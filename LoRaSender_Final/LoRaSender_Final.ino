/*
 * ============================================================================
 *  ÉMETTEUR LoRa — Projet IoT Bidirectionnel avec LLM
 *  LilyGO T-Beam S3 Supreme  (ESP32-S3 + SX1262)
 * ============================================================================
 *
 *  FLUX :
 *    Potentiomètre (IO2) ──► LoRa TX ──► [Récepteur]
 *                                             │
 *    DEL (IO3) ◄── LoRa RX ◄── LLM décision ─┘
 *
 *  FONCTIONNALITÉS :
 *    ✦ Lecture ADC lissée (moyenne exponentielle)
 *    ✦ Protocole LoRa JSON bidirectionnel via RadioLib
 *    ✦ Contrôle DEL PWM avec effets (ON / OFF / BLINK respiration)
 *    ✦ Affichage OLED avec barre de progression et tableau de bord
 *    ✦ Surveillance batterie via PMU AXP2101
 *
 *  LIBRAIRIES : RadioLib, ArduinoJson, U8g2lib, XPowersLib
 *  SYNC WORD  : 0x67
 * ============================================================================
 */

#include <RadioLib.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <XPowersLib.h>

// ============================================================================
// BROCHES — T-Beam S3 Supreme
// ============================================================================
#define PIN_POT       2
#define PIN_LED       3
#define PIN_BTN       0

// I2C Bus 0 : OLED SH1106
#define OLED_SDA      17
#define OLED_SCL      18

// I2C Bus 1 : PMU AXP2101
#define PMU_SDA       42
#define PMU_SCL       41

// SPI Radio SX1262
#define RADIO_SCLK    12
#define RADIO_MISO    13
#define RADIO_MOSI    11
#define RADIO_CS      10
#define RADIO_RST     5
#define RADIO_DIO1    1
#define RADIO_BUSY    4

// ============================================================================
// CONFIGURATION RADIO LoRa
// ============================================================================
#define LORA_FREQ       868.0    // MHz
#define LORA_BW         125.0    // kHz
#define LORA_SF         10
#define LORA_CR         7        // Coding Rate 4/7
#define LORA_SYNC       0x67
#define LORA_POWER      17       // dBm
#define LORA_PREAMBLE   16
#define LORA_TCXO_V     1.8      // Tension TCXO T-Beam S3

// ============================================================================
// TEMPORISATION
// ============================================================================
#define RX_TIMEOUT          45000  // Timeout attente réponse (ms) — LLM peut prendre 20s+
#define DISPLAY_RESPONSE_MS 8000   // Durée affichage réponse LLM (ms)
#define ADC_ALPHA           0.12f  // Lissage exponentiel ADC
#define OLED_REFRESH_MS     250    // Rafraîchissement écran pot (ms)

// ============================================================================
// COMPATIBILITÉ LEDC (ESP32 Core v2 vs v3)
// ============================================================================
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  #define LED_PWM_INIT()  ledcAttach(PIN_LED, 5000, 8)
  #define LED_PWM(v)      ledcWrite(PIN_LED, (v))
#else
  #define LED_CH 0
  #define LED_PWM_INIT()  do { ledcSetup(LED_CH, 5000, 8); ledcAttachPin(PIN_LED, LED_CH); } while(0)
  #define LED_PWM(v)      ledcWrite(LED_CH, (v))
#endif

// ============================================================================
// OBJETS GLOBAUX
// ============================================================================
SPIClass radioSPI(HSPI);
SX1262 radio = new Module(RADIO_CS, RADIO_DIO1, RADIO_RST, RADIO_BUSY, radioSPI);

U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
XPowersAXP2101 pmu;

// ============================================================================
// ÉTAT DU SYSTÈME
// ============================================================================
volatile bool   loraFlag       = false;
float           potFiltre      = 0;
unsigned long   dernierEnvoi   = 0;
uint32_t        compteurMsg    = 0;
String          reponseLLM     = "";
String          cmdLED         = "OFF";
int             rssiRx         = 0;
float           snrRx          = 0;
bool            attenteReponse = false;
unsigned long   debutAttente   = 0;
unsigned long   tempsReponse   = 0;
bool            radioOK        = false;
int             dernierPotAff  = -999;

// ============================================================================
// ISR — RadioLib DIO1
// ============================================================================
IRAM_ATTR void onLoRaFlag() { loraFlag = true; }

// ============================================================================
// INITIALISATION PMU AXP2101
// ============================================================================
void initPMU() {
  if (!pmu.init(Wire1, AXP2101_SLAVE_ADDRESS, PMU_SDA, PMU_SCL)) {
    Serial.println(F("[PMU] AXP2101 non detecte"));
    return;
  }
  Serial.println(F("[PMU] AXP2101 OK"));

  // Alimenter tous les périphériques du T-Beam S3 Supreme
  pmu.setALDO1Voltage(3300);  pmu.enableALDO1();
  pmu.setALDO2Voltage(3300);  pmu.enableALDO2();
  pmu.setALDO3Voltage(3300);  pmu.enableALDO3();  // LoRa
  pmu.setALDO4Voltage(3300);  pmu.enableALDO4();  // GPS
  pmu.setBLDO1Voltage(3300);  pmu.enableBLDO1();
  pmu.setBLDO2Voltage(3300);  pmu.enableBLDO2();
  pmu.setDC3Voltage(3300);    pmu.enableDC3();
  pmu.setDC5Voltage(3300);    pmu.enableDC5();
  pmu.setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);
}

// ============================================================================
// INITIALISATION RADIO SX1262 (RadioLib)
// ============================================================================
void initRadio() {
  radioSPI.begin(RADIO_SCLK, RADIO_MISO, RADIO_MOSI, RADIO_CS);

  Serial.print(F("[RADIO] Init SX1262... "));
  int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                           LORA_SYNC, LORA_POWER, LORA_PREAMBLE,
                           LORA_TCXO_V, false);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("ERREUR %d\n", state);
    radioOK = false;
    return;
  }
  Serial.println(F("OK"));
  radioOK = true;

  radio.setCRC(2);
  radio.setDio1Action(onLoRaFlag);
  radio.startReceive();
}

// ============================================================================
// ÉCRAN DE DÉMARRAGE
// ============================================================================
void showSplash() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_helvB12_tf);
  oled.drawUTF8(8, 18, "LoRa + LLM");
  oled.setFont(u8g2_font_helvR08_tf);
  oled.drawUTF8(12, 33, "\xC9metteur IoT v2.0");
  oled.drawHLine(8, 37, 112);
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawUTF8(10, 50, "T-Beam S3 Supreme");
  oled.drawUTF8(10, 60, "RadioLib | SX1262");
  oled.drawFrame(0, 0, 128, 64);
  oled.sendBuffer();
  delay(2500);
}

// ============================================================================
// COMPOSANT UI — Barre de progression
// ============================================================================
void drawBar(int x, int y, int w, int h, int pct) {
  oled.drawFrame(x, y, w, h);
  int remplissage = map(constrain(pct, 0, 100), 0, 100, 0, w - 2);
  if (remplissage > 0) oled.drawBox(x + 1, y + 1, remplissage, h - 2);
}

// ============================================================================
// AFFICHAGE — Tableau de bord potentiomètre
// ============================================================================
void afficherPot(int pot) {
  if (abs(pot - dernierPotAff) < 15) return;   // anti-flicker
  dernierPotAff = pot;

  int pct = map(pot, 0, 4095, 0, 100);
  float battV = pmu.getBattVoltage() / 1000.0;
  int battPct = constrain(map((int)(battV * 100), 310, 420, 0, 100), 0, 100);
  char buf[40];

  oled.clearBuffer();

  // ── En-tête ──
  oled.setFont(u8g2_font_helvB08_tf);
  oled.drawUTF8(0, 10, "EMETTEUR");
  snprintf(buf, sizeof(buf), "\xE2\x9A\xA1%.1fV", battV);   // ⚡
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawUTF8(80, 10, buf);
  oled.drawHLine(0, 13, 128);

  // ── Valeur pot ──
  oled.setFont(u8g2_font_helvB14_tf);
  snprintf(buf, sizeof(buf), "%d", pot);
  oled.drawUTF8(0, 32, buf);

  oled.setFont(u8g2_font_helvR10_tf);
  snprintf(buf, sizeof(buf), "%d%%", pct);
  oled.drawUTF8(80, 32, buf);

  // ── Barre de progression ──
  drawBar(0, 37, 128, 10, pct);

  // ── Pied de page ──
  oled.setFont(u8g2_font_5x7_tf);
  snprintf(buf, sizeof(buf), "LED:%-5s  Msg:#%lu", cmdLED.c_str(), compteurMsg);
  oled.drawUTF8(0, 58, buf);

  if (rssiRx != 0) {
    snprintf(buf, sizeof(buf), "RSSI:%d", rssiRx);
    oled.drawUTF8(0, 64, buf);
  }

  oled.sendBuffer();
}

// ============================================================================
// AFFICHAGE — Message simple 2 lignes
// ============================================================================
void afficherMsg(const char* l1, const char* l2) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_helvB10_tf);
  oled.drawUTF8(0, 22, l1);
  oled.setFont(u8g2_font_helvR08_tf);
  oled.drawUTF8(0, 42, l2);
  oled.sendBuffer();
}

// ============================================================================
// AFFICHAGE — Réponse LLM avec retour à la ligne
// ============================================================================
void afficherReponse(int pot) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_helvB08_tf);

  char hdr[48];
  snprintf(hdr, sizeof(hdr), "Pot:%d  LED:%s", pot, cmdLED.c_str());
  oled.drawUTF8(0, 10, hdr);
  oled.drawHLine(0, 13, 128);

  // Word-wrap de la réponse LLM
  oled.setFont(u8g2_font_5x7_tf);
  int y = 24, maxW = 128;
  const char* p = reponseLLM.c_str();

  while (*p && y <= 64) {
    const char* lineStart = p;
    const char* lastSpace = NULL;
    const char* scan = p;

    while (*scan && *scan != '\n') {
      const char* next = scan + 1;
      int len = (int)(next - lineStart);
      char tmp[128];
      if (len < (int)sizeof(tmp)) {
        memcpy(tmp, lineStart, len);
        tmp[len] = '\0';
        if (oled.getUTF8Width(tmp) > maxW) break;
      }
      if (*scan == ' ') lastSpace = scan;
      scan = next;
    }

    const char* lineEnd;
    if (*scan == '\0' || *scan == '\n') {
      lineEnd = scan;
      p = (*scan == '\n') ? scan + 1 : scan;
    } else if (lastSpace && lastSpace > lineStart) {
      lineEnd = lastSpace;
      p = lastSpace + 1;
    } else {
      lineEnd = scan;
      p = scan;
    }

    int len = (int)(lineEnd - lineStart);
    char lineBuf[128];
    if (len >= (int)sizeof(lineBuf)) len = sizeof(lineBuf) - 1;
    memcpy(lineBuf, lineStart, len);
    lineBuf[len] = '\0';
    oled.drawUTF8(0, y, lineBuf);
    y += 9;
  }

  oled.sendBuffer();
}

// ============================================================================
// ENVOI VALEUR POTENTIOMÈTRE VIA LoRa
// ============================================================================
void envoyerPot(int pot) {
  if (!radioOK) return;

  compteurMsg++;
  int pct = map(pot, 0, 4095, 0, 100);

  JsonDocument doc;
  doc["id"]  = compteurMsg;
  doc["pot"] = pot;
  doc["pct"] = pct;

  String payload;
  serializeJson(doc, payload);

  Serial.printf("[TX] #%lu  Pot=%d (%d%%)\n", compteurMsg, pot, pct);
  afficherMsg("Envoi LoRa...", ("Pot: " + String(pot)).c_str());

  int state = radio.transmit(payload);
  loraFlag = false;   // effacer flag TX

  if (state == RADIOLIB_ERR_NONE) {
    Serial.println(F("[TX] OK — attente reponse..."));
    attenteReponse = true;
    debutAttente   = millis();
    afficherMsg("Envoi OK!", "Attente du LLM...");
  } else {
    Serial.printf("[TX] Erreur: %d\n", state);
    afficherMsg("Erreur TX!", ("Code: " + String(state)).c_str());
  }

  radio.startReceive();
  dernierEnvoi = millis();
}

// ============================================================================
// TRAITEMENT RÉPONSE LoRa (du récepteur)
// ============================================================================
void traiterReponse() {
  String donnees;
  int state = radio.readData(donnees);

  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[RX] Erreur lecture: %d\n", state);
    radio.startReceive();
    return;
  }

  rssiRx = radio.getRSSI();
  snrRx  = radio.getSNR();

  Serial.println("[RX] Recu: " + donnees);
  Serial.printf("[RX] RSSI=%d dBm  SNR=%.1f dB\n", rssiRx, snrRx);

  // Parser JSON  {"msg":"...","led":"ON/OFF/BLINK"}
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, donnees);

  if (!err && doc.containsKey("msg") && doc.containsKey("led")) {
    reponseLLM = doc["msg"].as<String>();
    cmdLED     = doc["led"].as<String>();
    cmdLED.toUpperCase();
    if (cmdLED != "ON" && cmdLED != "OFF" && cmdLED != "BLINK") cmdLED = "OFF";
  } else {
    // Fallback : essayer d'extraire le JSON du texte brut
    int s = donnees.indexOf('{');
    int e = donnees.lastIndexOf('}');
    if (s >= 0 && e > s) {
      String jsonStr = donnees.substring(s, e + 1);
      JsonDocument doc2;
      if (deserializeJson(doc2, jsonStr) == DeserializationError::Ok) {
        reponseLLM = doc2["msg"].as<String>();
        cmdLED     = doc2["led"].as<String>();
        cmdLED.toUpperCase();
        if (cmdLED != "ON" && cmdLED != "OFF" && cmdLED != "BLINK") cmdLED = "OFF";
      } else {
        reponseLLM = donnees.substring(0, 80);
        cmdLED     = "OFF";
      }
    } else {
      reponseLLM = donnees.substring(0, 80);
      cmdLED     = "OFF";
    }
  }

  Serial.println("[RX] LLM : " + reponseLLM);
  Serial.println("[RX] LED : " + cmdLED);

  attenteReponse = false;
  tempsReponse   = millis();
  dernierPotAff  = -999;   // forcer rafraîchissement après
  afficherReponse((int)potFiltre);

  radio.startReceive();
}

// ============================================================================
// MISE À JOUR DEL (PWM + effets)
// ============================================================================
void mettreAJourLED() {
  if (cmdLED == "ON") {
    LED_PWM(255);
  } else if (cmdLED == "OFF") {
    LED_PWM(0);
  } else if (cmdLED == "BLINK") {
    // Effet respiration sinusoïdal
    float phase = (millis() % 2000) / 2000.0 * 2.0 * PI;
    int luminosite = (int)((sin(phase) + 1.0) * 127.5);
    LED_PWM(luminosite);
  }
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println(F("\n========================================"));
  Serial.println(F("  EMETTEUR LoRa + LLM IoT"));
  Serial.println(F("  T-Beam S3 Supreme | SX1262"));
  Serial.println(F("  Sync Word: 0x67"));
  Serial.println(F("========================================\n"));

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire1.begin(PMU_SDA, PMU_SCL);

  initPMU();

  oled.begin();
  oled.enableUTF8Print();
  oled.setContrast(220);
  showSplash();

  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_POT, INPUT);
  analogReadResolution(12);

  LED_PWM_INIT();
  LED_PWM(0);

  initRadio();

  potFiltre = analogRead(PIN_POT);
  afficherMsg("Pret!", "Lecture pot en cours...");
  delay(1000);
}

// ============================================================================
// LOOP
// ============================================================================
void loop() {
  // ── Lecture ADC lissée ──
  int raw = analogRead(PIN_POT);
  potFiltre = potFiltre * (1.0f - ADC_ALPHA) + raw * ADC_ALPHA;
  int pot = (int)potFiltre;

  // ── Réception LoRa (ISR flag) ──
  if (loraFlag) {
    loraFlag = false;
    traiterReponse();
  }

  // ── Envoi sur appui bouton uniquement ──
  static bool btnPrec = HIGH;
  bool btnAct = digitalRead(PIN_BTN);
  if (btnPrec == HIGH && btnAct == LOW && !attenteReponse) {
    delay(50);  // anti-rebond
    envoyerPot(pot);
  }
  btnPrec = btnAct;

  // ── Timeout attente réponse ──
  if (attenteReponse && (millis() - debutAttente > RX_TIMEOUT)) {
    Serial.println(F("[TX] Timeout — aucune reponse"));
    attenteReponse = false;
    afficherMsg("Timeout!", "Pas de reponse RX");
    delay(2000);
    dernierPotAff = -999;
    radio.startReceive();
  }

  // ── Affichage ──
  if (!attenteReponse) {
    if (tempsReponse > 0 && (millis() - tempsReponse < DISPLAY_RESPONSE_MS)) {
      // Montrer la réponse LLM pendant DISPLAY_RESPONSE_MS
      afficherReponse(pot);
    } else {
      tempsReponse = 0;
      static unsigned long dernierAff = 0;
      if (millis() - dernierAff > OLED_REFRESH_MS) {
        dernierAff = millis();
        afficherPot(pot);
      }
    }
  }

  // ── Effet LED ──
  mettreAJourLED();

  delay(10);
}
