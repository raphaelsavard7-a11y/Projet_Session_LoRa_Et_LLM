/*
 * ============================================================================
 *  RÉCEPTEUR LoRa — Projet IoT Bidirectionnel avec LLM + MQTT
 *  LilyGO T-Beam S3 Supreme  (ESP32-S3 + SX1262)
 * ============================================================================
 *
 *  FLUX :
 *    LoRa RX (pot) ──► WiFi ──► LLM (NanoGPT)
 *         │                         │
 *         │              décision ◄─┘
 *         │                  │
 *         │     MQTT pub ◄───┤
 *         │                  │
 *    LoRa TX (réponse) ◄─────┘
 *
 *  FONCTIONNALITÉS :
 *    ✦ Réception LoRa JSON via RadioLib (SX1262)
 *    ✦ Connexion WiFi (WPA2 Personnel ou Entreprise)
 *    ✦ Appel API LLM NanoGPT (format OpenAI) avec parsing robuste
 *    ✦ Publication MQTT TLS sur mqtt.wafie.net:443
 *    ✦ Renvoi de la décision LLM au émetteur via LoRa
 *    ✦ Tableau de bord OLED avec statut WiFi / MQTT / LoRa
 *    ✦ Reconnexion automatique WiFi et MQTT
 *    ✦ Heartbeat MQTT toutes les 30 secondes
 *
 *  LIBRAIRIES : RadioLib, ArduinoJson, WiFi, HTTPClient,
 *               WiFiClientSecure, PubSubClient, U8g2lib, XPowersLib
 *  SYNC WORD  : 0x67
 * ============================================================================
 */

#include <WiFi.h>
#include <esp_wpa2.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "mqtt_client.h"
#include <RadioLib.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <XPowersLib.h>

#include "auth.h"

// ============================================================================
// BROCHES — T-Beam S3 Supreme
// ============================================================================
#define PIN_BTN       0
#define PIN_LED_RX    2

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
#define LORA_FREQ       868.0
#define LORA_BW         125.0
#define LORA_SF         10
#define LORA_CR         7
#define LORA_SYNC       0x67
#define LORA_POWER      17
#define LORA_PREAMBLE   16
#define LORA_TCXO_V     1.8

// ============================================================================
// COMPATIBILITÉ LEDC (ESP32 Core v2 vs v3)
// ============================================================================
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  #define LED_RX_PWM_INIT()  ledcAttach(PIN_LED_RX, 5000, 8)
  #define LED_RX_PWM(v)      ledcWrite(PIN_LED_RX, (v))
#else
  #define LED_RX_CH 1
  #define LED_RX_PWM_INIT()  do { ledcSetup(LED_RX_CH, 5000, 8); ledcAttachPin(PIN_LED_RX, LED_RX_CH); } while(0)
  #define LED_RX_PWM(v)      ledcWrite(LED_RX_CH, (v))
#endif

// ============================================================================
// TEMPORISATION
// ============================================================================
#define LLM_TIMEOUT_MS      60000

// ============================================================================
// OBJETS GLOBAUX
// ============================================================================
SPIClass radioSPI(HSPI);
SX1262 radio = new Module(RADIO_CS, RADIO_DIO1, RADIO_RST, RADIO_BUSY, radioSPI);

U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
XPowersAXP2101 pmu;

esp_mqtt_client_handle_t mqttClient = NULL;

