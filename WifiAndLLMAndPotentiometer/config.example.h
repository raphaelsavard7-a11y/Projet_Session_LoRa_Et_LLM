/*
 * config.example.h - Modele de configuration
 * Copie ce fichier vers config.h et remplis tes valeurs.
 */

#pragma once

// =============================================
// CONFIGURATION WIFI
// =============================================

// Mettre a true pour WPA2 Entreprise, false pour WPA2 Personnel
#define USE_WPA2_ENTERPRISE  false

// --- WPA2 Personnel ---
const char* WIFI_SSID     = "MonReseau";
const char* WIFI_PASSWORD = "MonMotDePasse";

// --- WPA2 Entreprise (EAP-PEAP) ---
const char* EAP_IDENTITY  = "prenom.nom";
const char* EAP_USERNAME  = "prenom.nom";
const char* EAP_PASSWORD  = "motdepasse";

// =============================================
// CONFIGURATION LLM
// =============================================

const char* OPENWEBUI_URL = "http://10.50.0.108:8080/api/chat/completions";
const char* API_KEY       = "ta-cle-api-ici";
const char* MODEL_NAME    = "assistant-iot-v2";

// System prompt - modifie ce texte !
const char* SYSTEM_PROMPT =
  "Tu es un assistant. "
  "Reponds en 2-3 phrases maximum.";
