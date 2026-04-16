/*
 * Activite 1er avril - Appel LLM via OpenWebUI
 * Cegep de Limoilou - Objets connectes
 *
 * Materiel :
 *   - LilyGO T-Beam Supreme (ESP32-S3, SH1106 OLED, AXP2101 PMU)
 *   - Potentiometre sur GPIO 2 (ADC)
 *   - Bouton integre GPIO 0 (INPUT_PULLUP)
 *
 * Dependances (Arduino Library Manager) :
 *   - ArduinoJson (Benoit Blanchon)
 *   - U8g2 (olikraus)
 *   - XPowersLib (Lewis He)
 *
 * Board dans Arduino IDE :
 *   - ESP32S3 Dev Module (esp32:esp32:esp32s3)
 *   - USB CDC On Boot : Enabled
 *   - PSRAM : OPI PSRAM
 */

#include <WiFi.h>
#include <esp_wpa2.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <XPowersLib.h>

// =============================================
// CONFIGURATION - Voir config.h
// Copie config.example.h vers config.h et remplis tes valeurs.
// =============================================

#include "config.h"

// =============================================
// PINS T-Beam Supreme
// =============================================

#define PIN_POT         2     // GPIO ADC pour le potentiometre
#define PIN_BTN         0     // Bouton integre du T-Beam Supreme

// I2C bus 0 : OLED + capteurs (SDA=17, SCL=18)
#define OLED_SDA        17
#define OLED_SCL        18

// I2C bus 1 : PMU AXP2101 (SDA=42, SCL=41)
#define PMU_SDA         42
#define PMU_SCL         41
#define PMU_IRQ_PIN     40

// =============================================
// OLED SH1106 128x64
// =============================================

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// =============================================
// PMU
// =============================================

XPowersAXP2101 pmu;

// =============================================
// VARIABLES
// =============================================

bool     btnPrecedent     = HIGH;
bool     enAttente        = false;
bool     afficherPot      = true;   // true = mode lecture pot en temps reel
String   derniereReponse  = "";
int      dernierPotAffiche = -1;

// =============================================
// PROTOTYPES
// =============================================

void initPMU();
void connecterWiFi();
void afficherMessage(const char* ligne1, const char* ligne2);
void afficherReponse(String texte, int pot);
String appelLLM(int valeurPot);

// =============================================
// SETUP
// =============================================

void setup() {
  Serial.begin(115200);
  delay(500);

  // I2C bus 0 : OLED
  Wire.begin(OLED_SDA, OLED_SCL);

  // I2C bus 1 : PMU
  Wire1.begin(PMU_SDA, PMU_SCL);

  // Initialiser le PMU (alimentation OLED, LoRa, GPS, etc.)
  initPMU();

  // Initialiser l'OLED avec support UTF-8 (accents francais)
  u8g2.begin();
  u8g2.enableUTF8Print();
  afficherMessage("Demarrage...", "");

  // Pins
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_POT, INPUT);
  analogReadResolution(12); // 0-4095

  // WiFi
  connecterWiFi();
}

// =============================================
// LOOP
// =============================================

void loop() {
  bool btnActuel = digitalRead(PIN_BTN);

  // Detection front descendant (appui bouton)
  if (btnPrecedent == HIGH && btnActuel == LOW && !enAttente) {
    if (!afficherPot) {
      // On etait sur l'ecran reponse -> retour au mode pot
      afficherPot = true;
      dernierPotAffiche = -1; // forcer rafraichissement
    } else {
      // Mode pot -> envoyer au LLM
      int valeurPot = analogRead(PIN_POT);

      Serial.println("=================================");
      Serial.println("Bouton pressé!");
      Serial.println("Potentiomètre : " + String(valeurPot));
      Serial.println("Envoi au LLM...");

      afficherMessage("Envoi...", ("pot: " + String(valeurPot)).c_str());
      enAttente = true;
      afficherPot = false;

      String reponse = appelLLM(valeurPot);

      Serial.println("--- Réponse LLM ---");
      Serial.println(reponse);
      Serial.println("===================");

      derniereReponse = reponse;
      afficherReponse(reponse, valeurPot);
      enAttente = false;
    }
  }

  // Affichage temps reel du pot quand on est en mode pot
  if (afficherPot && !enAttente) {
    int valeurPot = analogRead(PIN_POT);
    // Rafraichir seulement si la valeur change significativement
    if (abs(valeurPot - dernierPotAffiche) > 20) {
      dernierPotAffiche = valeurPot;
      String l1 = "Pot: " + String(valeurPot);
      afficherMessage(l1.c_str(), "Appuie pour envoyer");
    }
  }

  btnPrecedent = btnActuel;
  delay(50);
}

// =============================================
// INITIALISATION PMU AXP2101
// =============================================

void initPMU() {
  if (!pmu.init(Wire1, AXP2101_SLAVE_ADDRESS, PMU_SDA, PMU_SCL)) {
    Serial.println("Avertissement: PMU AXP2101 non detecte");
    return;
  }
  Serial.println("PMU AXP2101 initialise");

  // Alimenter les peripheriques du T-Beam Supreme
  pmu.setALDO1Voltage(3300);  pmu.enableALDO1();  // capteurs
  pmu.setALDO2Voltage(3300);  pmu.enableALDO2();  // capteurs
  pmu.setALDO3Voltage(3300);  pmu.enableALDO3();  // LoRa
  pmu.setALDO4Voltage(3300);  pmu.enableALDO4();  // GPS
  pmu.setBLDO1Voltage(3300);  pmu.enableBLDO1();  // SD card
  pmu.setBLDO2Voltage(3300);  pmu.enableBLDO2();
  pmu.setDC3Voltage(3300);    pmu.enableDC3();     // M.2
  pmu.setDC5Voltage(3300);    pmu.enableDC5();

  // LED de charge
  pmu.setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);
}

