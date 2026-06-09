// Configuration de la version matérielle (à modifier selon votre carte)
#define V1          1  // Esp32 carte V1 (NodeMCU-32S)
#define V2_C3       2  // carte V2 XIAO ESP32-C3
#define V2_S3       3  // carte V2 XIAO ESP32-S3
#ifndef DEBITDOUILLE_VERSION     // surchargé par platformio.ini (-D DEBITDOUILLE_VERSION=1/2/3)
#define DEBITDOUILLE_VERSION V1  // <-- Modifier ici pour changer de version (IDE Arduino)
#endif

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "Arduino.h"
#include <algorithm>
#include <Wire.h>
#include <TinyGPS++.h>
#include <Preferences.h>
#if DEBITDOUILLE_VERSION == V2_S3
    #include "driver/pcnt.h"  // PCNT uniquement pour ESP32-S3
#endif
#include <Adafruit_ADS1X15.h>
#include <ArduinoJson.h>
#include <Update.h>
#include "mbedtls/sha256.h"
#include "mbedtls/version.h"
#include "freertos/stream_buffer.h"

// ============================================
// 📌 INFORMATIONS FIRMWARE
// ============================================
// Version surchargeable par la CI de release (-D FW_VERSION_MAJOR=... depuis le tag fw-vX.Y.Z)
#ifndef FW_VERSION_MAJOR
#define FW_VERSION_MAJOR 2// version majeure, rétrocompatibilité souhaitée dans les avec l'app dans une même version majeure.
#endif
#ifndef FW_VERSION_MINOR
#define FW_VERSION_MINOR 2//TODO faire évoluer cela lors d'implémentation de fonctionnalités, en lien avec l'app flutter.
#endif
#ifndef FW_VERSION_PATCH
#define FW_VERSION_PATCH 0
#endif
#define FW_BUILD_DATE __DATE__  // Date de compilation automatique
#define FW_BUILD_TIME __TIME__  // Heure de compilation automatique

// Modèle ESP selon la version hardware
#if DEBITDOUILLE_VERSION == V1
  #define FW_ESP_MODEL "ESP32-WROOM-32"
#elif DEBITDOUILLE_VERSION == V2_C3
  #define FW_ESP_MODEL "XIAO ESP32-C3"
#elif DEBITDOUILLE_VERSION == V2_S3
  #define FW_ESP_MODEL "XIAO ESP32-S3"
#else
  #define FW_ESP_MODEL "ESP32 Unknown"
#endif

HardwareSerial neogps(1);
Preferences preferences;
TinyGPSPlus gps;

// Configuration ADS1115
Adafruit_ADS1115 ads;  // Instance ADS1115 (adresse I2C par défaut 0x48)

// Constantes pour conversion 4-20mA DÉBITMÈTRES (via ADS1115)
#define SHUNT_RESISTOR 150.0      // Résistance de shunt en Ohms (150Ω actuel, 180Ω recommandé)
#define CURRENT_MIN 4.0           // Courant minimum 4mA
#define CURRENT_MAX 20.0          // Courant maximum 20mA
#define VOLTAGE_MIN (CURRENT_MIN * SHUNT_RESISTOR / 1000.0)   // 0.6V avec 150Ω
#define VOLTAGE_MAX (CURRENT_MAX * SHUNT_RESISTOR / 1000.0)   // 3.0V avec 150Ω
#define CURRENT_DISCONNECTED 3.5  // Seuil de détection déconnexion (<3.5mA)

// Constantes pour conversion 4-20mA PRESSION (en direct sur ESP32)
#define PRESSURE_SHUNT_RESISTOR 150.0    // Résistance de shunt pression en Ohms
#define PRESSURE_CURRENT_MIN 4.0          // Courant minimum 4mA
#define PRESSURE_CURRENT_MAX 20.0         // Courant maximum 20mA
#define PRESSURE_CURRENT_DISCONNECTED 3.5 // Seuil déconnexion pression (<3.5mA)

// ============= CALIBRATION DES DÉBITMÈTRES (VALEURS PAR DÉFAUT, PROGRAMMABLES PAR BLE) =============

// Débitmètres 4-20mA : Échelles maximales (L/min)
float maxFlow1 = 20.0;   // Canal 1: VMZ08 = 0-20 L/min
float maxFlow2 = 20.0;   // Canal 2: VMZ08 = 0-20 L/min
float maxFlow3 = 20.0;   // Canal 3: VMZ08 = 0-20 L/min
float maxFlow4 = 20.0;   // Canal 4: VMZ08 = 0-20 L/min

// Débitmètres PCNT/Interruptions : Nombre d'impulsions par litre
int NbImpulsionsDebitmetre1 = 1000;  // Canal 1: 1000 pulse/L par défaut
int NbImpulsionsDebitmetre2 = 1000;  // Canal 2: 1000 pulse/L par défaut
int NbImpulsionsDebitmetre3 = 1000;  // Canal 3: 1000 pulse/L par défaut
int NbImpulsionsDebitmetre4 = 1000;  // Canal 4: 1000 pulse/L par défaut

// ============= DÉLAIS DES TÂCHES FREERTOS (ms) =============
unsigned long delay_task1_ReadPulse =       1000;  //1000 Période d'échantillonnage PCNT (plus longue = plus précis). Ex : 65Hz => seulement 65 pulses en 1 seconde = 3.9L/min sur vmz08 => précision de 0.06L/min, si éch de 200ms => précision de 0.3L/min. 
unsigned long delay_task_ReadADS1115 =      200;   
unsigned long delay_task_ReadPressure =     500;   // Période de mise à jour de la pression
unsigned long delay_task_ReadGPS =          1000;   //1000
unsigned long delay_task_SendBLE =          1000;   //1000
unsigned long delay_task_HandleCommands =   50;   //
unsigned long delay_task_DebugSerial =      1000;   //

// UUIDs pour le service BLE - Nordic UART Service (NUS)
#define BLE_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"     // Nordic UART Service
#define BLE_TX_CHAR_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"     // TX Characteristic (ESP32 -> App)
#define BLE_RX_CHAR_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"     // RX Characteristic (App -> ESP32)

BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic;
BLECharacteristic *pRxCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// Configuration des broches
#if DEBITDOUILLE_VERSION == V1
#define PIN_PRESSURE 34
#define RXD2 16
#define TXD2 17
#define DEBITMETRE1_PIN 32
#define DEBITMETRE2_PIN 33
#define DEBITMETRE3_PIN 13//NOT USED
#define DEBITMETRE4_PIN 12//NOT USED
#define PIN_PWR_12V_OUT 14//NOT USED
#define PIN_LED         2
#define PIN_SDA         21  // I2C SDA par défaut ESP32
#define PIN_SCL         22  // I2C SCL par défaut ESP32
#elif  DEBITDOUILLE_VERSION == V2_C3//https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started/
#define PIN_PRESSURE 3
#define RXD2 20
#define TXD2 21
#define DEBITMETRE1_PIN 4 //Gauche
#define DEBITMETRE2_PIN 9//centre gauche 
#define DEBITMETRE3_PIN 8//centre droit 
#define DEBITMETRE4_PIN 5 //DROIT
#define PIN_SDA         6 //
#define PIN_SCL         7 //
#define PIN_LED         2 //
#define PIN_PWR_12V_OUT 10 //BTS7004
#elif  DEBITDOUILLE_VERSION == V2_S3//https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/
#define PIN_PRESSURE 2
#define RXD2 44
#define TXD2 43
#define DEBITMETRE1_PIN 3 //Gauche
#define DEBITMETRE2_PIN 8 //centre gauche
#define DEBITMETRE3_PIN 7 //centre droit 
#define DEBITMETRE4_PIN 4 //DROIT
#define PIN_SDA         5 //
#define PIN_SCL         6 //
#define PIN_LED         1 //
#define PIN_PWR_12V_OUT 9 //BTS7004
#endif

// Variables pour l'auto-détection GPS RX/TX
uint8_t gpsRxPin = RXD2;           // Pin RX actuelle
uint8_t gpsTxPin = TXD2;           // Pin TX actuelle
uint16_t gpsFailCounter = 0;       // Compteur d'échecs de communication GPS
bool gpsPinsSwapped = false;       // Indicateur si les pins ont été inversés
#define GPS_FAIL_THRESHOLD 30      // Nombre d'échecs avant d'inverser les pins (30 secondes)
#define GPS_RETRY_THRESHOLD 60     // Nombre d'échecs avant de ré-inverser (si toujours pas de signal)
unsigned long lastGpsDataTime = 0; // Timestamp de la dernière donnée GPS valide

// Variables pour comptage des impulsions
#if DEBITDOUILLE_VERSION == V2_S3
    // PCNT pour ESP32-S3 (4 canaux)
    pcnt_unit_t pcntUnit1 = PCNT_UNIT_0;
    pcnt_unit_t pcntUnit2 = PCNT_UNIT_1;
    pcnt_unit_t pcntUnit3 = PCNT_UNIT_2;
    pcnt_unit_t pcntUnit4 = PCNT_UNIT_3;
    int16_t count1 = 0;
    int16_t count2 = 0;
    int16_t count3 = 0;
    int16_t count4 = 0;
#else
    // Comptage par interruptions pour ESP32-C3 et V1 (4 canaux)
    volatile uint32_t pulseCount1 = 0;
    volatile uint32_t pulseCount2 = 0;
    volatile uint32_t pulseCount3 = 0;
    volatile uint32_t pulseCount4 = 0;
    portMUX_TYPE mux1 = portMUX_INITIALIZER_UNLOCKED;
    portMUX_TYPE mux2 = portMUX_INITIALIZER_UNLOCKED;
    portMUX_TYPE mux3 = portMUX_INITIALIZER_UNLOCKED;
    portMUX_TYPE mux4 = portMUX_INITIALIZER_UNLOCKED;

    // Fonctions ISR pour interruptions
    void IRAM_ATTR pulseCounter1() {
        portENTER_CRITICAL_ISR(&mux1);
        pulseCount1++;
        portEXIT_CRITICAL_ISR(&mux1);
    }

    void IRAM_ATTR pulseCounter2() {
        portENTER_CRITICAL_ISR(&mux2);
        pulseCount2++;
        portEXIT_CRITICAL_ISR(&mux2);
    }

    void IRAM_ATTR pulseCounter3() {
        portENTER_CRITICAL_ISR(&mux3);
        pulseCount3++;
        portEXIT_CRITICAL_ISR(&mux3);
    }

    void IRAM_ATTR pulseCounter4() {
        portENTER_CRITICAL_ISR(&mux4);
        pulseCount4++;
        portEXIT_CRITICAL_ISR(&mux4);
    }
#endif

// Variables pour la réception Bluetooth
const byte numChars = 32;
char receivedChars[numChars];
char message[numChars] = {0};
char value[numChars] = {0};
boolean newData = false;

// Variables pour les constantes sauvegardées
int constDeb1, constDeb2, constDeb3, constDeb4;  // Débitmètres PCNT (pulse/L)
int constManA, constManB;  // Corrections manomètre
int constFlow1, constFlow2, constFlow3, constFlow4;  // Échelles débitmètres 4-20mA (L/min)

// Variables globales
String bluetoothMsg = "";
String messageRecu;
unsigned int valeurRecue;
String valeurRecueStr = "";  // Pour les commandes nécessitant une chaîne de caractères
int calib = 0;  // Calibration ADC en mV (0 = utilise uniquement la calibration interne ESP32)
int correctionManometreA = 100;  // Correction pente (100 = 1.00, pas de correction)
int correctionManometreB = 0;    // Correction offset en bars*100 (0 = pas de correction)
int pressureSensorType = 0;  // Type de capteur de pression: 0=Gravity analogique (0.5-4.5V, 0-16 bar), 1=4-20mA (0-16 bar)

// Nouveaux paramètres de calibration configurables (mode Tension 3 fils)
float pressureMaxBar = 16.0;      // Pression maximale du capteur (bar) - configurable via BLE
float pressureVoltageMin = 0.5;   // Tension minimale (V) - configurable via BLE
float pressureVoltageMax = 4.5;   // Tension maximale (V) - configurable via BLE

