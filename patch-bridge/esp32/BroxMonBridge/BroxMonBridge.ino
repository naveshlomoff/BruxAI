/*
 * BroxMon Bridge — BLE-to-WiFi relay for the BroxMon patch.
 *
 * Problem this solves: Web Bluetooth cannot run in any iOS browser (Apple/WebKit restriction),
 * and on this project's Windows laptop the direct browser<->patch BLE connection has proven
 * unstable at the GATT layer even from Windows' own native Bluetooth settings (not a browser or
 * app-code issue - see BruxAI's git history for the debugging trail). This sketch moves the BLE
 * connection onto dedicated hardware: the ESP32 is the only thing that ever speaks Bluetooth to
 * the patch. Every other device (iPhone, Android, laptop, whatever) talks to the ESP32 over plain
 * Wi-Fi/HTTP/WebSocket, which every browser on every platform already fully supports.
 *
 * Architecture:
 *   BroxMon01 (patch) --BLE (NimBLE central)--> ESP32 --WiFi--> browser at http://broxmon.local/
 *   The browser loads /page.html (served from this device's LittleFS) which opens a WebSocket to
 *   /ws and renders the exact same live charts / CSV export / session detection UI as BruxAI's
 *   own Patch tab (ported as-is from index.html — that part was already working correctly).
 *
 * Libraries required (install via Arduino IDE > Tools > Manage Libraries):
 *   - NimBLE-Arduino (h2zero)              — BLE central role, much lighter than the stock BLEDevice
 *   - ESP Async WebServer (ESP32Async fork) — HTTP + WebSocket server
 *   - Async TCP (ESP32Async fork)           — dependency of the above
 *   - ArduinoJson (Benoit Blanchon)         — building the WebSocket JSON messages
 *
 * Board: any ESP32 dev board (ESP32-WROOM32 etc.). In Arduino IDE, also install the "Arduino
 * LittleFS Upload" tool so the data/page.html file in this sketch folder gets flashed alongside
 * the sketch — see ../../README.md for the full step-by-step.
 */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// ── USER CONFIG: edit these two lines before flashing ──────────────────────────────────────
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
// If station Wi-Fi above fails (wrong password, out of range, or left as the placeholder),
// the bridge still starts its own access point below so it's never unreachable.
const char* AP_SSID     = "BroxMon-Bridge";
const char* AP_PASSWORD = "brux12345"; // WPA2 requires 8+ characters
// ─────────────────────────────────────────────────────────────────────────────────────────

static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
static const char*         PATCH_NAME_PREFIX        = "BroxMon";

// Same UUIDs confirmed against BroxMon_Firmware/.../App/custom_stm.c and already proven correct
// against the real hardware from BruxAI's own (direct Web Bluetooth) connection attempts.
static const NimBLEUUID SERVICE_UUID("0000fe40-cc7a-482a-984a-7f2ed5b3e58f");
static const NimBLEUUID CHAR_ACC_UUID("0000fe41-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID CHAR_MIC_UUID("0000fe42-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID CHAR_FSM_UUID("0000fe43-0000-1000-8000-00805f9b34fb");

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

static NimBLEClient*            pClient      = nullptr;
static NimBLEAdvertisedDevice*  targetDevice = nullptr;
static volatile bool            doConnect    = false;
static volatile bool            patchConnected = false;
static bool                     scanning     = false;
static String                   patchDeviceName = "";

void startScan();
void broadcastStatus();

// ── BLE notification handler: decode 182-byte / 91 x uint16-LE packets and forward as JSON ──
void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  const char* channel = nullptr;
  if (pChar->getUUID().equals(CHAR_ACC_UUID)) channel = "acc";
  else if (pChar->getUUID().equals(CHAR_MIC_UUID)) channel = "mic";
  else if (pChar->getUUID().equals(CHAR_FSM_UUID)) channel = "fsm";
  if (!channel || ws.count() == 0) return;

  size_t count = length / 2;
  DynamicJsonDocument doc(4096);
  doc["type"] = "sample";
  doc["channel"] = channel;
  JsonArray arr = doc.createNestedArray("values");
  for (size_t i = 0; i < count; i++) {
    uint16_t v = (uint16_t)pData[i * 2] | ((uint16_t)pData[i * 2 + 1] << 8);
    arr.add(v);
  }
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

// ── Scan: match on advertised name, same reasoning as the web app fix — the hub's ADV packet
// carries a different, unrelated 16-bit UUID (0xAA01), never the real 128-bit GATT service UUID,
// so filtering by service UUID at scan time doesn't work; filtering by name does. ──
class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* advertisedDevice) override {
    if (advertisedDevice->haveName() &&
        advertisedDevice->getName().rfind(PATCH_NAME_PREFIX, 0) == 0) {
      Serial.printf("[BLE] found %s\n", advertisedDevice->getName().c_str());
      NimBLEDevice::getScan()->stop();
      if (targetDevice) delete targetDevice;
      targetDevice = new NimBLEAdvertisedDevice(*advertisedDevice);
      patchDeviceName = String(advertisedDevice->getName().c_str());
      doConnect = true;
    }
  }
};

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* pclient) override {
    Serial.println("[BLE] patch disconnected");
    patchConnected = false;
    broadcastStatus();
    startScan();
  }
};