// Chaîne complète: GlobalSign Root CA R4 + Google Trust Services WE1
static const char MQTT_CA_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIB3DCCAYOgAwIBAgINAgPlfvU/k/2lCSGypjAKBggqhkjOPQQDAjBQMSQwIgYD
VQQLExtHbG9iYWxTaWduIEVDQyBSb290IENBIC0gUjQxEzARBgNVBAoTCkdsb2Jh
bFNpZ24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMTIxMTEzMDAwMDAwWhcNMzgw
MTE5MDMxNDA3WjBQMSQwIgYDVQQLExtHbG9iYWxTaWduIEVDQyBSb290IENBIC0g
UjQxEzARBgNVBAoTCkdsb2JhbFNpZ24xEzARBgNVBAMTCkdsb2JhbFNpZ24wWTAT
BgcqhkjOPQIBBggqhkjOPQMBBwNCAAS4xnnTj2wlDp8uORkcA6SumuU5BwkWymOx
uYb4ilfBV85C+nOh92VC/x7BALJucw7/xyHlGKSq2XE/qNS5zowdo0IwQDAOBgNV
HQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUVLB7rUW44kB/
+wpu+74zyTyjhNUwCgYIKoZIzj0EAwIDRwAwRAIgIk90crlgr/HmnKAWBVBfw147
bmF0774BxL4YSFlhgjICICadVGNA3jdgUM/I2O2dgq43mLyjj0xMqTQrbO/7lZsm
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICjjCCAjOgAwIBAgIQf/NXaJvCTjAtkOGKQb0OHzAKBggqhkjOPQQDAjBQMSQw
IgYDVQQLExtHbG9iYWxTaWduIEVDQyBSb290IENBIC0gUjQxEzARBgNVBAoTCkds
b2JhbFNpZ24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMjMxMjEzMDkwMDAwWhcN
MjkwMjIwMTQwMDAwWjA7MQswCQYDVQQGEwJVUzEeMBwGA1UEChMVR29vZ2xlIFRy
dXN0IFNlcnZpY2VzMQwwCgYDVQQDEwNXRTEwWTATBgcqhkjOPQIBBggqhkjOPQMB
BwNCAARvzTr+Z1dHTCEDhUDCR127WEcPQMFcF4XGGTfn1XzthkubgdnXGhOlCgP4
mMTG6J7/EFmPLCaY9eYmJbsPAvpWo4IBAjCB/zAOBgNVHQ8BAf8EBAMCAYYwHQYD
VR0lBBYwFAYIKwYBBQUHAwEGCCsGAQUFBwMCMBIGA1UdEwEB/wQIMAYBAf8CAQAw
HQYDVR0OBBYEFJB3kjVnxP+ozKnme9mAeXvMk/k4MB8GA1UdIwQYMBaAFFSwe61F
uOJAf/sKbvu+M8k8o4TVMDYGCCsGAQUFBwEBBCowKDAmBggrBgEFBQcwAoYaaHR0
cDovL2kucGtpLmdvb2cvZ3NyNC5jcnQwLQYDVR0fBCYwJDAioCCgHoYcaHR0cDov
L2MucGtpLmdvb2cvci9nc3I0LmNybDATBgNVHSAEDDAKMAgGBmeBDAECATAKBggq
hkjOPQQDAgNJADBGAiEAokJL0LgR6SOLR02WWxccAq3ndXp4EMRveXMUVUxMWSMC
IQDspFWa3fj7nLgouSdkcPy1SdOR2AGm9OQWs7veyXsBwA==
-----END CERTIFICATE-----
)EOF";

// ============================================================================
// ÉTAT DU SYSTÈME
// ============================================================================
volatile bool   loraFlag          = false;
bool            radioOK           = false;
bool            wifiOK            = false;
bool            mqttOK            = false;

