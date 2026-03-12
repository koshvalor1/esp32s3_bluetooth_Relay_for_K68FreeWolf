#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "esp_wifi.h"

// --- CONFIGURAÇÕES WIFI ---
const char* ssid = "TIM_ULTRAFIBRA_4A48_2G";
const char* password = "BcSk@9D4A48";

// --- DNA DO TECLADO (Os primeiros 10 dígitos) ---
String macPrefix = "db:d6:ff:79:47"; 

static BLEAddress *pServerAddress = nullptr;
static BLERemoteCharacteristic* pRemoteCharacteristic;
USBHIDKeyboard Keyboard;
WebServer server(80);

bool dispositivoConectado = false;
bool capsAtivo = false;
bool encontrouTeclado = false;
unsigned long ultimaTentativaReconexao = 0;

// HTML do Injetor
String htmlPage = "<html><meta charset='UTF-8'><style>body{font-family:sans-serif;padding:20px;background:#f4f4f4;}textarea{width:100%;border-radius:8px;padding:10px;}</style><body>"
                  "<h1>Bocó Injector V1</h1><form action='/inject' method='POST'>"
                  "<textarea name='code' rows='12' placeholder='Cole o código aqui...'></textarea><br><br>"
                  "<input type='submit' value='Injetar no PC'></form></body></html>";

// --- FUNÇÃO AUXILIAR: MANDA A TECLA FÍSICA ---
void mandarTecla(uint8_t tecla, uint8_t mod = 0) {
    KeyReport report;
    memset(&report, 0, sizeof(report));
    report.modifiers = mod;
    report.keys[0] = tecla;
    Keyboard.sendReport(&report);
    delay(25); Keyboard.releaseAll(); delay(15);
}

// --- TRADUTOR DE TEXTO PARA INJEÇÃO (WEB -> PC) ---
void typeTextABNT2(String text) {
  for (int i = 0; i < text.length(); i++) {
    uint8_t c = (uint8_t)text[i];
    if (c == 0xC3) { 
      i++; uint8_t c2 = (uint8_t)text[i];
      if (c2 == 0xA1) { mandarTecla(0x2F); delay(50); mandarTecla(0x04); }      // á
      else if (c2 == 0xA3) { mandarTecla(0x34, 0x02); delay(50); mandarTecla(0x04); } // ã
      else if (c2 == 0xA7) { mandarTecla(0x33); } // ç
      continue;
    }
    if (c >= 'a' && c <= 'z') { mandarTecla(0x04 + (c - 'a')); }
    else if (c >= 'A' && c <= 'Z') { mandarTecla(0x04 + (c - 'A'), 0x02); }
    else if (c >= '0' && c <= '9') { mandarTecla(c == '0' ? 0x27 : 0x1E + (c - '1')); }
    else if (c == ' ') { mandarTecla(0x2C); }
    else if (c == '\n') { mandarTecla(0x28); }
    else if (c == '.') { mandarTecla(0x37); }
    else if (c == ',') { mandarTecla(0x36); }
    else if (c == '?') { mandarTecla(0x1A, 0x40); }
  }
}

// --- ENGINE DE REMAPEAMENTO DO K68 ---
void processarERemaplear(uint8_t* pData, size_t length) {
    if (length < 3) return;
    uint8_t modifier = pData[0];
    uint8_t key = pData[2];
    if (key == 0 && modifier == 0) { Keyboard.releaseAll(); return; }

    if (key == 0x39) { capsAtivo = !capsAtivo; }
    if (capsAtivo) {
        if (key >= 0x1E && key <= 0x26) { key = (key - 0x1E) + 0x3A; } 
        else if (key == 0x27) { key = 0x43; } 
        else if (key == 0x2D) { key = 0x44; } 
        else if (key == 0x2E) { key = 0x45; } 
    }

    if ((modifier & 0x44)) {
        if (key == 0x15) { modifier = 0x02; key = 0x35; }      
        else if (key == 0x17) { modifier = 0x00; key = 0x35; } 
        else if (key == 0x2C) { modifier = 0x02; key = 0x2D; } 
        else if (key == 0x33 || key == 0x38) { modifier = 0x40; key = 0x1A; } 
    }
    else if (key == 0x4B) { key = 0x4D; } 
    else if (key == 0x4D) { key = 0x4B; } 

    KeyReport report;
    memset(&report, 0, sizeof(report));
    report.modifiers = modifier;
    report.keys[0] = key; 
    Keyboard.sendReport(&report);
}

// --- CALLBACK DE DADOS (AGORA NO LUGAR CERTO) ---
void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    processarERemaplear(pData, length);
}

// --- CLASSE DO SCANNER PERDIGUEIRO ---
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        String encontrado = advertisedDevice.getAddress().toString().c_str();
        if (encontrado.startsWith("db:d6:ff:79:47")) {
            Serial.print(">>> DNA IDENTIFICADO: ");
            Serial.println(encontrado);
            advertisedDevice.getScan()->stop();
            if(pServerAddress) delete pServerAddress;
            pServerAddress = new BLEAddress(advertisedDevice.getAddress());
            encontrouTeclado = true;
        }
    }
};

// --- FUNÇÃO DE CONEXÃO ---
bool connectToServer() {
    if (pServerAddress == nullptr) return false;
    BLEClient* pClient = BLEDevice::createClient();
    if (!pClient->connect(*pServerAddress, BLE_ADDR_RANDOM)) return false;
    
    BLERemoteService* pSer = pClient->getService(BLEUUID((uint16_t)0x1812));
    if (pSer == nullptr) { pClient->disconnect(); return false; }
    
    pRemoteCharacteristic = pSer->getCharacteristic(BLEUUID((uint16_t)0x2A4D));
    if (pRemoteCharacteristic != nullptr && pRemoteCharacteristic->canNotify()) {
        pRemoteCharacteristic->registerForNotify(notifyCallback);
        dispositivoConectado = true;
        return true;
    }
    pClient->disconnect();
    return false;
}

void setup() {
    USB.begin();
    Keyboard.begin();
    delay(2000); 

    Serial.begin(115200);
    BLEDevice::init("ESP32_S3_Bridge");
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    WiFi.begin(ssid, password);
    server.on("/", []() { server.send(200, "text/html", htmlPage); });
    server.on("/inject", HTTP_POST, []() {
        if (server.hasArg("code")) {
            server.send(200, "text/html", "<h3>Injetando...</h3>");
            typeTextABNT2(server.arg("code"));
        }
    });
    server.begin();
    Serial.println("Bocó Perdigueiro Pronto.");
}

void loop() {
    server.handleClient();
    if (!dispositivoConectado) {
        if (millis() - ultimaTentativaReconexao > 4000) {
            ultimaTentativaReconexao = millis();
            Serial.println("Farejando DNA...");
            BLEScan* pBLEScan = BLEDevice::getScan();
            pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
            pBLEScan->setActiveScan(false);
            pBLEScan->start(2, false);

            if (encontrouTeclado) {
                if (connectToServer()) {
                    Serial.println(">>> REAGIU! CONECTADO.");
                    encontrouTeclado = false;
                }
            }
        }
    }
    delay(10);
}