// =============================================
// CONNEXION WIFI (WPA2 Personnel ou Entreprise)
// =============================================

void connecterWiFi() {
  Serial.println("Connexion WiFi...");
  afficherMessage("WiFi...", WIFI_SSID);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  delay(100);

  if (USE_WPA2_ENTERPRISE) {
    // WPA2 Entreprise (EAP-PEAP)
    esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
    esp_wifi_sta_wpa2_ent_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
    esp_wifi_sta_wpa2_ent_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));
    esp_wifi_sta_wpa2_ent_enable();
    WiFi.begin(WIFI_SSID);
  } else {
    // WPA2 Personnel
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  int tentatives = 0;
  while (WiFi.status() != WL_CONNECTED && tentatives < 40) {
    delay(500);
    Serial.print(".");
    tentatives++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connecte : " + WiFi.localIP().toString());
    afficherMessage("WiFi OK", WiFi.localIP().toString().c_str());
    delay(1000);
    afficherMessage("Pret!", "Appuie sur le bouton");
  } else {
    Serial.println("\nErreur WiFi !");
    afficherMessage("Erreur WiFi", "Verifie credentials");
  }
}

// =============================================
// APPEL API OPENWEBUI
// =============================================

String appelLLM(int valeurPot) {
  if (WiFi.status() != WL_CONNECTED) {
    return "Erreur: WiFi deconnecte";
  }

  HTTPClient http;
  http.begin(OPENWEBUI_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + API_KEY);
  http.setTimeout(30000);

  // Construction du JSON
  JsonDocument doc;
  doc["model"] = MODEL_NAME;

  JsonArray messages = doc["messages"].to<JsonArray>();

  JsonObject systemMsg = messages.add<JsonObject>();
  systemMsg["role"]    = "system";
  systemMsg["content"] = SYSTEM_PROMPT;

  JsonObject userMsg = messages.add<JsonObject>();
  userMsg["role"]    = "user";
  userMsg["content"] = "potentiometre: " + String(valeurPot);

  String payload;
  serializeJson(doc, payload);

  Serial.println("Payload: " + payload);

  int httpCode = http.POST(payload);
  String reponse = "";

  if (httpCode == 200) {
    String body = http.getString();

    JsonDocument rep;
    DeserializationError err = deserializeJson(rep, body);

    if (!err) {
      reponse = rep["choices"][0]["message"]["content"].as<String>();
    } else {
      reponse = "Erreur JSON: " + String(err.c_str());
    }
  } else {
    reponse = "Erreur HTTP: " + String(httpCode);
    Serial.println(http.getString());
  }

  http.end();
  return reponse;
}

// =============================================
// AFFICHAGE OLED (SH1106 128x64)
// =============================================

void afficherMessage(const char* ligne1, const char* ligne2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB10_tf);
  u8g2.drawUTF8(0, 15, ligne1);
  u8g2.setFont(u8g2_font_helvR08_tf);
  u8g2.drawUTF8(0, 30, ligne2);
  u8g2.sendBuffer();
}

void afficherReponse(String texte, int pot) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB08_tf);

  // Valeur pot en haut
  String header = "Pot: " + String(pot);
  u8g2.drawUTF8(0, 10, header.c_str());
  u8g2.drawHLine(0, 13, 128);

  // Texte de la reponse avec word-wrap par pixels
  u8g2.setFont(u8g2_font_5x7_tf);
  int y = 24;
  int maxWidth = 128;
  const char* p = texte.c_str();

  while (*p && y <= 64) {
    // Trouver combien de caracteres entrent sur une ligne
    const char* lineStart = p;
    const char* lastSpace = NULL;
    const char* scan = p;

    while (*scan && *scan != '\n') {
      // Avancer d'un caractere UTF-8
      const char* next = scan;
      if ((*next & 0x80) == 0) next += 1;
      else if ((*next & 0xE0) == 0xC0) next += 2;
      else if ((*next & 0xF0) == 0xE0) next += 3;
      else next += 4;

      // Mesurer la largeur du texte jusqu'ici
      int len = next - lineStart;
      char buf[128];
      if (len < (int)sizeof(buf)) {
        memcpy(buf, lineStart, len);
        buf[len] = '\0';
        if (u8g2.getUTF8Width(buf) > maxWidth) break;
      }

      if (*scan == ' ') lastSpace = scan;
      scan = next;
    }

    // Determiner la fin de la ligne
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

    // Dessiner la ligne
    int len = lineEnd - lineStart;
    char lineBuf[128];
    if (len >= (int)sizeof(lineBuf)) len = sizeof(lineBuf) - 1;
    memcpy(lineBuf, lineStart, len);
    lineBuf[len] = '\0';
    u8g2.drawUTF8(0, y, lineBuf);
    y += 9;
  }

  u8g2.sendBuffer();
}