int             dernierPot        = 0;
int             dernierPct        = 0;
uint32_t        dernierMsgId      = 0;
String          derniereLLMReponse = "";
String          derniereLedCmd    = "";
int             rssiRx            = 0;
float           snrRx             = 0;
uint32_t        compteurRx        = 0;

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

  pmu.setALDO1Voltage(3300);  pmu.enableALDO1();
  pmu.setALDO2Voltage(3300);  pmu.enableALDO2();
  pmu.setALDO3Voltage(3300);  pmu.enableALDO3();
  pmu.setALDO4Voltage(3300);  pmu.enableALDO4();
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
// CONNEXION WIFI
// ============================================================================
void connectWiFi() {
  Serial.println(F("[WIFI] Connexion..."));
  afficherMsg("WiFi...", WIFI_SSID);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  delay(100);

  if (USE_WPA2_ENTERPRISE) {
    esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
    esp_wifi_sta_wpa2_ent_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
    esp_wifi_sta_wpa2_ent_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));
    esp_wifi_sta_wpa2_ent_enable();
    WiFi.begin(WIFI_SSID);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  int essais = 0;
  while (WiFi.status() != WL_CONNECTED && essais < 40) {
    delay(500);
    Serial.print(".");
    essais++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiOK = true;
    Serial.printf("\n[WIFI] Connecte : %s\n", WiFi.localIP().toString().c_str());
    afficherMsg("WiFi OK!", WiFi.localIP().toString().c_str());
  } else {
    wifiOK = false;
    Serial.println(F("\n[WIFI] ECHEC"));
    afficherMsg("WiFi ECHEC", "Verifier auth.h");
  }
  delay(1000);
}

// ============================================================================
// MQTT EVENT HANDLER (ESP-IDF natif — supporte WSS)
// ============================================================================
static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
  switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
      mqttOK = true;
      Serial.println(F("[MQTT] Connecte via WSS!"));
      break;
    case MQTT_EVENT_DISCONNECTED:
      mqttOK = false;
      Serial.println(F("[MQTT] Deconnecte"));
      break;
    case MQTT_EVENT_ERROR:
      Serial.printf("[MQTT] Erreur type=%d\n", ev->error_handle->error_type);
      if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        Serial.printf("[MQTT]   transport errno=%d\n",
                      ev->error_handle->esp_transport_sock_errno);
      }
      if (ev->error_handle->error_type == MQTT_ERROR_TYPE_ESP_TLS) {
        Serial.printf("[MQTT]   tls err=0x%lx  tls_stack=0x%lx\n",
                      ev->error_handle->esp_tls_last_esp_err,
                      ev->error_handle->esp_tls_stack_err);
      }
      break;
    case MQTT_EVENT_PUBLISHED:
      Serial.printf("[MQTT] Message publie (msg_id=%d)\n", ev->msg_id);
      break;
    default:
      break;
  }
}

// ============================================================================
// CONNEXION MQTT (WSS sur port 443)
// ============================================================================
// URI MQTT statique (doit survivre à connectMQTT)
static char mqttUri[128];

void connectMQTT() {
  if (!wifiOK) return;

  if (mqttClient != NULL) {
    esp_mqtt_client_stop(mqttClient);
    esp_mqtt_client_destroy(mqttClient);
    mqttClient = NULL;
  }

  snprintf(mqttUri, sizeof(mqttUri), "ws://%s:%d/mqtt", MQTT_BROKER, 80);

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = mqttUri;
  cfg.credentials.username = MQTT_USER;
  cfg.credentials.authentication.password = MQTT_PASS;
  cfg.credentials.client_id = MQTT_CLIENT_ID;
  cfg.session.keepalive = 120;
  cfg.buffer.size = 1024;

  mqttClient = esp_mqtt_client_init(&cfg);
  esp_mqtt_client_register_event(mqttClient, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                 mqtt_event_handler, NULL);
  esp_mqtt_client_start(mqttClient);

  Serial.printf("[MQTT] Demarrage WSS: %s\n", mqttUri);

  // Attendre connexion (max 5s)
  unsigned long t0 = millis();
  while (!mqttOK && millis() - t0 < 5000) {
    delay(100);
  }
  Serial.printf("[MQTT] Statut apres init: %s\n", mqttOK ? "CONNECTE" : "EN ATTENTE...");
}

// ============================================================================
// ÉCRAN DE DÉMARRAGE
// ============================================================================
void showSplash() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_helvB12_tf);
  oled.drawUTF8(4, 18, "LoRa + LLM");
  oled.setFont(u8g2_font_helvR08_tf);
  oled.drawUTF8(8, 33, "R\xe9""cepteur IoT v2.0");
  oled.drawHLine(8, 37, 112);
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawUTF8(10, 50, "WiFi + MQTT + NanoGPT");
  oled.drawUTF8(10, 60, "RadioLib | SX1262");
  oled.drawFrame(0, 0, 128, 64);
  oled.sendBuffer();
  delay(2500);
}