String deviceID = "";  // Identifiant unique du device (par défaut: 6 derniers caractères de l'adresse MAC)
float debit1, debit2, pressure, sat, lon, llat, sspeed;
unsigned long frameID = 0;  // ID incrémental de trame BLE pour détecter les pertes de paquets

// ============== FreeRTOS Configuration ==============
// Handles des tâches
TaskHandle_t taskPCNT = NULL;
TaskHandle_t taskADS1115 = NULL;
TaskHandle_t taskPressure = NULL;
TaskHandle_t taskGPS = NULL;
TaskHandle_t taskBLE = NULL;
TaskHandle_t taskCommands = NULL;
TaskHandle_t taskDebug = NULL;
TaskHandle_t taskLED = NULL;

// Mutex pour protéger les données partagées
SemaphoreHandle_t xMutexData = NULL;

// ============== LED Blink Control ==============
volatile int ledBlinkCount = 0;      // Nombre de clignotements à effectuer (0 = arrêté)
volatile int ledBlinkPeriod = 500;   // Période de clignotement en ms (par défaut 500ms)
SemaphoreHandle_t xMutexLED = NULL;  // Mutex pour protéger les paramètres LED

// Structure de données partagées entre les tâches
struct SensorData {
    float debit1;           // Débit PCNT/Interruption #1
    float debit2;           // Débit PCNT/Interruption #2
    float debit3;           // Débit PCNT/Interruption #3
    float debit4;           // Débit PCNT/Interruption #4
    float debit1_4_20mA;    // Débit 4-20mA canal 1
    float debit2_4_20mA;    // Débit 4-20mA canal 2
    float debit3_4_20mA;    // Débit 4-20mA canal 3
    float debit4_4_20mA;    // Débit 4-20mA canal 4
    bool sensor1_connected; // État connexion capteur 1
    bool sensor2_connected; // État connexion capteur 2
    bool sensor3_connected; // État connexion capteur 3
    bool sensor4_connected; // État connexion capteur 4
    float pressure;
    int pressureRawMilliVolts;  // Tension brute ADC en mV (debug/calibration)
    float sat;
    float lon;
    float llat;
    float sspeed;
} sensorData = {0};

// Configuration BLE
String device_name = "debitdouille-";

// Déclaration forward de la fonction LED
void triggerLEDBlink(int count, int period_ms);

// ============================================
// 📦 OTA BLE — mise à jour du firmware depuis l'app Flutter
// ============================================
// Protocole complet : docs/ota.md. Résumé :
//   1. App -> {"cmd":"ota_begin","size":N,"sha256":"...","esp_model":"...","chunk_size":X}
//   2. ESP -> {"event":"ota_ready","chunk_size":X,"window_bytes":W} (après vérif modèle + Update.begin)
//   3. App -> chunks binaires bruts sur RX ; ESP -> {"event":"ota_progress","bytes":N} (accusés
//      de réception servant de flow control : l'app garde au plus W octets non confirmés en vol)
//   4. À réception de `size` octets : vérif SHA-256, Update.end(), {"event":"ota_success"}, reboot.
// Le callback BLE ne fait que copier les octets dans un stream buffer ; une tâche
// éphémère (otaTransferTask) écrit la flash pour ne jamais bloquer la pile BLE.
enum OtaState { OTA_IDLE, OTA_RECEIVING };
volatile OtaState otaState = OTA_IDLE;
volatile bool otaAbortRequest = false;
uint32_t otaExpectedSize = 0;
volatile uint32_t otaReceived = 0;
uint8_t otaExpectedSha[32];
StreamBufferHandle_t otaStreamBuf = NULL;
TaskHandle_t taskOTA = NULL;

#define OTA_STREAM_BUF_SIZE 12288  // > fenêtre + 1 chunk : ne déborde jamais grâce au flow control
#define OTA_WINDOW_BYTES    8192   // octets non confirmés max autorisés à l'app
#define OTA_ACK_STEP_BYTES  4096   // un ota_progress tous les N octets écrits en flash
#define OTA_MAX_CHUNK       480    // borne haute du chunk_size négocié (l'app propose ≤ MTU-3)
#define OTA_TIMEOUT_MS      30000  // session abandonnée si aucune donnée pendant ce délai

// Compatibilité mbedtls 2.x (core Arduino 2.x / IDF4, API *_ret) et 3.x (core 3.x / IDF5)
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
  #define OTA_SHA256_STARTS(c)      mbedtls_sha256_starts(c, 0)
  #define OTA_SHA256_UPDATE(c,d,l)  mbedtls_sha256_update(c, d, l)
  #define OTA_SHA256_FINISH(c,o)    mbedtls_sha256_finish(c, o)
#else
  #define OTA_SHA256_STARTS(c)      mbedtls_sha256_starts_ret(c, 0)
  #define OTA_SHA256_UPDATE(c,d,l)  mbedtls_sha256_update_ret(c, d, l)
  #define OTA_SHA256_FINISH(c,o)    mbedtls_sha256_finish_ret(c, o)
#endif

void sendOtaEvent(const String &json) {
    Serial.println("OTA -> " + json);
    if (deviceConnected) {
        pTxCharacteristic->setValue(json.c_str());
        pTxCharacteristic->notify();
    }
}

void otaCleanup() {
    otaState = OTA_IDLE;
    otaAbortRequest = false;
    otaReceived = 0;
    otaExpectedSize = 0;
    if (otaStreamBuf != NULL) {
        vStreamBufferDelete(otaStreamBuf);
        otaStreamBuf = NULL;
    }
}

// Convertit le SHA-256 hexadécimal (64 caractères) du manifest en 32 octets. false si invalide.
bool parseShaHex(const char *hex, uint8_t *out) {
    if (hex == NULL || strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        char hi = tolower(hex[2 * i]);
        char lo = tolower(hex[2 * i + 1]);
        if (!isxdigit(hi) || !isxdigit(lo)) return false;
        uint8_t h = (hi <= '9') ? hi - '0' : hi - 'a' + 10;
        uint8_t l = (lo <= '9') ? lo - '0' : lo - 'a' + 10;
        out[i] = (h << 4) | l;
    }
    return true;
}

// Tâche éphémère créée par ota_begin : consomme le stream buffer rempli par le
// callback BLE, écrit la flash, envoie les accusés ota_progress, puis finalise
// (SHA-256 + Update.end) et reboote. S'autodétruit en cas d'erreur ou d'abort.
void otaTransferTask(void *pvParameters) {
    uint8_t buf[1024];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    OTA_SHA256_STARTS(&sha);

    uint32_t lastAckedBytes = 0;
    unsigned long lastActivity = millis();
    const char *failReason = NULL;

    while (otaReceived < otaExpectedSize) {
        size_t n = xStreamBufferReceive(otaStreamBuf, buf, sizeof(buf), pdMS_TO_TICKS(250));
        if (otaAbortRequest) {
            failReason = deviceConnected ? "aborted" : "ble_disconnected";
            break;
        }
        if (n == 0) {
            if (millis() - lastActivity > OTA_TIMEOUT_MS) { failReason = "timeout"; break; }
            continue;
        }
        lastActivity = millis();
        if (Update.write(buf, n) != n) { failReason = "flash_write"; break; }
        OTA_SHA256_UPDATE(&sha, buf, n);
        otaReceived += n;
        if (otaReceived - lastAckedBytes >= OTA_ACK_STEP_BYTES || otaReceived >= otaExpectedSize) {
            lastAckedBytes = otaReceived;
            sendOtaEvent(String("{\"event\":\"ota_progress\",\"bytes\":") + otaReceived + "}");
        }
    }

    if (failReason == NULL) {
        uint8_t digest[32];
        OTA_SHA256_FINISH(&sha, digest);
        if (memcmp(digest, otaExpectedSha, 32) != 0) {
            failReason = "sha_mismatch";
        } else if (!Update.end(true)) {
            failReason = "flash_end";
        }
    }
    mbedtls_sha256_free(&sha);

    if (failReason != NULL) {
        if (Update.isRunning()) Update.abort();
        Serial.printf("OTA: échec (%s) après %u octets\r\n", failReason, otaReceived);
        sendOtaEvent(String("{\"event\":\"ota_error\",\"reason\":\"") + failReason + "\"}");
        otaCleanup();
        taskOTA = NULL;
        vTaskDelete(NULL);
        return;
    }

    sendOtaEvent("{\"event\":\"ota_success\"}");
    Serial.println("OTA: firmware écrit et vérifié, redémarrage...");
    vTaskDelay(pdMS_TO_TICKS(700));  // laisser partir la notification avant le reboot
    ESP.restart();
}

// Callbacks BLE pour gérer la connexion
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("Client connecté");
      // Déclencher 3 clignotements rapides lors de la connexion (200ms période)
      triggerLEDBlink(3, 200);
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("Client déconnecté");
      // Une déconnexion pendant un transfert OTA annule la session (reprise impossible)
      if (otaState == OTA_RECEIVING) otaAbortRequest = true;
      // Déclencher 1 clignotement long lors de la déconnexion (1000ms période)
      triggerLEDBlink(1, 1000);
    }
};

// Callback pour recevoir des données
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      std::string rxValue = pCharacteristic->getValue();

      if (rxValue.length() > 0) {
        // Session OTA active : tout ce qui arrive sur RX est du binaire firmware.
        // Exception : avant le tout premier chunk, un JSON (ota_abort) reste accepté —
        // sans ambiguïté car un binaire ESP32 commence par le magic 0xE9, jamais par '{'.
        if (otaState == OTA_RECEIVING) {
          bool isPreStreamJson = (otaReceived == 0 &&
                                  xStreamBufferIsEmpty(otaStreamBuf) == pdTRUE &&
                                  rxValue[0] == '{');
          if (!isPreStreamJson) {
            size_t sent = xStreamBufferSend(otaStreamBuf, rxValue.data(), rxValue.length(), pdMS_TO_TICKS(200));
            if (sent != rxValue.length()) {
              // Ne devrait pas arriver (flow control par fenêtre) : le SHA-256 le détectera
              Serial.println("OTA: stream buffer plein, octets perdus");
            }
            return;
          }
        }

        String receivedString = "";
        for (int i = 0; i < rxValue.length(); i++) {
          receivedString += rxValue[i];
        }

        // Détection du format : JSON (commence par '{') ou ancien format "commande:valeur"
        if (receivedString.startsWith("{")) {
          // ========== FORMAT JSON ==========
          StaticJsonDocument<1024> doc;
          DeserializationError error = deserializeJson(doc, receivedString);

          if (error) {
            Serial.print("Erreur parsing JSON: ");
            Serial.println(error.c_str());
            return;
          }

          // Commande: get_coeff
          if (doc.containsKey("get_coeff") && doc["get_coeff"] == true) {
            messageRecu = "get_coeff";
            newData = true;
            Serial.println("Commande JSON reçue: get_coeff");
          }
          // Commande: get_info (infos firmware)
          else if (doc.containsKey("get_info") && doc["get_info"] == true) {
            messageRecu = "get_info";
            newData = true;
            Serial.println("Commande JSON reçue: get_info");
          }
          // Commande: update_coeff
          else if (doc.containsKey("update_coeff")) {
            messageRecu = "update_coeff";
            // Stocker le sous-objet pour traitement ultérieur dans taskHandleCommands
            valeurRecueStr = receivedString;  // Garder le JSON complet
            newData = true;
            Serial.println("Commande JSON reçue: update_coeff");
          }
          // Commandes OTA: {"cmd":"ota_begin"|"ota_abort"|"ota_rollback", ...}
          else if (doc.containsKey("cmd")) {
            const char *cmd = doc["cmd"];
            if (strcmp(cmd, "ota_begin") == 0 || strcmp(cmd, "ota_abort") == 0 || strcmp(cmd, "ota_rollback") == 0) {
              messageRecu = String(cmd);
              valeurRecueStr = receivedString;  // Garder le JSON complet
              newData = true;
              Serial.println("Commande JSON reçue: " + messageRecu);
            } else {
              Serial.println("Commande JSON inconnue");
            }
          }
          else {
            Serial.println("Commande JSON inconnue");
          }
        }
        else {
          // ========== FORMAT ANCIEN "commande:valeur" ==========
          int separatorIndex = receivedString.indexOf(':');
          if (separatorIndex > 0) {
            messageRecu = receivedString.substring(0, separatorIndex);
            String valeurStr = receivedString.substring(separatorIndex + 1);
            valeurRecue = valeurStr.toInt();
            valeurRecueStr = valeurStr;  // Garder aussi la version string
            newData = true;

            Serial.print("Message reçu (ancien format): ");
            Serial.print(messageRecu);
            Serial.print(" Valeur: ");
            Serial.println(valeurStr);
          }
        }
      }
    }
};

