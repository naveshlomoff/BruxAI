/*
 * BroxMon Bridge — BLE-to-Supabase relay for the BroxMon patch.
 *
 * v2: relays through Supabase Realtime Broadcast instead of serving its own local web page.
 *
 * Why: the first version (local WebSocket + a page served by this device) worked, but only at a
 * URL separate from the real app (naveshlomoff.github.io/BruxAI) -- and it fundamentally could
 * only ever work that way. A page served over HTTPS (which GitHub Pages forces) can never open a
 * plain ws:// connection to a device on the local network -- every browser, Safari included,
 * blocks that as mixed content, and there is no way to get a real trusted TLS certificate for a
 * device on a private home network. Routing through Supabase (which BruxAI's own index.html
 * already loads supabase-js for) sidesteps this entirely: Supabase's realtime endpoint has a
 * normal, properly-trusted certificate, so the SAME naveshlomoff.github.io/BruxAI/ page can
 * subscribe to it directly, on any device, without ever touching the local network.
 *
 * Architecture:
 *   BroxMon01 (patch) --BLE (NimBLE central)--> ESP32 --HTTPS POST (Supabase Realtime Broadcast
 *   REST API)--> Supabase --wss (already-loaded supabase-js)--> naveshlomoff.github.io/BruxAI/
 *   Patch tab, from any device, on any network.
 *
 * This device never serves anything and never needs to be reachable from the browser at all --
 * it only ever makes outbound HTTPS requests, so it works from anywhere with Wi-Fi + internet,
 * not just the same LAN as the browser.
 *
 * Wi-Fi setup is self-service, not hardcoded: this device moves between locations (home, office
 * demos, ...), and a password baked into the source would also sit in this public repo's git
 * history forever. WiFiManager (see setupWiFi()) instead opens its own "BroxMon-Setup" network
 * with a captive portal the first time it can't reach a known one, and remembers whatever you
 * pick from then on -- hold the BOOT button for ~2s at power-on to forget it and pick again.
 *
 * Libraries required (Arduino IDE > Tools > Manage Libraries): NimBLE-Arduino (h2zero) and
 * WiFiManager (tzapu). WiFi/HTTPClient/WiFiClientSecure are built into the ESP32 Arduino core.
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>

static const int BOOT_BUTTON_PIN = 0; // GPIO0 -- the BOOT button on every common ESP32 devkit

// Same project/key already hardcoded in BruxAI/index.html (SUPABASE_URL/SUPABASE_KEY) -- the
// publishable key is meant to be public client-side, protected by RLS; using it here matches
// the app's existing security posture, not a new exposure.
const char* SUPABASE_URL = "https://ukoswihzqpztfypqhdnc.supabase.co";
const char* SUPABASE_KEY = "sb_publishable_VTSRe2BT1ppguJKRhwQF3A_xr0rUtBt";
const char* CHANNEL_TOPIC = "patch-live";

static const unsigned long FLUSH_INTERVAL_MS   = 1000; // batched broadcast rate -- see header note
static const char*         PATCH_NAME_PREFIX   = "BroxMon";

// Confirmed against BroxMon_Firmware/.../App/custom_stm.c.
static const NimBLEUUID SERVICE_UUID("0000fe40-cc7a-482a-984a-7f2ed5b3e58f");
static const NimBLEUUID CHAR_ACC_UUID("0000fe41-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID CHAR_MIC_UUID("0000fe42-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID CHAR_FSM_UUID("0000fe43-0000-1000-8000-00805f9b34fb");

static NimBLEClient*           pClient        = nullptr;
static NimBLEAdvertisedDevice* targetDevice   = nullptr;
static volatile bool           doConnect      = false;
static volatile bool           patchConnected = false;
static bool                    scanning       = false;
static String                  patchDeviceName = "";

// Buffers accumulate every sample between flushes (not just the most recent ones) so batching
// only adds latency, never drops data -- cleared after each successful POST.
static std::vector<uint16_t> bufMic, bufAcc, bufFsm;
static unsigned long lastFlush = 0;

void startScan();
bool postBroadcast(const char* event, const JsonDocument& payload);

// ── BLE notification handler: decode 182-byte / 91 x uint16-LE packets into the buffers ──
void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  std::vector<uint16_t>* buf = nullptr;
  if (pChar->getUUID().equals(CHAR_ACC_UUID)) buf = &bufAcc;
  else if (pChar->getUUID().equals(CHAR_MIC_UUID)) buf = &bufMic;
  else if (pChar->getUUID().equals(CHAR_FSM_UUID)) buf = &bufFsm;
  if (!buf) return;

  size_t count = length / 2;
  for (size_t i = 0; i < count; i++) {
    uint16_t v = (uint16_t)pData[i * 2] | ((uint16_t)pData[i * 2 + 1] << 8);
    buf->push_back(v);
  }
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
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
  void onScanEnd(const NimBLEScanResults& results, int reason) override {}
};

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* pclient, int reason) override {
    Serial.println("[BLE] patch disconnected");
    patchConnected = false;
    JsonDocument doc;
    doc["patchConnected"] = false;
    postBroadcast("status", doc);
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
  pScan->setScanCallbacks(&scanCallbacks, false);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->start(0, false, true); // duration 0 = scan indefinitely until a match stops it
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

  // Hub firmware fires a one-shot L2CAP connection-parameter-update ~1s after connecting
  // (BroxMon_Firmware's app_ble.c -- "critical for reliable 8kHz audio streaming"). Waiting past
  // that mark before touching services avoids a service-discovery race confirmed on real hardware.
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
  JsonDocument doc;
  doc["patchConnected"] = true;
  doc["deviceName"] = patchDeviceName;
  postBroadcast("status", doc);
  return true;
}

// ── Supabase Realtime Broadcast REST API (confirmed against Supabase's own docs) ──
// POST {url}/realtime/v1/api/broadcast/{topic}/events/{event}, header apikey: <key>, JSON body.
bool postBroadcast(const char* event, const JsonDocument& payload) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure(); // skip cert pinning: avoids a brittle hardcoded root CA that breaks on
                         // rotation, and this payload is non-sensitive sensor waveform data --
                         // same tradeoff many ESP32-to-cloud-API sketches make.
  HTTPClient http;
  String url = String(SUPABASE_URL) + "/realtime/v1/api/broadcast/" + CHANNEL_TOPIC + "/events/" + event;
  if (!http.begin(client, url)) return false;
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Content-Type", "application/json");

  String body;
  serializeJson(payload, body);
  int code = http.POST(body);
  http.end();

  if (code < 200 || code >= 300) {
    Serial.printf("[Supabase] broadcast '%s' failed, HTTP %d\n", event, code);
    return false;
  }
  return true;
}

void flushBuffers() {
  if (bufMic.empty() && bufAcc.empty() && bufFsm.empty()) return;

  JsonDocument doc;
  JsonArray mic = doc["mic"].to<JsonArray>();
  for (uint16_t v : bufMic) mic.add(v);
  JsonArray acc = doc["acc"].to<JsonArray>();
  for (uint16_t v : bufAcc) acc.add(v);
  JsonArray fsm = doc["fsm"].to<JsonArray>();
  for (uint16_t v : bufFsm) fsm.add(v);

  if (postBroadcast("sample", doc)) {
    bufMic.clear();
    bufAcc.clear();
    bufFsm.clear();
  }
  // On failure, buffers are deliberately left intact -- they'll be included (plus whatever
  // arrived meanwhile) in the next flush attempt, so a dropped request loses time, not data.
}

// Tries the last-saved network first (fast, silent if already known); only if that fails does it
// fall back to opening the "BroxMon-Setup" portal. Safe to call again on a dropped connection --
// a still-in-range known network reconnects quickly without ever showing the portal again.
void setupWiFi() {
  WiFiManager wm;
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    Serial.println("[WiFi] BOOT button held at startup -- forgetting saved network");
    wm.resetSettings();
  }
  Serial.println("[WiFi] connecting (or opening the 'BroxMon-Setup' portal if no known network is in range)...");
  if (!wm.autoConnect("BroxMon-Setup")) {
    Serial.println("[WiFi] setup portal closed without a network chosen -- restarting to try again");
    delay(2000);
    ESP.restart();
  }
  Serial.printf("[WiFi] connected, IP = %s\n", WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== BroxMon Bridge (Supabase relay) starting ===");

  setupWiFi();
  NimBLEDevice::init("");
  startScan();

  lastFlush = millis();
  Serial.println("=== Ready ===");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] connection lost, reconnecting...");
    setupWiFi();
  }

  if (doConnect) {
    doConnect = false;
    scanning = false;
    if (!connectToPatch()) {
      delay(1000);
      startScan();
    }
  }

  if (millis() - lastFlush >= FLUSH_INTERVAL_MS) {
    lastFlush = millis();
    flushBuffers();
  }

  delay(10);
}