static ScanCallbacks   scanCallbacks;
static ClientCallbacks clientCallbacks;

void startScan() {
  if (scanning || patchConnected) return;
  scanning = true;
  Serial.println("[BLE] scanning for BroxMon...");
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(&scanCallbacks, false);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->start(0, nullptr, false); // scan indefinitely until a match stops it
}

bool connectToPatch() {
  Serial.printf("[BLE] connecting to %s ...\n", targetDevice->getAddress().toString().c_str());

  if (pClient == nullptr) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(&clientCallbacks, false);
  }

  if (!pClient->connect(targetDevice)) {
    Serial.println("[BLE] connect() failed");
    return false;
  }

  // Mirror the fix already confirmed on the web-app side (BruxAI/index.html, patchConnectGatt):
  // the hub's firmware fires a one-shot L2CAP connection-parameter-update request ~1s after
  // connecting (BroxMon_Firmware's app_ble.c — "critical for reliable 8kHz audio streaming").
  // Waiting past that mark before touching services avoids the race.
  delay(2000);

  NimBLERemoteService* pService = pClient->getService(SERVICE_UUID);
  if (!pService) {
    Serial.println("[BLE] service not found");
    pClient->disconnect();
    return false;
  }

  const NimBLEUUID* charUuids[3] = { &CHAR_ACC_UUID, &CHAR_MIC_UUID, &CHAR_FSM_UUID };
  bool anySubscribed = false;
  for (int i = 0; i < 3; i++) {
    NimBLERemoteCharacteristic* pChar = pService->getCharacteristic(*charUuids[i]);
    if (pChar && pChar->canNotify()) {
      pChar->subscribe(true, notifyCallback);
      anySubscribed = true;
    } else {
      Serial.printf("[BLE] characteristic %d missing or no-notify\n", i);
    }
  }

  if (!anySubscribed) {
    pClient->disconnect();
    return false;
  }

  patchConnected = true;
  Serial.println("[BLE] connected + subscribed");
  broadcastStatus();
  return true;
}

// ── WebSocket: broadcast status to all connected browsers, accept "reconnect" commands ──
void broadcastStatus() {
  StaticJsonDocument<192> doc;
  doc["type"] = "status";
  doc["patchConnected"] = patchConnected;
  doc["deviceName"] = patchDeviceName;
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type,
               void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    broadcastStatus();
  } else if (type == WS_EVT_DATA) {
    String msg;
    for (size_t i = 0; i < len; i++) msg += (char)data[i];
    if (msg == "reconnect") {
      Serial.println("[WS] rescan requested");
      if (pClient && pClient->isConnected()) pClient->disconnect();
      patchConnected = false;
      startScan();
    }
  }
}

// ── Wi-Fi: station with an always-on AP fallback, so the bridge is never unreachable ──
void setupWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WiFi] connecting to %s", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] connected, IP = %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WiFi] station connect failed — check WIFI_SSID/WIFI_PASSWORD at the top of this file");
  }

  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("[WiFi] AP '%s' up, IP = %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  if (MDNS.begin("broxmon")) {
    Serial.println("[mDNS] http://broxmon.local/");
  } else {
    Serial.println("[mDNS] failed to start — use the IP address printed above instead");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== BroxMon Bridge starting ===");

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount failed — did you run 'ESP32 LittleFS Data Upload' with data/page.html present?");
  }

  setupWiFi();

  NimBLEDevice::init("");

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/page.html", "text/html");
  });
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.begin();

  startScan();
  Serial.println("=== Ready ===");
}

void loop() {
  ws.cleanupClients();

  if (doConnect) {
    doConnect = false;
    scanning = false;
    if (!connectToPatch()) {
      delay(1000);
      startScan();
    }
  }

  delay(10);
}