// Déclarations de fonctions
void setupPCNT();
// int calculerMedian();
// float calculerMoyenneSansOutliers();
void setupBLE();

void setupBLE() {
    // Initialiser le périphérique BLE
    BLEDevice::init(device_name.c_str());

    // MTU local élevé : permet à l'app de négocier de gros paquets (chunks OTA plus rapides)
    BLEDevice::setMTU(517);

    // Créer le serveur BLE
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    // Créer le service BLE (Nordic UART Service)
    BLEService *pService = pServer->createService(BLE_SERVICE_UUID);

    // Créer la caractéristique TX (ESP32 -> App)
    pTxCharacteristic = pService->createCharacteristic(
        BLE_TX_CHAR_UUID,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    // Créer la caractéristique RX (App -> ESP32)
    // WRITE_NR (write without response) requis pour le streaming rapide des chunks OTA
    pRxCharacteristic = pService->createCharacteristic(
        BLE_RX_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    pRxCharacteristic->setCallbacks(new MyCallbacks());

    // Démarrer le service
    pService->start();

    // Configurer et démarrer la publicité (advertising)
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
    pAdvertising->setScanResponse(false);
    pAdvertising->setMinPreferred(0x0);
    BLEDevice::startAdvertising();

    Serial.println("En attente de connexion BLE...");
}

void setupPulseCounter() {
#if DEBITDOUILLE_VERSION == V2_S3
    // Configuration des pull-ups pour stabilité (comme les interruptions)
    pinMode(DEBITMETRE1_PIN, INPUT_PULLUP);
    pinMode(DEBITMETRE2_PIN, INPUT_PULLUP);
    pinMode(DEBITMETRE3_PIN, INPUT_PULLUP);
    pinMode(DEBITMETRE4_PIN, INPUT_PULLUP);

    // Configuration du PCNT pour ESP32-S3 (4 canaux)
    // Comptage sur front DESCENDANT (FALLING) comme les interruptions pour meilleure stabilité
    pcnt_config_t pcntConfig1 = {
        .pulse_gpio_num = DEBITMETRE1_PIN,
        .ctrl_gpio_num = PCNT_PIN_NOT_USED,
        .pos_mode = PCNT_COUNT_DIS,        // Pas de comptage sur front montant
        .neg_mode = PCNT_COUNT_INC,        // Comptage sur front descendant (FALLING)
        .counter_h_lim = 32767,
        .counter_l_lim = -32768,
        .unit = pcntUnit1,
        .channel = PCNT_CHANNEL_0,
    };
    pcnt_unit_config(&pcntConfig1);

    pcnt_config_t pcntConfig2 = {
        .pulse_gpio_num = DEBITMETRE2_PIN,
        .ctrl_gpio_num = PCNT_PIN_NOT_USED,
        .pos_mode = PCNT_COUNT_DIS,        // Pas de comptage sur front montant
        .neg_mode = PCNT_COUNT_INC,        // Comptage sur front descendant (FALLING)
        .counter_h_lim = 32767,
        .counter_l_lim = -32768,
        .unit = pcntUnit2,
        .channel = PCNT_CHANNEL_0,
    };
    pcnt_unit_config(&pcntConfig2);

    pcnt_config_t pcntConfig3 = {
        .pulse_gpio_num = DEBITMETRE3_PIN,
        .ctrl_gpio_num = PCNT_PIN_NOT_USED,
        .pos_mode = PCNT_COUNT_DIS,        // Pas de comptage sur front montant
        .neg_mode = PCNT_COUNT_INC,        // Comptage sur front descendant (FALLING)
        .counter_h_lim = 32767,
        .counter_l_lim = -32768,
        .unit = pcntUnit3,
        .channel = PCNT_CHANNEL_0,
    };
    pcnt_unit_config(&pcntConfig3);

    pcnt_config_t pcntConfig4 = {
        .pulse_gpio_num = DEBITMETRE4_PIN,
        .ctrl_gpio_num = PCNT_PIN_NOT_USED,
        .pos_mode = PCNT_COUNT_DIS,        // Pas de comptage sur front montant
        .neg_mode = PCNT_COUNT_INC,        // Comptage sur front descendant (FALLING)
        .counter_h_lim = 32767,
        .counter_l_lim = -32768,
        .unit = pcntUnit4,
        .channel = PCNT_CHANNEL_0,
    };
    pcnt_unit_config(&pcntConfig4);

    // Configuration du filtre glitch (anti-rebond)
    // Valeur = nombre de cycles APB (80MHz) pour filtrer les glitches
    // 1000 cycles @ 80MHz = 12.5 µs de filtrage
    // Rejette les pulses parasites < 12.5 µs (débitmètre pulse ~7-15 ms)
    pcnt_set_filter_value(pcntUnit1, 1000);
    pcnt_filter_enable(pcntUnit1);
    pcnt_set_filter_value(pcntUnit2, 1000);
    pcnt_filter_enable(pcntUnit2);
    pcnt_set_filter_value(pcntUnit3, 1000);
    pcnt_filter_enable(pcntUnit3);
    pcnt_set_filter_value(pcntUnit4, 1000);
    pcnt_filter_enable(pcntUnit4);

    pcnt_counter_pause(pcntUnit1);
    pcnt_counter_clear(pcntUnit1);
    pcnt_counter_resume(pcntUnit1);

    pcnt_counter_pause(pcntUnit2);
    pcnt_counter_clear(pcntUnit2);
    pcnt_counter_resume(pcntUnit2);

    pcnt_counter_pause(pcntUnit3);
    pcnt_counter_clear(pcntUnit3);
    pcnt_counter_resume(pcntUnit3);

    pcnt_counter_pause(pcntUnit4);
    pcnt_counter_clear(pcntUnit4);
    pcnt_counter_resume(pcntUnit4);

    Serial.println("PCNT initialisé avec filtre glitch (4 canaux - ESP32-S3)");
#else
    // Configuration des interruptions pour ESP32-C3 et V1 (4 canaux)
    pinMode(DEBITMETRE1_PIN, INPUT_PULLUP);
    pinMode(DEBITMETRE2_PIN, INPUT_PULLUP);
    pinMode(DEBITMETRE3_PIN, INPUT_PULLUP);
    pinMode(DEBITMETRE4_PIN, INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(DEBITMETRE1_PIN), pulseCounter1, FALLING);
    attachInterrupt(digitalPinToInterrupt(DEBITMETRE2_PIN), pulseCounter2, FALLING);
    attachInterrupt(digitalPinToInterrupt(DEBITMETRE3_PIN), pulseCounter3, FALLING);
    attachInterrupt(digitalPinToInterrupt(DEBITMETRE4_PIN), pulseCounter4, FALLING);

    Serial.println("Interruptions initialisées (4 canaux - ESP32-C3)");
#endif
}

// ============== Tâches FreeRTOS ==============

// Tâche 1: Lecture des débitmètres (toutes les 1000 ms)
void taskReadPulseCounters(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(delay_task1_ReadPulse);  // 1000 ms

    unsigned long lastTime = millis();

    for(;;) {
        // Attendre le prochain cycle (précis à ±1ms)
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        unsigned long currentTime = millis();
        unsigned long deltaTime = currentTime - lastTime;//non utilisé si PCNT 

        uint32_t count1_local = 0;
        uint32_t count2_local = 0;
        uint32_t count3_local = 0;
        uint32_t count4_local = 0;

#if DEBITDOUILLE_VERSION == V2_S3
        // Lecture PCNT pour ESP32-S3 (4 canaux)
        int16_t pcnt1, pcnt2, pcnt3, pcnt4;
        pcnt_get_counter_value(pcntUnit1, &pcnt1);
        pcnt_get_counter_value(pcntUnit2, &pcnt2);
        pcnt_get_counter_value(pcntUnit3, &pcnt3);
        pcnt_get_counter_value(pcntUnit4, &pcnt4);
        count1_local = pcnt1;
        count2_local = pcnt2;
        count3_local = pcnt3;
        count4_local = pcnt4;

        // Réinitialisation des compteurs PCNT
        pcnt_counter_clear(pcntUnit1);
        pcnt_counter_clear(pcntUnit2);
        pcnt_counter_clear(pcntUnit3);
        pcnt_counter_clear(pcntUnit4);
#else
        // Lecture et réinitialisation des compteurs par interruption (ESP32-C3 et V1)
        portENTER_CRITICAL(&mux1);
        count1_local = pulseCount1;
        pulseCount1 = 0;
        portEXIT_CRITICAL(&mux1);

        portENTER_CRITICAL(&mux2);
        count2_local = pulseCount2;
        pulseCount2 = 0;
        portEXIT_CRITICAL(&mux2);

        portENTER_CRITICAL(&mux3);
        count3_local = pulseCount3;
        pulseCount3 = 0;
        portEXIT_CRITICAL(&mux3);

        portENTER_CRITICAL(&mux4);
        count4_local = pulseCount4;
        pulseCount4 = 0;
        portEXIT_CRITICAL(&mux4);
#endif

        // Calcul des débits (L/min)
        float debit1_calc = (count1_local * 60000.0) / deltaTime / NbImpulsionsDebitmetre1;
        float debit2_calc = (count2_local * 60000.0) / deltaTime / NbImpulsionsDebitmetre2;
        float debit3_calc = (count3_local * 60000.0) / deltaTime / NbImpulsionsDebitmetre3;
        float debit4_calc = (count4_local * 60000.0) / deltaTime / NbImpulsionsDebitmetre4;

        // Mise à jour de la structure partagée (avec mutex)
        if(xSemaphoreTake(xMutexData, portMAX_DELAY) == pdTRUE) {
            sensorData.debit1 = debit1_calc;
            sensorData.debit2 = debit2_calc;
            sensorData.debit3 = debit3_calc;
            sensorData.debit4 = debit4_calc;
            xSemaphoreGive(xMutexData);
        }

        lastTime = currentTime;
    }
}

// Tâche 2: Lecture des débitmètres 4-20mA (toutes les 100 ms)
void taskReadADS1115(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(delay_task_ReadADS1115);  // 100 ms

    for(;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        // Lecture des 4 canaux ADS1115
        int16_t adc0 = ads.readADC_SingleEnded(0);  // Canal 1
        int16_t adc1 = ads.readADC_SingleEnded(1);  // Canal 2
        int16_t adc2 = ads.readADC_SingleEnded(2);  // Canal 3
        int16_t adc3 = ads.readADC_SingleEnded(3);  // Canal 4

        // Conversion ADC vers tension (avec gain ±4.096V)
        float voltage0 = ads.computeVolts(adc0);
        float voltage1 = ads.computeVolts(adc1);
        float voltage2 = ads.computeVolts(adc2);
        float voltage3 = ads.computeVolts(adc3);

        // Conversion tension vers courant (I = V / R)
        float current0 = (voltage0 / SHUNT_RESISTOR) * 1000.0;  // Résultat en mA
        float current1 = (voltage1 / SHUNT_RESISTOR) * 1000.0;
        float current2 = (voltage2 / SHUNT_RESISTOR) * 1000.0;
        float current3 = (voltage3 / SHUNT_RESISTOR) * 1000.0;

        // Détection de la connexion et calcul du débit
        float flow0 = 0.0, flow1 = 0.0, flow2 = 0.0, flow3 = 0.0;
        bool connected0 = false, connected1 = false, connected2 = false, connected3 = false;

        // Canal 0
        if (current0 >= CURRENT_DISCONNECTED) {
            connected0 = true;
            // Conversion 4-20mA vers 0-maxFlow L/min
            flow0 = ((current0 - CURRENT_MIN) / (CURRENT_MAX - CURRENT_MIN)) * maxFlow1;
            if (flow0 < 0) flow0 = 0;
        }

        // Canal 1
        if (current1 >= CURRENT_DISCONNECTED) {
            connected1 = true;
            flow1 = ((current1 - CURRENT_MIN) / (CURRENT_MAX - CURRENT_MIN)) * maxFlow2;
            if (flow1 < 0) flow1 = 0;
        }

        // Canal 2
        if (current2 >= CURRENT_DISCONNECTED) {
            connected2 = true;
            flow2 = ((current2 - CURRENT_MIN) / (CURRENT_MAX - CURRENT_MIN)) * maxFlow3;
            if (flow2 < 0) flow2 = 0;
        }

        // Canal 3
        if (current3 >= CURRENT_DISCONNECTED) {
            connected3 = true;
            flow3 = ((current3 - CURRENT_MIN) / (CURRENT_MAX - CURRENT_MIN)) * maxFlow4;
            if (flow3 < 0) flow3 = 0;
        }

        // Mise à jour de la structure partagée (avec mutex)
        if(xSemaphoreTake(xMutexData, portMAX_DELAY) == pdTRUE) {
            sensorData.debit1_4_20mA = flow0;
            sensorData.debit2_4_20mA = flow1;
            sensorData.debit3_4_20mA = flow2;
            sensorData.debit4_4_20mA = flow3;
            sensorData.sensor1_connected = connected0;
            sensorData.sensor2_connected = connected1;
            sensorData.sensor3_connected = connected2;
            sensorData.sensor4_connected = connected3;
            xSemaphoreGive(xMutexData);
        }
    }
}

// Tâche 3: Lecture de la pression
void taskReadPressure(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(delay_task_ReadPressure);

    for(;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        float pressure_bar = 0.0;
        // Échantillonnage rapide :
        uint32_t sum_mv = 0;
        const uint8_t NB_MES = 10;
        for(int i = 0; i < NB_MES; i++) {
            sum_mv += analogReadMilliVolts(PIN_PRESSURE);
            delayMicroseconds(100);  // 100µs entre chaque lecture
        }
        // Calcul de la moyenne (pas médiane)
        float avg_mv = sum_mv / (float)NB_MES;
        float voltage_v = avg_mv / 1000.0;  // mV → V

        // ========== TYPE 0: Capteur Gravity analogique (0.5-4.5V, 0-16 bar) ==========
        // ========== TYPE 1: Capteur 4-20mA (0-16 bar) en direct sur ESP32 ==========
        if (pressureSensorType == 0) {
            // Conversion Tension 3 fils: utilise les paramètres configurables
            // Permet calibration fine via l'app Flutter
            // Exemple: si capteur Gravity standard → Vmin=0.5V, Vmax=4.5V, Pmax=16 bar
            // Pour corriger un offset (ex: -0.27 bar), ajuster Vmin/Vmax via l'app
            float voltage_range = pressureVoltageMax - pressureVoltageMin;
            if (voltage_range > 0.1) {  // Protection division par zéro
                pressure_bar = (voltage_v - pressureVoltageMin) / voltage_range * pressureMaxBar;
            } else {
                pressure_bar = 0.0;  // Configuration invalide
            }
        }
        else if (pressureSensorType == 1) {
            // Conversion Courant 2 fils: 4-20mA → 0-Pmax bar
            // Résistance de shunt pour 4-20mA pression (150Ω → 0.6V-3.0V)
            // Utilise pressureMaxBar configurable via l'app Flutter
            float current_ma = (voltage_v / PRESSURE_SHUNT_RESISTOR) * 1000.0;

            // Vérification connexion capteur (>3.5mA)
            if (current_ma >= PRESSURE_CURRENT_DISCONNECTED) {
                // Conversion 4-20mA → 0-pressureMaxBar
                pressure_bar = ((current_ma - PRESSURE_CURRENT_MIN) / (PRESSURE_CURRENT_MAX - PRESSURE_CURRENT_MIN)) * pressureMaxBar;
                if (pressure_bar < 0) pressure_bar = 0;
            } else {
                // Capteur déconnecté
                pressure_bar = 0.0;
            }
        }

        // Application des corrections manomètre
        float pressure_calc = (pressure_bar - (correctionManometreB / 100.0)) / (correctionManometreA / 100.0);

        // Mise à jour de la structure partagée (avec mutex)
        if(xSemaphoreTake(xMutexData, portMAX_DELAY) == pdTRUE) {
            sensorData.pressure = pressure_calc;
            sensorData.pressureRawMilliVolts = (int)avg_mv;  // Stockage tension brute pour debug/calibration
            xSemaphoreGive(xMutexData);
        }
    }
}

// Fonction pour réinitialiser le GPS avec inversion des pins RX/TX
void reinitGPSWithSwappedPins() {
    // Arrêter la communication série actuelle
    neogps.end();
    vTaskDelay(pdMS_TO_TICKS(100));  // Petit délai pour s'assurer que le port est libéré

    // Inverser les pins
    uint8_t tempPin = gpsRxPin;
    gpsRxPin = gpsTxPin;
    gpsTxPin = tempPin;

    // Réinitialiser le port série avec les nouvelles pins
    neogps.begin(9600, SERIAL_8N1, gpsRxPin, gpsTxPin);

    // Réinitialiser les compteurs
    gpsFailCounter = 0;
    gpsPinsSwapped = !gpsPinsSwapped;
    lastGpsDataTime = millis();

    // Message de debug
    Serial.printf("║ GNSS -> Nouvelle configuration: RX=GPIO%d, TX=GPIO%d\n", gpsRxPin, gpsTxPin);
    Serial.printf("║ GNSS -> Tentative: %s\n", gpsPinsSwapped ? "Pins inversés" : "Pins d'origine");
}

// Tâche 3: Lecture du GPS (toutes les 100 ms avec détection automatique RX/TX)
void taskReadGPS(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(delay_task_ReadGPS);  // 1000 ms

    // Variables de détection
    bool gpsDataReceived = false;
    unsigned long lastCheckTime = millis();

    for(;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        gpsDataReceived = false;

        // Lecture des données GPS disponibles
        while (neogps.available()) {
            if (gps.encode(neogps.read())) {
                gpsDataReceived = true;
            }
        }

        // Si position GPS valide, mettre à jour les données
        if (gps.location.isValid()) {
            float sat_val = gps.satellites.value();
            float lon_val = gps.location.lng() * 1000000;
            float llat_val = gps.location.lat() * 1000000;
            float sspeed_val = gps.speed.kmph();

            // Mise à jour de la structure partagée (avec mutex)
            if(xSemaphoreTake(xMutexData, portMAX_DELAY) == pdTRUE) {
                sensorData.sat = sat_val;
                sensorData.lon = lon_val;
                sensorData.llat = llat_val;
                sensorData.sspeed = sspeed_val;
                xSemaphoreGive(xMutexData);
            }

            // GPS fonctionne correctement
            gpsFailCounter = 0;
            lastGpsDataTime = millis();
        }
        // Détection d'absence de données GPS
        else if (!gpsDataReceived || !gps.location.isUpdated()) {
            // Incrémenter le compteur d'échecs toutes les secondes
            gpsFailCounter++;

            // Premier basculement après GPS_FAIL_THRESHOLD échecs
            if (gpsFailCounter == GPS_FAIL_THRESHOLD && !gpsPinsSwapped) {
                Serial.println(" 📌   GNSS Aucune donnée reçue, tentative d'inversion RX/TX...");
                reinitGPSWithSwappedPins();
            }
            // Second basculement après GPS_RETRY_THRESHOLD échecs (retour à la config d'origine)
            else if (gpsFailCounter >= GPS_RETRY_THRESHOLD && gpsPinsSwapped) {
                Serial.println(" 📌   GNSS: Toujours pas de signal, retour à la configuration d'origine...");
                reinitGPSWithSwappedPins();
            }
        }
    }
}

// Fonction helper: Récupérer l'adresse MAC complète
String getFullMacAddress() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macStr[18];
    sprintf(macStr, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}

// Tâche 4: Envoi des données BLE en JSON (toutes les 1000 ms)
void taskSendBLE(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(delay_task_SendBLE);// 1000 ms

    // Récupérer l'adresse MAC une seule fois au démarrage de la tâche
    String macAddress = getFullMacAddress();

    for(;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        // Télémétrie suspendue pendant un transfert OTA (la bande passante BLE
        // est réservée aux chunks et aux accusés ota_progress)
        if (deviceConnected && otaState == OTA_IDLE) {
            // Copie locale des données (avec mutex)
            float local_pressure, local_sat, local_lon, local_llat, local_sspeed;
            float local_debit1, local_debit2, local_debit3, local_debit4;
            float local_debit1_420, local_debit2_420, local_debit3_420, local_debit4_420;
            int local_pressure_raw_mv;

            if(xSemaphoreTake(xMutexData, portMAX_DELAY) == pdTRUE) {
                local_pressure = sensorData.pressure;
                local_pressure_raw_mv = sensorData.pressureRawMilliVolts;
                local_sat = sensorData.sat;
                local_lon = sensorData.lon;
                local_llat = sensorData.llat;
                local_sspeed = sensorData.sspeed;

                // Débits PCNT (fréquence)
                local_debit1 = sensorData.debit1;
                local_debit2 = sensorData.debit2;
                local_debit3 = sensorData.debit3;
                local_debit4 = sensorData.debit4;

                // Débits 4-20mA
                local_debit1_420 = sensorData.debit1_4_20mA;
                local_debit2_420 = sensorData.debit2_4_20mA;
                local_debit3_420 = sensorData.debit3_4_20mA;
                local_debit4_420 = sensorData.debit4_4_20mA;

                xSemaphoreGive(xMutexData);
            }

            // Construction du JSON avec ArduinoJson
            // Taille du document : ~350 bytes (estimé pour tous les champs)
            StaticJsonDocument<512> doc;

            // ID de trame (incrémenté à chaque envoi pour détecter les pertes)
            doc["ID"] = frameID++;

            // Adresse MAC (identifiant unique)
            doc["MAC"] = macAddress;

            // Pression (en bars)
            doc["P"] = serialized(String(local_pressure, 2));

            // Tension brute ADC de la pression (mV) - pour debug/calibration
            doc["P_raw_mV"] = local_pressure_raw_mv;

            // Vitesse GPS (km/h)
            doc["V"] = serialized(String(local_sspeed, 2));

            // Débitmètres - Valeurs principales (4-20mA par défaut)
            // Mapping: DG1=ADS0, DG2=ADS1, DD1=ADS2, DD2=ADS3
            doc["DG1"] = serialized(String(local_debit1_420, 2));  // Débitmètre Gauche 1 (ADS canal 0)
            doc["DG2"] = serialized(String(local_debit2_420, 2));  // Débitmètre Gauche 2 (ADS canal 1)
            doc["DD1"] = serialized(String(local_debit3_420, 2));  // Débitmètre Droit 1 (ADS canal 2)
            doc["DD2"] = serialized(String(local_debit4_420, 2));  // Débitmètre Droit 2 (ADS canal 3)

            // Débitmètres - Valeurs PCNT (suffixe "p" pour pulse counter)
#if DEBITDOUILLE_VERSION == V1
            // V1 : seules les pins 32 (PCNT1) et 33 (PCNT2) sont câblées.
            // Câblage utilisateur : Gauche sur pin 32, Droit sur pin 33.
            // On route donc le canal droit (PCNT2) vers DD1p (et non DG2p) pour
            // afficher correctement la paire DG1 (gauche) / DD1 (droit) dans l'app.
            doc["DG1p"] = serialized(String(local_debit1, 2));     // Débitmètre Gauche 1 (PCNT1, pin 32)
            doc["DD1p"] = serialized(String(local_debit2, 2));     // Débitmètre Droit 1 (PCNT2, pin 33)
            doc["DG2p"] = serialized(String(local_debit3, 2));     // Non câblé sur V1 (PCNT3)
            doc["DD2p"] = serialized(String(local_debit4, 2));     // Non câblé sur V1 (PCNT4)
#else
            // Mapping: DG1=PCNT1, DG2=PCNT2, DD1=PCNT4, DD2=PCNT3
            doc["DG1p"] = serialized(String(local_debit1, 2));     // Débitmètre Gauche 1 (PCNT1)
            doc["DG2p"] = serialized(String(local_debit2, 2));     // Débitmètre Gauche 2 (PCNT2)
            doc["DD1p"] = serialized(String(local_debit4, 2));     // Débitmètre Droit 1 (PCNT4)
            doc["DD2p"] = serialized(String(local_debit3, 2));     // Débitmètre Droit 2 (PCNT3)
#endif

            // GPS (optionnel - champs supplémentaires ignorés par l'app)
            doc["SAT"] = serialized(String(local_sat, 0));
            doc["LAT"] = serialized(String(local_llat / 1000000.0, 6));
            doc["LON"] = serialized(String(local_lon / 1000000.0, 6));

            // Sérialisation en string
            String jsonString;
            serializeJson(doc, jsonString);

            // Envoi via BLE
            pTxCharacteristic->setValue(jsonString.c_str());
            pTxCharacteristic->notify();
        }
    }
}

// Tâche 5: Traitement des commandes BLE (toutes les 50 ms)
void taskHandleCommands(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(delay_task_HandleCommands);  // 50 ms

    for(;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        if (newData) {
            // Traiter les commandes
            if (messageRecu == "cns") {
                // Récupération de TOUTES les constantes et envoi en BLE
                preferences.begin("constantes", false);

                // Débitmètres PCNT/Interruptions (pulse/L)
                constDeb1 = preferences.getUInt("constDeb1", 0);
                constDeb2 = preferences.getUInt("constDeb2", 0);
                constDeb3 = preferences.getUInt("constDeb3", 0);
                constDeb4 = preferences.getUInt("constDeb4", 0);

                // Échelles débitmètres 4-20mA (L/min)
                constFlow1 = preferences.getUInt("constFlow1", 0);
                constFlow2 = preferences.getUInt("constFlow2", 0);
                constFlow3 = preferences.getUInt("constFlow3", 0);
                constFlow4 = preferences.getUInt("constFlow4", 0);

                // Corrections manomètre
                constManA = preferences.getUInt("constManA", 0);
                constManB = preferences.getUInt("constManB", 0);
                preferences.end();

                // Application des constantes PCNT si sauvegardées
                if (constDeb1 != 0) NbImpulsionsDebitmetre1 = constDeb1;
                if (constDeb2 != 0) NbImpulsionsDebitmetre2 = constDeb2;
                if (constDeb3 != 0) NbImpulsionsDebitmetre3 = constDeb3;
                if (constDeb4 != 0) NbImpulsionsDebitmetre4 = constDeb4;

                // Application des échelles 4-20mA si sauvegardées
                if (constFlow1 != 0) maxFlow1 = constFlow1;
                if (constFlow2 != 0) maxFlow2 = constFlow2;
                if (constFlow3 != 0) maxFlow3 = constFlow3;
                if (constFlow4 != 0) maxFlow4 = constFlow4;

                // Application des corrections manomètre
                if (constManA) correctionManometreA = constManA;
                if (constManB) correctionManometreB = constManB;

                // Format message BLE: "B;pcnt1;pcnt2;pcnt3;pcnt4;flow1;flow2;flow3;flow4;manoA;manoB"
                String bluetoothMsg = "B;" +
                                     String(NbImpulsionsDebitmetre1) + ";" +
                                     String(NbImpulsionsDebitmetre2) + ";" +
                                     String(NbImpulsionsDebitmetre3) + ";" +
                                     String(NbImpulsionsDebitmetre4) + ";" +
                                     String((int)maxFlow1) + ";" +
                                     String((int)maxFlow2) + ";" +
                                     String((int)maxFlow3) + ";" +
                                     String((int)maxFlow4) + ";" +
                                     String(correctionManometreA) + ";" +
                                     String(correctionManometreB) + "\n";

                if (deviceConnected) {
                    pTxCharacteristic->setValue(bluetoothMsg.c_str());
                    pTxCharacteristic->notify();
                }

                messageRecu = "";
            }
            // ========== COMMANDES DÉBITMÈTRES PCNT/INTERRUPTIONS ==========
            else if (messageRecu == "pcnt1" || messageRecu == "gauche") {  // Rétrocompatibilité "gauche"
                preferences.begin("constantes", false);
                preferences.putUInt("constDeb1", valeurRecue);
                preferences.end();
                messageRecu = "cns";
            }
            else if (messageRecu == "pcnt2" || messageRecu == "droit") {  // Rétrocompatibilité "droit"
                preferences.begin("constantes", false);
                preferences.putUInt("constDeb2", valeurRecue);
                preferences.end();
                messageRecu = "cns";
            }
            else if (messageRecu == "pcnt3") {
                preferences.begin("constantes", false);
                preferences.putUInt("constDeb3", valeurRecue);
                preferences.end();
                messageRecu = "cns";
            }
            else if (messageRecu == "pcnt4") {
                preferences.begin("constantes", false);
                preferences.putUInt("constDeb4", valeurRecue);
                preferences.end();
                messageRecu = "cns";
            }
            // ========== COMMANDES DÉBITMÈTRES 4-20mA ==========
            else if (messageRecu == "flow1") {
                preferences.begin("constantes", false);
                preferences.putUInt("constFlow1", valeurRecue);
                preferences.end();
                messageRecu = "cns";
            }
            else if (messageRecu == "flow2") {
                preferences.begin("constantes", false);
                preferences.putUInt("constFlow2", valeurRecue);
                preferences.end();
                messageRecu = "cns";
            }
            else if (messageRecu == "flow3") {
                preferences.begin("constantes", false);
                preferences.putUInt("constFlow3", valeurRecue);
                preferences.end();
                messageRecu = "cns";
            }
            else if (messageRecu == "flow4") {
                preferences.begin("constantes", false);
                preferences.putUInt("constFlow4", valeurRecue);
                preferences.end();
                messageRecu = "cns";
            }
            // ========== COMMANDES MANOMÈTRE ==========
            else if (messageRecu == "manoA") {
                preferences.begin("constantes", false);
                preferences.putUInt("constManA", valeurRecue);
                preferences.end();
                messageRecu = "cns";
            }
            else if (messageRecu == "manoB") {
                preferences.begin("constantes", false);
                preferences.putUInt("constManB", valeurRecue);
                preferences.end();
                messageRecu = "cns";
            }
            // ========== COMMANDE TYPE CAPTEUR PRESSION ==========
            else if (messageRecu == "sensP") {
                // Changement du type de capteur de pression
                // 0 = Gravity analogique (0.5-4.5V, 0-16 bar)
                // 1 = 4-20mA (0-16 bar)
                if (valeurRecue == 0 || valeurRecue == 1) {
                    pressureSensorType = valeurRecue;
                    preferences.begin("constantes", false);
                    preferences.putUInt("sensP", valeurRecue);
                    preferences.end();
                    Serial.print("Type capteur pression changé: ");
                    Serial.println(valeurRecue == 0 ? "Gravity analogique" : "4-20mA");
                }
                messageRecu = "";
            }
            // ========== COMMANDE CHANGEMENT ID DEVICE ==========
            else if (messageRecu == "devID") {
                // Changement de l'ID du device pour le nom BLE
                // Format: devID:XXXX où XXXX est l'ID souhaité (max 10 caractères)
                if (valeurRecueStr.length() > 0 && valeurRecueStr.length() <= 10) {
                    deviceID = valeurRecueStr;
                    device_name = "debitdouille-" + deviceID;

                    // Sauvegarder le nouvel ID
                    preferences.begin("constantes", false);
                    preferences.putString("deviceID", deviceID);
                    preferences.end();

                    Serial.print("ID device changé: ");
                    Serial.println(deviceID);
                    Serial.print("Nouveau nom BLE: ");
                    Serial.println(device_name);
                    Serial.println("ATTENTION: Redémarrer l'ESP32 pour appliquer le nouveau nom BLE");
                }
                messageRecu = "";
            }

            // ========== COMMANDE GET_COEFF (FORMAT JSON) ==========
            else if (messageRecu == "get_coeff") {
                // Charger les coefficients depuis NVS
                preferences.begin("calibration", true);  // Mode lecture seule

                // Créer le JSON de réponse
                StaticJsonDocument<1024> responseDoc;
                JsonObject coeff = responseDoc.createNestedObject("coeff");

                // Pression : conserver A et B
                JsonObject coeffP = coeff.createNestedObject("P");
                coeffP["A"] = preferences.getFloat("P_A", 1.0);
                coeffP["B"] = preferences.getFloat("P_B", 0.0);

                // Type de capteur de pression (0=Gravity, 1=4-20mA)
                responseDoc["pressureSensorType"] = preferences.getUInt("sensP", 0);

                // Nouveaux paramètres de calibration pression (mode Tension 3 fils)
                responseDoc["pressureMaxBar"] = preferences.getFloat("P_max", 16.0);
                responseDoc["pressureVoltageMin"] = preferences.getFloat("P_Vmin", 0.5);
                responseDoc["pressureVoltageMax"] = preferences.getFloat("P_Vmax", 4.5);

                // Débitmètres : PPL (pulse per liter) et flow (débit max en L/min)
                // DG1 = debit1_4_20mA, DD1 = debit2_4_20mA, DG2 = debit3_4_20mA, DD2 = debit4_4_20mA
                JsonObject coeffDG1 = coeff.createNestedObject("DG1");
                coeffDG1["PPL"] = preferences.getUInt("DG1_PPL", NbImpulsionsDebitmetre1);
                coeffDG1["flow"] = preferences.getUInt("DG1_flow", maxFlow1);

                JsonObject coeffDD1 = coeff.createNestedObject("DD1");
                coeffDD1["PPL"] = preferences.getUInt("DD1_PPL", NbImpulsionsDebitmetre2);
                coeffDD1["flow"] = preferences.getUInt("DD1_flow", maxFlow2);

                JsonObject coeffDG2 = coeff.createNestedObject("DG2");
                coeffDG2["PPL"] = preferences.getUInt("DG2_PPL", NbImpulsionsDebitmetre3);
                coeffDG2["flow"] = preferences.getUInt("DG2_flow", maxFlow3);

                JsonObject coeffDD2 = coeff.createNestedObject("DD2");
                coeffDD2["PPL"] = preferences.getUInt("DD2_PPL", NbImpulsionsDebitmetre4);
                coeffDD2["flow"] = preferences.getUInt("DD2_flow", maxFlow4);

                // Anciens débitmètres DG3, DG4, DD3, DD4 (pas utilisés mais pour compatibilité app)//TODO implémenter les valeurs par defaut 
                JsonObject coeffDG3 = coeff.createNestedObject("DG3");
                coeffDG3["PPL"] = preferences.getUInt("DG3_PPL", 1000);
                coeffDG3["flow"] = preferences.getUInt("DG3_flow", 150);

                JsonObject coeffDG4 = coeff.createNestedObject("DG4");
                coeffDG4["PPL"] = preferences.getUInt("DG4_PPL", 1000);
                coeffDG4["flow"] = preferences.getUInt("DG4_flow", 150);

                JsonObject coeffDD3 = coeff.createNestedObject("DD3");
                coeffDD3["PPL"] = preferences.getUInt("DD3_PPL", 1000);
                coeffDD3["flow"] = preferences.getUInt("DD3_flow", 150);

                JsonObject coeffDD4 = coeff.createNestedObject("DD4");
                coeffDD4["PPL"] = preferences.getUInt("DD4_PPL", 1000);
                coeffDD4["flow"] = preferences.getUInt("DD4_flow", 150);

                preferences.end();

                // Sérialiser et envoyer
                String jsonResponse;
                serializeJson(responseDoc, jsonResponse);

                if (deviceConnected) {
                    pTxCharacteristic->setValue(jsonResponse.c_str());
                    pTxCharacteristic->notify();
                    Serial.println("Coefficients envoyés:");
                    Serial.println(jsonResponse);
                }

                messageRecu = "";
            }

            // ========== COMMANDE GET_INFO (FORMAT JSON) - INFOS FIRMWARE ==========
            else if (messageRecu == "get_info") {
                // Créer le JSON de réponse avec les infos firmware
                StaticJsonDocument<256> responseDoc;

                // Construction de la version au format "X.Y.Z" (aligné sur le manifest des releases)
                String version = String(FW_VERSION_MAJOR) + "." + String(FW_VERSION_MINOR) + "." + String(FW_VERSION_PATCH);

                responseDoc["fw_version"] = version;
                responseDoc["build_date"] = FW_BUILD_DATE;
                responseDoc["build_time"] = FW_BUILD_TIME;
                responseDoc["esp_model"] = FW_ESP_MODEL;

                // Sérialiser et envoyer
                String jsonResponse;
                serializeJson(responseDoc, jsonResponse);

                if (deviceConnected) {
                    pTxCharacteristic->setValue(jsonResponse.c_str());
                    pTxCharacteristic->notify();
                    Serial.println("Infos firmware envoyées:");
                    Serial.println(jsonResponse);
                }

                messageRecu = "";
            }

            // ========== COMMANDE UPDATE_COEFF (FORMAT JSON) ==========
            else if (messageRecu == "update_coeff") {
                // Parser le JSON complet stocké dans valeurRecueStr
                StaticJsonDocument<1024> doc;
                DeserializationError error = deserializeJson(doc, valeurRecueStr);

                if (!error && doc.containsKey("update_coeff")) {
                    JsonObject coeffs = doc["update_coeff"];

                    preferences.begin("calibration", false);  // Mode écriture

                    // Sauvegarder le type de capteur de pression
                    if (coeffs.containsKey("pressureSensorType")) {
                        int sensorType = coeffs["pressureSensorType"]["value"].as<int>();
                        preferences.putUInt("sensP", sensorType);
                        pressureSensorType = sensorType;
                        Serial.print("Type capteur pression mis à jour: ");
                        Serial.println(sensorType == 0 ? "Gravity analogique" : "4-20mA");
                    }

                    // Sauvegarder les nouveaux paramètres de calibration pression (mode Tension 3 fils)
                    if (coeffs.containsKey("pressureMaxBar")) {
                        float pMax = coeffs["pressureMaxBar"].as<float>();
                        if (pMax > 0.0) {
                            preferences.putFloat("P_max", pMax);
                            pressureMaxBar = pMax;
                            Serial.printf("Pression max mise à jour: %.2f bar\r\n", pMax);
                        }
                    }
                    if (coeffs.containsKey("pressureVoltageMin")) {
                        float vMin = coeffs["pressureVoltageMin"].as<float>();
                        if (vMin >= 0.0) {
                            preferences.putFloat("P_Vmin", vMin);
                            pressureVoltageMin = vMin;
                            Serial.printf("Tension min mise à jour: %.3f V\r\n", vMin);
                        }
                    }
                    if (coeffs.containsKey("pressureVoltageMax")) {
                        float vMax = coeffs["pressureVoltageMax"].as<float>();
                        if (vMax > 0.0) {
                            preferences.putFloat("P_Vmax", vMax);
                            pressureVoltageMax = vMax;
                            Serial.printf("Tension max mise à jour: %.3f V\r\n", vMax);
                        }
                    }

                    // Sauvegarder les coefficients pour chaque capteur
                    // Pression : A et B
                    if (coeffs.containsKey("P")) {
                        float coeffA = coeffs["P"]["A"].as<float>();
                        float coeffB = coeffs["P"]["B"].as<float>();

                        // Utiliser les valeurs par défaut si non fournies (A=1.0, B=0.0)
                        preferences.putFloat("P_A", (coeffA != 0.0) ? coeffA : 1.0);
                        preferences.putFloat("P_B", coeffB);  // B peut être 0, c'est valide

                        // Mettre à jour les variables globales
                        if (coeffA != 0.0) correctionManometreA = (int)(coeffA * 100);
                        correctionManometreB = (int)(coeffB * 100);

                        Serial.printf("Coeff P A=%.2f, B=%.2f\r\n", coeffA, coeffB);
                    }

                    // Débitmètres : PPL et flow
                    if (coeffs.containsKey("DG1")) {
                        // Récupérer les valeurs avec valeurs par défaut si absentes
                        uint32_t ppl = coeffs["DG1"]["PPL"].as<uint32_t>();
                        uint32_t flow = coeffs["DG1"]["flow"].as<uint32_t>();

                        // Sauvegarder dans NVS
                        preferences.putUInt("DG1_PPL", ppl ? ppl : NbImpulsionsDebitmetre1);
                        preferences.putUInt("DG1_flow", flow ? flow : maxFlow1);

                        // Mettre à jour les variables globales
                        if (ppl) NbImpulsionsDebitmetre1 = ppl;
                        if (flow) maxFlow1 = flow;

                        Serial.printf("Coeff DG1 PPL=%u, Max flow=%uL/min\r\n", NbImpulsionsDebitmetre1, maxFlow1);
                    }
                    if (coeffs.containsKey("DD1")) {
                        // Récupérer les valeurs avec valeurs par défaut si absentes
                        uint32_t ppl = coeffs["DD1"]["PPL"].as<uint32_t>();
                        uint32_t flow = coeffs["DD1"]["flow"].as<uint32_t>();

                        // Sauvegarder dans NVS
                        preferences.putUInt("DD1_PPL", ppl ? ppl : NbImpulsionsDebitmetre2);
                        preferences.putUInt("DD1_flow", flow ? flow : maxFlow2);

                        // Mettre à jour les variables globales
                        if (ppl) NbImpulsionsDebitmetre2 = ppl;
                        if (flow) maxFlow2 = flow;

                        Serial.printf("Coeff DD1 PPL=%u, Max flow=%uL/min\r\n", NbImpulsionsDebitmetre2, maxFlow2);
                    }
                    if (coeffs.containsKey("DG2")) {
                        // Récupérer les valeurs avec valeurs par défaut si absentes
                        uint32_t ppl = coeffs["DG2"]["PPL"].as<uint32_t>();
                        uint32_t flow = coeffs["DG2"]["flow"].as<uint32_t>();

                        // Sauvegarder dans NVS
                        preferences.putUInt("DG2_PPL", ppl ? ppl : NbImpulsionsDebitmetre3);
                        preferences.putUInt("DG2_flow", flow ? flow : maxFlow3);

                        // Mettre à jour les variables globales
                        if (ppl) NbImpulsionsDebitmetre3 = ppl;
                        if (flow) maxFlow3 = flow;

                        Serial.printf("Coeff DG2 PPL=%u, Max flow=%uL/min\r\n", NbImpulsionsDebitmetre3, maxFlow3);
                    }
                    if (coeffs.containsKey("DD2")) {
                        // Récupérer les valeurs avec valeurs par défaut si absentes
                        uint32_t ppl = coeffs["DD2"]["PPL"].as<uint32_t>();
                        uint32_t flow = coeffs["DD2"]["flow"].as<uint32_t>();

                        // Sauvegarder dans NVS
                        preferences.putUInt("DD2_PPL", ppl ? ppl : NbImpulsionsDebitmetre4);
                        preferences.putUInt("DD2_flow", flow ? flow : maxFlow4);

                        // Mettre à jour les variables globales
                        if (ppl) NbImpulsionsDebitmetre4 = ppl;
                        if (flow) maxFlow4 = flow;

                        Serial.printf("Coeff DD2 PPL=%u, Max flow=%uL/min\r\n", NbImpulsionsDebitmetre4, maxFlow4);
                    }
                    // DG3, DG4, DD3, DD4 : Débitmètres 5-8 (non implémentés matériellement)
                    // Ces débitmètres sont gardés pour compatibilité app mais n'ont pas de hardware associé
                    if (coeffs.containsKey("DG3")) {
                        uint32_t ppl = coeffs["DG3"]["PPL"].as<uint32_t>();
                        uint32_t flow = coeffs["DG3"]["flow"].as<uint32_t>();
                        preferences.putUInt("DG3_PPL", ppl ? ppl : 1000);
                        preferences.putUInt("DG3_flow", flow ? flow : 150);
                        Serial.printf("Coeff DG3 PPL=%u, Max flow=%uL/min (non utilisé)\r\n", ppl, flow);
                    }
                    if (coeffs.containsKey("DG4")) {
                        uint32_t ppl = coeffs["DG4"]["PPL"].as<uint32_t>();
                        uint32_t flow = coeffs["DG4"]["flow"].as<uint32_t>();
                        preferences.putUInt("DG4_PPL", ppl ? ppl : 1000);
                        preferences.putUInt("DG4_flow", flow ? flow : 150);
                        Serial.printf("Coeff DG4 PPL=%u, Max flow=%uL/min (non utilisé)\r\n", ppl, flow);
                    }
                    if (coeffs.containsKey("DD3")) {
                        uint32_t ppl = coeffs["DD3"]["PPL"].as<uint32_t>();
                        uint32_t flow = coeffs["DD3"]["flow"].as<uint32_t>();
                        preferences.putUInt("DD3_PPL", ppl ? ppl : 1000);
                        preferences.putUInt("DD3_flow", flow ? flow : 150);
                        Serial.printf("Coeff DD3 PPL=%u, Max flow=%uL/min (non utilisé)\r\n", ppl, flow);
                    }
                    if (coeffs.containsKey("DD4")) {
                        uint32_t ppl = coeffs["DD4"]["PPL"].as<uint32_t>();
                        uint32_t flow = coeffs["DD4"]["flow"].as<uint32_t>();
                        preferences.putUInt("DD4_PPL", ppl ? ppl : 1000);
                        preferences.putUInt("DD4_flow", flow ? flow : 150);
                        Serial.printf("Coeff DD4 PPL=%u, Max flow=%uL/min (non utilisé)\r\n", ppl, flow);
                    }

                    preferences.end();

                    Serial.println("Coefficients de calibration sauvegardés");
                }
                else {
                    Serial.println("Erreur parsing update_coeff JSON");
                }

                messageRecu = "";
            }

            // ========== COMMANDE OTA_BEGIN (MISE À JOUR FIRMWARE PAR BLE) ==========
            else if (messageRecu == "ota_begin") {
                StaticJsonDocument<512> doc;
                DeserializationError err = deserializeJson(doc, valeurRecueStr);
                String errorJson = "";

                if (otaState != OTA_IDLE || taskOTA != NULL) {
                    errorJson = "{\"event\":\"ota_error\",\"reason\":\"already_running\"}";
                }
                else if (err || !doc.containsKey("size") || !doc.containsKey("sha256") || !doc.containsKey("esp_model")) {
                    errorJson = "{\"event\":\"ota_error\",\"reason\":\"bad_request\"}";
                }
                // Protection anti-brick : refus si le binaire ne cible pas exactement ce modèle
                else if (strcmp(doc["esp_model"] | "", FW_ESP_MODEL) != 0) {
                    errorJson = String("{\"event\":\"ota_error\",\"reason\":\"model_mismatch\",\"expected\":\"")
                                + FW_ESP_MODEL + "\",\"got\":\"" + (doc["esp_model"] | "") + "\"}";
                }
                else if (!parseShaHex(doc["sha256"] | "", otaExpectedSha)) {
                    errorJson = "{\"event\":\"ota_error\",\"reason\":\"bad_sha256\"}";
                }
                else {
                    uint32_t size = doc["size"];
                    if (size == 0 || !Update.begin(size)) {
                        errorJson = "{\"event\":\"ota_error\",\"reason\":\"size_too_large\"}";
                    } else {
                        otaStreamBuf = xStreamBufferCreate(OTA_STREAM_BUF_SIZE, 1);
                        if (otaStreamBuf == NULL) {
                            Update.abort();
                            errorJson = "{\"event\":\"ota_error\",\"reason\":\"no_memory\"}";
                        } else {
                            otaExpectedSize = size;
                            otaReceived = 0;
                            otaAbortRequest = false;
                            uint32_t chunkSize = doc["chunk_size"] | 240;
                            chunkSize = constrain(chunkSize, (uint32_t)20, (uint32_t)OTA_MAX_CHUNK);
                            if (xTaskCreatePinnedToCore(otaTransferTask, "OtaTransfer", 8192, NULL, 3, &taskOTA, 1) != pdPASS) {
                                Update.abort();
                                otaCleanup();
                                errorJson = "{\"event\":\"ota_error\",\"reason\":\"no_memory\"}";
                            } else {
                                // RECEIVING avant le notify : le premier chunk peut arriver juste derrière
                                otaState = OTA_RECEIVING;
                                Serial.printf("OTA: session ouverte, %u octets attendus, chunk=%u\r\n", size, chunkSize);
                                sendOtaEvent(String("{\"event\":\"ota_ready\",\"chunk_size\":") + chunkSize
                                             + ",\"window_bytes\":" + OTA_WINDOW_BYTES + "}");
                            }
                        }
                    }
                }

                if (errorJson.length() > 0) sendOtaEvent(errorJson);
                messageRecu = "";
            }

            // ========== COMMANDE OTA_ABORT ==========
            else if (messageRecu == "ota_abort") {
                // N'est reçue qu'avant le premier chunk (pendant le streaming, RX = binaire ;
                // l'annulation en cours de transfert passe par une déconnexion BLE)
                if (otaState == OTA_RECEIVING) {
                    otaAbortRequest = true;  // otaTransferTask répondra ota_error/aborted et nettoiera
                } else {
                    sendOtaEvent("{\"event\":\"ota_error\",\"reason\":\"no_session\"}");
                }
                messageRecu = "";
            }

            // ========== COMMANDE OTA_ROLLBACK ==========
            else if (messageRecu == "ota_rollback") {
                // Rebascule manuellement sur la partition OTA précédente si elle contient
                // encore une image valide (dépannage après une mise à jour problématique)
                if (otaState == OTA_IDLE && Update.canRollBack()) {
                    sendOtaEvent("{\"event\":\"ota_rollback_ok\"}");
                    vTaskDelay(pdMS_TO_TICKS(700));  // laisser partir la notification
                    Update.rollBack();
                    ESP.restart();
                } else {
                    sendOtaEvent("{\"event\":\"ota_error\",\"reason\":\"no_backup\"}");
                }
                messageRecu = "";
            }

            newData = false;
        }
    }
}

// Tâche 6: Affichage debug sur port série (toutes les 5000 ms)
void taskDebugSerial(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(delay_task_DebugSerial);  // 5000 ms = 5 secondes

    static uint16_t lineCounter = 0;  // Compteur pour afficher l'en-tête toutes les 20 lignes

    for(;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        // Copie locale des données (avec mutex)
        float local_pressure, local_sat, local_lon, local_llat, local_sspeed;
        float local_debit1, local_debit2, local_debit3, local_debit4;
        float local_debit1_4_20, local_debit2_4_20, local_debit3_4_20, local_debit4_4_20;
        bool local_sensor1_conn, local_sensor2_conn, local_sensor3_conn, local_sensor4_conn;

        if(xSemaphoreTake(xMutexData, portMAX_DELAY) == pdTRUE) {
            local_pressure = sensorData.pressure;
            local_sat = sensorData.sat;
            local_lon = sensorData.lon;
            local_llat = sensorData.llat;
            local_sspeed = sensorData.sspeed;
            local_debit1 = sensorData.debit1;
            local_debit2 = sensorData.debit2;
            local_debit3 = sensorData.debit3;
            local_debit4 = sensorData.debit4;
            local_debit1_4_20 = sensorData.debit1_4_20mA;
            local_debit2_4_20 = sensorData.debit2_4_20mA;
            local_debit3_4_20 = sensorData.debit3_4_20mA;
            local_debit4_4_20 = sensorData.debit4_4_20mA;
            local_sensor1_conn = sensorData.sensor1_connected;
            local_sensor2_conn = sensorData.sensor2_connected;
            local_sensor3_conn = sensorData.sensor3_connected;
            local_sensor4_conn = sensorData.sensor4_connected;
            xSemaphoreGive(xMutexData);
        }

        // Afficher l'en-tête toutes les 20 mesures
        if (lineCounter % 20 == 0) {
            Serial.println("\n╔════════════════════════════════════════════════════════════════════════════════╗");
            Serial.print("║ Device ID: "); Serial.print(deviceID);
            Serial.print(" | FW: v"); Serial.print(FW_VERSION_MAJOR); Serial.print("."); Serial.print(FW_VERSION_MINOR);
            Serial.print(" ("); Serial.print(FW_BUILD_DATE); Serial.print(")");
            Serial.println();
            Serial.print("║ BLE: "); Serial.print(device_name);
            Serial.print(" | Connecté: "); Serial.println(deviceConnected ? "OUI" : "NON");
            Serial.print("║ GPS - Sat: "); Serial.print(local_sat, 0);
            Serial.print(" | Lat: "); Serial.print(local_llat / 1000000.0, 6);
            Serial.print(" | Lon: "); Serial.println(local_lon / 1000000.0, 6);
            Serial.println("╚════════════════════════════════════════════════════════════════════════════════╝");
            Serial.println("P | PCNT1 | PCNT2 | PCNT3 | PCNT4 | 4-20_1 | 4-20_2 | 4-20_3 | 4-20_4 | Vitesse");
            Serial.println("  (bar)  | L/min | L/min | L/min | L/min | L/min  | L/min  | L/min  | L/min  | (km/h)");
        }

        // Affichage des données en format tableau
        // Pression
        Serial.print("  ");
        Serial.print(local_pressure, 2);
        Serial.print("  |");

        // Débits PCNT/Interruptions (2 canaux actifs, 2 réservés)
        Serial.print("  ");
        Serial.print(local_debit1, 2);
        Serial.print(" |");

        Serial.print("  ");
        Serial.print(local_debit2, 2);
        Serial.print(" |");

        Serial.print("  ");
        Serial.print(local_debit3, 2);
        Serial.print(" |");

        Serial.print("  ");
        Serial.print(local_debit4, 2);
        Serial.print(" |");

        // Débits 4-20mA (4 canaux)
        Serial.print(local_sensor1_conn ? " " : "*");
        Serial.print(local_sensor1_conn ? local_debit1_4_20 : 0.0, 2);
        Serial.print(" |");

        Serial.print(local_sensor2_conn ? " " : "*");
        Serial.print(local_sensor2_conn ? local_debit2_4_20 : 0.0, 2);
        Serial.print(" |");

        Serial.print(local_sensor3_conn ? " " : "*");
        Serial.print(local_sensor3_conn ? local_debit3_4_20 : 0.0, 2);
        Serial.print(" |");

        Serial.print(local_sensor4_conn ? " " : "*");
        Serial.print(local_sensor4_conn ? local_debit4_4_20 : 0.0, 2);
        Serial.print(" |");

        // Vitesse GPS
        Serial.print("  ");
        Serial.println(local_sspeed, 1);

        lineCounter++;
    }
}

// Tâche 7: Gestion LED asynchrone avec clignotement paramétrable
void taskLEDBlink(void *pvParameters) {
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);  // LED éteinte au démarrage

    for(;;) {
        int localBlinkCount = 0;
        int localBlinkPeriod = 500;

        // Lire les paramètres de clignotement de manière thread-safe
        if(xSemaphoreTake(xMutexLED, portMAX_DELAY) == pdTRUE) {
            localBlinkCount = ledBlinkCount;
            localBlinkPeriod = ledBlinkPeriod;
            xSemaphoreGive(xMutexLED);
        }

        if (localBlinkCount > 0) {
            // Allumer la LED
            digitalWrite(PIN_LED, HIGH);
            vTaskDelay(pdMS_TO_TICKS(localBlinkPeriod / 2));

            // Éteindre la LED
            digitalWrite(PIN_LED, LOW);
            vTaskDelay(pdMS_TO_TICKS(localBlinkPeriod / 2));

            // Décrémenter le compteur de manière thread-safe
            if(xSemaphoreTake(xMutexLED, portMAX_DELAY) == pdTRUE) {
                ledBlinkCount--;
                xSemaphoreGive(xMutexLED);
            }
        } else {
            // Pas de clignotement actif, attendre un peu
            digitalWrite(PIN_LED, LOW);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// Fonction utilitaire pour déclencher un clignotement
void triggerLEDBlink(int count, int period_ms) {
    if(xSemaphoreTake(xMutexLED, portMAX_DELAY) == pdTRUE) {
        ledBlinkCount = count;
        ledBlinkPeriod = period_ms;
        xSemaphoreGive(xMutexLED);
    }
}

void setupFreeRTOSTasks(void){
    // ============== Création du mutex et des tâches FreeRTOS ==============
    // Créer les mutex pour protéger les données partagées
    xMutexData = xSemaphoreCreateMutex();
    if (xMutexData == NULL) {
        Serial.println("ERREUR: Impossible de créer le mutex data!");
        while(1);  // Bloquer si échec critique
    }

    xMutexLED = xSemaphoreCreateMutex();
    if (xMutexLED == NULL) {
        Serial.println("ERREUR: Impossible de créer le mutex LED!");
        while(1);  // Bloquer si échec critique
    }
    // Créer les tâches FreeRTOS (pinnées sur Core 1, Core 0 réservé pour BLE)

    // Tâche comptage impulsions - Priorité 3 (HAUTE) - Toutes les 1000ms
    xTaskCreatePinnedToCore(
        taskReadPulseCounters,  // Fonction de la tâche
        "ReadPulseCounters",    // Nom de la tâche
        4096,                   // Taille de la pile (bytes)
        NULL,                   // Paramètres de la tâche
        3,                      // Priorité (3 = haute)
        &taskPCNT,              // Handle de la tâche
        1                       // Core 1 (Core 0 pour BLE)
    );

    // Tâche ADS1115 - Priorité 2 (MOYENNE) - Toutes les 100ms
    xTaskCreatePinnedToCore(
        taskReadADS1115,
        "ReadADS1115",
        4096,
        NULL,
        2,                  // Priorité moyenne
        &taskADS1115,
        1
    );

    // Tâche Pression - Priorité 2 (MOYENNE) - Toutes les 20ms
    xTaskCreatePinnedToCore(
        taskReadPressure,
        "ReadPressure",
        4096,
        NULL,
        2,                  // Priorité moyenne
        &taskPressure,
        1
    );

    // Tâche GPS - Priorité 2 (MOYENNE) - Toutes les 100ms
    xTaskCreatePinnedToCore(
        taskReadGPS,
        "ReadGPS",
        4096,
        NULL,
        2,                  // Priorité moyenne
        &taskGPS,
        1
    );

    // Tâche Envoi BLE - Priorité 2 (MOYENNE) - Toutes les 1000ms
    xTaskCreatePinnedToCore(
        taskSendBLE,
        "SendBLE",
        4096,
        NULL,
        2,                  // Priorité moyenne
        &taskBLE,
        1
    );

    // Tâche Commandes - Priorité 1 (BASSE) - Toutes les 50ms
    xTaskCreatePinnedToCore(
        taskHandleCommands,
        "HandleCommands",
        4096,
        NULL,
        1,                  // Priorité basse
        &taskCommands,
        1
    );

    // Tâche Debug Serial - Priorité 1 (BASSE) - Toutes les 5000ms
    xTaskCreatePinnedToCore(
        taskDebugSerial,
        "DebugSerial",
        4096,
        NULL,
        1,                  // Priorité basse
        &taskDebug,
        1
    );

    // Tâche LED Blink - Priorité 1 (BASSE) - Gestion asynchrone LED
    xTaskCreatePinnedToCore(
        taskLEDBlink,
        "LEDBlink",
        2048,               // Taille pile réduite (tâche simple)
        NULL,
        1,                  // Priorité basse
        &taskLED,
        1
    );

    Serial.println("Toutes les tâches FreeRTOS ont été créées avec succès!");
    Serial.println("Affichage des données capteurs toutes les 5 secondes...\n");
}

void setup() {
    Serial.begin(115200);

    // Initialisation GPS avec les pins par défaut (auto-détection activée dans taskReadGPS)
    neogps.begin(9600, SERIAL_8N1, gpsRxPin, gpsTxPin);
    Serial.printf("GPS initialisé: RX=GPIO%d, TX=GPIO%d (auto-détection activée)\n", gpsRxPin, gpsTxPin);

    // Configuration de l'ADC interne (pour le capteur de pression)
    // analogReadMilliVolts() utilise automatiquement la calibration eFuse de l'ESP32
    pinMode(PIN_PRESSURE, INPUT);
    analogSetPinAttenuation(PIN_PRESSURE, ADC_11db);  // Plage 0-3.3V

    // Lecture de stabilisation ADC (jeter les premières lectures)
    for(int i = 0; i < 10; i++) {
        analogReadMilliVolts(PIN_PRESSURE);
        delay(10);
    }

    Serial.println("ADC configuré: 0-3300mV (calibration eFuse) sur GPIO " + String(PIN_PRESSURE));

    // Initialisation I2C
    Wire.begin(PIN_SDA, PIN_SCL);

    // Initialisation ADS1115
    Serial.println("Initialisation ADS1115...");
    if (!ads.begin()) {
        Serial.println("ERREUR: ADS1115 introuvable! Vérifiez le câblage I2C.");
    } else {
        Serial.println("ADS1115 initialisé avec succès (adresse 0x48)");
        // Configuration du gain: ±4.096V (1 bit = 0.125mV)
        ads.setGain(GAIN_ONE);
        Serial.println("Gain configuré: ±4.096V");
    }

    // Récupération de l'ID device depuis les préférences ou génération depuis MAC
    preferences.begin("constantes", false);
    deviceID = preferences.getString("deviceID", "");

    if (deviceID == "") {
        // Générer un ID par défaut à partir de l'adresse MAC (6 derniers caractères)
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char macStr[7];
        sprintf(macStr, "%02X%02X%02X", mac[3], mac[4], mac[5]);
        deviceID = String(macStr);

        // Sauvegarder l'ID par défaut
        preferences.putString("deviceID", deviceID);
        Serial.println("ID device généré depuis MAC: " + deviceID);
    } else {
        Serial.println("ID device chargé depuis NVS: " + deviceID);
    }
    preferences.end();

    // Construire le nom BLE avec l'ID
    device_name = "debitdouille-" + deviceID;

    // Initialisation BLE
    setupBLE();

    // Initialisation comptage d'impulsions (PCNT ou interruptions)
    setupPulseCounter();

    // Configuration de l'alimentation 12V
    pinMode(PIN_PWR_12V_OUT, OUTPUT);
    digitalWrite(PIN_PWR_12V_OUT,HIGH);

    // ========== CHARGEMENT DES PARAMÈTRES DE CALIBRATION ==========
    // Priorité au namespace "calibration" (utilisé par update_coeff depuis l'app Flutter)
    // Si absent, fallback sur "constantes" (anciennes versions) pour migration

    preferences.begin("calibration", false);

    // ===== PRESSION : Type capteur et paramètres =====
    pressureSensorType = preferences.getUInt("sensP", 0);  // 0=Gravity (défaut), 1=4-20mA
    pressureMaxBar = preferences.getFloat("P_max", 0.0);
    pressureVoltageMin = preferences.getFloat("P_Vmin", 0.0);
    pressureVoltageMax = preferences.getFloat("P_Vmax", 0.0);

    // Coefficients de correction pression
    float calibA = preferences.getFloat("P_A", 0.0);
    float calibB = preferences.getFloat("P_B", 0.0);

    // ===== DÉBITMÈTRES : PPL et flow =====
    uint32_t dg1_ppl = preferences.getUInt("DG1_PPL", 0);
    uint32_t dg1_flow = preferences.getUInt("DG1_flow", 0);
    uint32_t dd1_ppl = preferences.getUInt("DD1_PPL", 0);
    uint32_t dd1_flow = preferences.getUInt("DD1_flow", 0);
    uint32_t dg2_ppl = preferences.getUInt("DG2_PPL", 0);
    uint32_t dg2_flow = preferences.getUInt("DG2_flow", 0);
    uint32_t dd2_ppl = preferences.getUInt("DD2_PPL", 0);
    uint32_t dd2_flow = preferences.getUInt("DD2_flow", 0);

    preferences.end();

    // ===== MIGRATION depuis "constantes" si valeurs absentes dans "calibration" =====
    if (calibA == 0.0 || pressureMaxBar == 0.0 || dg1_ppl == 0) {
        Serial.println("Migration des anciennes constantes vers calibration...");
        preferences.begin("constantes", false);

        // Migration P_A, P_B
        if (calibA == 0.0) {
            calibA = preferences.getFloat("P_A", 0.0);
            if (calibA == 0.0) {
                int constManA = preferences.getUInt("constManA", 0);
                if (constManA != 0) {
                    calibA = constManA / 100.0;
                    Serial.printf("Migration P_A: %d -> %.2f\r\n", constManA, calibA);
                } else {
                    calibA = 1.0;  // Valeur par défaut
                }
            }
        }
        if (calibB == 0.0) {
            calibB = preferences.getFloat("P_B", 0.0);
            if (calibB == 0.0) {
                int constManB = preferences.getUInt("constManB", 0);
                if (constManB != 0) {
                    calibB = constManB / 100.0;
                    Serial.printf("Migration P_B: %d -> %.2f\r\n", constManB, calibB);
                }
            }
        }

        // Migration paramètres pression (valeurs par défaut si absentes)
        if (pressureMaxBar == 0.0) pressureMaxBar = 16.0;
        if (pressureVoltageMin == 0.0) pressureVoltageMin = 0.5;
        if (pressureVoltageMax == 0.0) pressureVoltageMax = 4.5;

        // Migration débitmètres
        if (dg1_ppl == 0) {
            dg1_ppl = preferences.getUInt("DG1_PPL", 0);
            if (dg1_ppl == 0) {
                int constDeb1 = preferences.getUInt("constDeb1", 0);
                if (constDeb1 != 0) {
                    dg1_ppl = constDeb1;
                    Serial.printf("Migration DG1_PPL: %d\r\n", dg1_ppl);
                }
            }
        }
        if (dg1_flow == 0) dg1_flow = preferences.getUInt("DG1_flow", 0);

        if (dd1_ppl == 0) {
            dd1_ppl = preferences.getUInt("DD1_PPL", 0);
            if (dd1_ppl == 0) {
                int constDeb2 = preferences.getUInt("constDeb2", 0);
                if (constDeb2 != 0) {
                    dd1_ppl = constDeb2;
                    Serial.printf("Migration DD1_PPL: %d\r\n", dd1_ppl);
                }
            }
        }
        if (dd1_flow == 0) dd1_flow = preferences.getUInt("DD1_flow", 0);

        if (dg2_ppl == 0) dg2_ppl = preferences.getUInt("DG2_PPL", 0);
        if (dg2_flow == 0) dg2_flow = preferences.getUInt("DG2_flow", 0);
        if (dd2_ppl == 0) dd2_ppl = preferences.getUInt("DD2_PPL", 0);
        if (dd2_flow == 0) dd2_flow = preferences.getUInt("DD2_flow", 0);

        preferences.end();

        // Sauvegarder les valeurs migrées dans "calibration" pour unifier le stockage
        preferences.begin("calibration", false);
        if (calibA != 0.0) preferences.putFloat("P_A", calibA);
        if (calibB != 0.0) preferences.putFloat("P_B", calibB);
        if (pressureMaxBar != 0.0) preferences.putFloat("P_max", pressureMaxBar);
        if (pressureVoltageMin != 0.0) preferences.putFloat("P_Vmin", pressureVoltageMin);
        if (pressureVoltageMax != 0.0) preferences.putFloat("P_Vmax", pressureVoltageMax);
        if (dg1_ppl != 0) preferences.putUInt("DG1_PPL", dg1_ppl);
        if (dg1_flow != 0) preferences.putUInt("DG1_flow", dg1_flow);
        if (dd1_ppl != 0) preferences.putUInt("DD1_PPL", dd1_ppl);
        if (dd1_flow != 0) preferences.putUInt("DD1_flow", dd1_flow);
        if (dg2_ppl != 0) preferences.putUInt("DG2_PPL", dg2_ppl);
        if (dg2_flow != 0) preferences.putUInt("DG2_flow", dg2_flow);
        if (dd2_ppl != 0) preferences.putUInt("DD2_PPL", dd2_ppl);
        if (dd2_flow != 0) preferences.putUInt("DD2_flow", dd2_flow);
        preferences.end();
    }

    // ===== APPLICATION DES PARAMÈTRES =====
    correctionManometreA = (int)(calibA * 100);
    correctionManometreB = (int)(calibB * 100);

    if (dg1_ppl != 0) NbImpulsionsDebitmetre1 = dg1_ppl;
    if (dg1_flow != 0) maxFlow1 = dg1_flow;
    if (dd1_ppl != 0) NbImpulsionsDebitmetre2 = dd1_ppl;
    if (dd1_flow != 0) maxFlow2 = dd1_flow;
    if (dg2_ppl != 0) NbImpulsionsDebitmetre3 = dg2_ppl;
    if (dg2_flow != 0) maxFlow3 = dg2_flow;
    if (dd2_ppl != 0) NbImpulsionsDebitmetre4 = dd2_ppl;
    if (dd2_flow != 0) maxFlow4 = dd2_flow;

    Serial.print("Type capteur pression: ");
    Serial.println(pressureSensorType == 0 ? "Gravity analogique (0.5-4.5V, 0-16 bar)" : "4-20mA (0-16 bar)");

    //FreeRTOS 
    setupFreeRTOSTasks();
}

void loop() {
    // ============== Loop simplifié : gestion de la reconnexion BLE uniquement ==============
    // Toutes les autres tâches sont gérées par FreeRTOS (PCNT, Pression, GPS, BLE, Commandes)

    // Gestion de la reconnexion BLE
    if (!deviceConnected && oldDeviceConnected) {
        delay(500); // Délai avant de relancer la publicité
        pServer->startAdvertising();
        Serial.println("Relancement de la advertising BLE");
        oldDeviceConnected = deviceConnected;
    }
    // Connexion établie
    if (deviceConnected && !oldDeviceConnected) {
        Serial.println("Nouveau client BLE connecté");
        oldDeviceConnected = deviceConnected;
    }

    // Petit délai pour ne pas saturer le CPU
    delay(100);
}
  
// // Fonction pour calculer la médiane
// int calculerMedian() {
//     int valeursTriees[n];
//     memcpy(valeursTriees, mesures, sizeof(mesures));
//     std::sort(valeursTriees, valeursTriees + n);
//     return valeursTriees[n / 2];
// }

// // Fonction pour calculer la moyenne sans les outliers
// float calculerMoyenneSansOutliers() {
//     int mediane = calculerMedian();
//     int somme = 0;
//     int nombreDeValeurs = 0;
//     for (int i = 0; i < n; i++) {
//         if (abs(mesures[i] - mediane) <= 2) {
//             somme += mesures[i];
//             nombreDeValeurs++;
//         }
//     }
//     return somme / (float)nombreDeValeurs;
// }