// ============================================================================
// AFFICHAGE — Message simple
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
// AFFICHAGE — Tableau de bord principal (texte LLM complet + LED visuelle)
// ============================================================================
void afficherDashboard() {
  oled.clearBuffer();
  char buf[64];

  // ── Barre de statut compacte + indicateur LED ──
  oled.setFont(u8g2_font_4x6_tf);
  snprintf(buf, sizeof(buf), "W:%s M:%s R:%s Pot:%d(%d%%)",
           wifiOK ? "+" : "-",
           mqttOK ? "+" : "-",
           radioOK ? "+" : "-",
           dernierPot, dernierPct);
  oled.drawUTF8(0, 6, buf);

  oled.drawHLine(0, 8, 128);

  if (compteurRx > 0) {
    // ── Texte LLM complet avec retour à la ligne automatique ──
    // Font 4x6 = 32 chars/ligne, 7 lignes disponibles (y=15 à y=63)
    const int CHARS_PER_LINE = 32;
    const int LINE_HEIGHT = 8;
    const int MAX_LINES = 7;
    int y = 15;

    String txt = derniereLLMReponse;
    int pos = 0;
    int line = 0;

    while (pos < (int)txt.length() && line < MAX_LINES) {
      int remaining = txt.length() - pos;
      int len = min(remaining, CHARS_PER_LINE);

      // Retour à la ligne sur un espace si possible
      if (remaining > CHARS_PER_LINE) {
        int lastSpace = -1;
        for (int i = len - 1; i > len / 2; i--) {
          if (txt.charAt(pos + i) == ' ') { lastSpace = i; break; }
        }
        if (lastSpace > 0) len = lastSpace + 1;
      }

      String segment = txt.substring(pos, pos + len);
      segment.trim();
      oled.drawUTF8(0, y, segment.c_str());
      pos += len;
      y += LINE_HEIGHT;
      line++;
    }
  } else {
    oled.setFont(u8g2_font_5x7_tf);
    oled.drawUTF8(10, 30, "En attente...");
    int dots = (millis() / 500) % 4;
    String anim = "";
    for (int i = 0; i < dots; i++) anim += ".";
    oled.drawUTF8(80, 30, anim.c_str());
  }

  oled.sendBuffer();
}

// ============================================================================
// APPEL API LLM (NanoGPT — format OpenAI)
// ============================================================================
String appelLLM(int pot) {
  if (WiFi.status() != WL_CONNECTED) {
    wifiOK = false;
    return "{\"msg\":\"Erreur: WiFi deconnecte\",\"led\":\"OFF\"}";
  }

  WiFiClientSecure httpsClient;
  httpsClient.setInsecure();   // Skip cert verification pour LLM

  HTTPClient http;
  http.begin(httpsClient, OPENWEBUI_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + API_KEY);
  http.setTimeout(LLM_TIMEOUT_MS);

  JsonDocument reqDoc;
  reqDoc["model"] = MODEL_NAME;

  JsonArray messages = reqDoc["messages"].to<JsonArray>();

  JsonObject sysMsg = messages.add<JsonObject>();
  sysMsg["role"]    = "system";
  sysMsg["content"] = SYSTEM_PROMPT;

  JsonObject usrMsg = messages.add<JsonObject>();
  usrMsg["role"]    = "user";
  usrMsg["content"] = "potentiometre: " + String(pot);

  String payload;
  serializeJson(reqDoc, payload);

  Serial.println("[LLM] Requete envoyee...");

  int httpCode = http.POST(payload);
  String resultat = "";

  if (httpCode == 200) {
    String body = http.getString();

    JsonDocument repDoc;
    DeserializationError err = deserializeJson(repDoc, body);

    if (!err) {
      resultat = repDoc["choices"][0]["message"]["content"].as<String>();
      Serial.println("[LLM] Reponse brute: " + resultat);
    } else {
      resultat = "{\"msg\":\"Erreur parsing reponse LLM\",\"led\":\"OFF\"}";
      Serial.println("[LLM] Erreur JSON: " + String(err.c_str()));
    }
  } else {
    resultat = "{\"msg\":\"Erreur HTTP " + String(httpCode) + "\",\"led\":\"OFF\"}";
    Serial.printf("[LLM] Erreur HTTP: %d\n", httpCode);
    if (httpCode > 0) Serial.println(http.getString());
  }

  http.end();
  return resultat;
}

// ============================================================================
// PARSING ROBUSTE DE LA RÉPONSE LLM
// Gère : JSON pur, JSON dans markdown, texte brut
// ============================================================================
void parserReponseLLM(String raw, String& msg, String& led) {
  // Nettoyer les artefacts markdown
  raw.replace("```json", "");
  raw.replace("```", "");
  raw.trim();

  // Tentative 1 : JSON direct
  JsonDocument doc;
  if (deserializeJson(doc, raw) == DeserializationError::Ok
      && doc.containsKey("msg") && doc.containsKey("led")) {
    msg = doc["msg"].as<String>();
    led = doc["led"].as<String>();
    led.toUpperCase();
    if (led != "ON" && led != "OFF" && led != "BLINK") led = "OFF";
    return;
  }

  // Tentative 2 : JSON embarqué dans du texte
  int s = raw.indexOf('{');
  int e = raw.lastIndexOf('}');
  if (s >= 0 && e > s) {
    String jsonStr = raw.substring(s, e + 1);
    JsonDocument doc2;
    if (deserializeJson(doc2, jsonStr) == DeserializationError::Ok) {
      msg = doc2["msg"] | "Analyse indisponible";
      led = doc2["led"] | "OFF";
      led.toUpperCase();
      if (led != "ON" && led != "OFF" && led != "BLINK") led = "OFF";
      return;
    }
  }

  // Fallback : texte brut
  msg = raw.substring(0, 60);
  led = "OFF";
  Serial.println(F("[LLM] Fallback texte brut"));
}

// ============================================================================
// PUBLICATION MQTT — Potentiomètre + Message LLM + LED
// ============================================================================
void publierMQTT(int pot, int pct, const String& llmMsg, const String& ledCmd) {
  if (!mqttOK || mqttClient == NULL) {
    Serial.println(F("[MQTT] Pas connecte, publication ignoree"));
    return;
  }

  String payload = "Potentiometre: " + String(pot) + " (" + String(pct) + "%)"
                 + " | LED: " + ledCmd
                 + " | Licorne: " + llmMsg;

  int msg_id = esp_mqtt_client_publish(mqttClient, MQTT_TOPIC_DATA,
                                       payload.c_str(), 0, 0, 0);
  if (msg_id >= 0) {
    Serial.println("[MQTT] Publie: " + payload);
  } else {
    Serial.println(F("[MQTT] Echec publication"));
  }
}

// ============================================================================
// TRAITEMENT COMPLET D'UN PAQUET LoRa REÇU
// ============================================================================
void traiterReception() {
  String donnees;
  int state = radio.readData(donnees);

  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[RX] Erreur: %d\n", state);
    radio.startReceive();
    return;
  }

  rssiRx = radio.getRSSI();
  snrRx  = radio.getSNR();
  compteurRx++;

  Serial.println("\n════════════════════════════════════");
  Serial.println("[RX] Paquet #" + String(compteurRx));
  Serial.println("[RX] Donnees: " + donnees);
  Serial.printf("[RX] RSSI=%d dBm  SNR=%.1f dB\n", rssiRx, snrRx);

  // ── 1. Parser le JSON du émetteur ──
  JsonDocument rxDoc;
  DeserializationError err = deserializeJson(rxDoc, donnees);

  if (!err) {
    dernierMsgId = rxDoc["id"] | 0;
    dernierPot   = rxDoc["pot"] | 0;
    dernierPct   = rxDoc["pct"] | 0;
  } else {
    // Essayer de lire comme entier brut
    dernierPot = donnees.toInt();
    dernierPct = map(dernierPot, 0, 4095, 0, 100);
    Serial.println(F("[RX] Parsing JSON echoue, utilisation valeur brute"));
  }

  Serial.printf("[RX] Pot=%d  Pct=%d%%\n", dernierPot, dernierPct);
  afficherMsg("Recu!", ("Pot: " + String(dernierPot) + " - LLM...").c_str());

  // ── 2. Appeler le LLM ──
  String llmBrut = appelLLM(dernierPot);

  String llmMsg, ledCmd;
  parserReponseLLM(llmBrut, llmMsg, ledCmd);

  derniereLLMReponse = llmMsg;
  derniereLedCmd     = ledCmd;

  Serial.println("[LLM] Message : " + llmMsg);
  Serial.println("[LLM] LED     : " + ledCmd);

  // ── 3. Publier sur MQTT ──
  publierMQTT(dernierPot, dernierPct, llmMsg, ledCmd);

  // ── 4. Renvoyer la décision au émetteur via LoRa ──
  JsonDocument txDoc;
  txDoc["msg"] = llmMsg;
  txDoc["led"] = ledCmd;

  String txPayload;
  serializeJson(txDoc, txPayload);

  Serial.println("[TX] Envoi reponse: " + txPayload);
  afficherMsg("Envoi reponse...", ledCmd.c_str());

  int txState = radio.transmit(txPayload);
  loraFlag = false;

  if (txState == RADIOLIB_ERR_NONE) {
    Serial.println(F("[TX] Reponse envoyee OK"));
  } else {
    Serial.printf("[TX] Erreur envoi: %d\n", txState);
  }

  // ── 5. Retour en mode réception ──
  radio.startReceive();

  Serial.println("════════════════════════════════════\n");
  afficherDashboard();
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println(F("\n========================================"));
  Serial.println(F("  RECEPTEUR LoRa + LLM + MQTT"));
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
  LED_RX_PWM_INIT();
  LED_RX_PWM(0);

  // ── Radio ──
  initRadio();

  // ── WiFi ──
  connectWiFi();

  // ── MQTT via WSS ──
  connectMQTT();

  afficherMsg("Pret!", "Ecoute LoRa...");
  delay(1000);
  afficherDashboard();
}

// ============================================================================
// LOOP
// ============================================================================
void loop() {
  // ── Reconnexion WiFi ──
  if (WiFi.status() != WL_CONNECTED) {
    wifiOK = false;
    mqttOK = false;
    static unsigned long dernierReconWifi = 0;
    if (millis() - dernierReconWifi > 15000) {
      dernierReconWifi = millis();
      Serial.println(F("[WIFI] Reconnexion..."));
      connectWiFi();
      if (wifiOK) connectMQTT();
    }
  }

  // ── Réception LoRa ──
  if (loraFlag) {
    loraFlag = false;
    traiterReception();
  }

  // ── Contrôle LED physique (IO2) — même effet que le sender ──
  if (derniereLedCmd == "ON") {
    LED_RX_PWM(255);
  } else if (derniereLedCmd == "BLINK") {
    float phase = (millis() % 2000) / 2000.0 * 2.0 * PI;
    int luminosite = (int)((sin(phase) + 1.0) * 127.5);
    LED_RX_PWM(luminosite);
  } else {
    LED_RX_PWM(0);
  }

  // ── Rafraîchir le dashboard périodiquement ──
  static unsigned long dernierAffDash = 0;
  if (millis() - dernierAffDash > 2000) {
    dernierAffDash = millis();
    afficherDashboard();
  }

  delay(10);
}
