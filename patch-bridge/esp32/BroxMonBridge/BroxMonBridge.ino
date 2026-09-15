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
 * Data path (v2.1, after a crash loop on real hardware): the mic streams at 8 kHz, ~88 BLE
 * notifications a second. Notifications arrive on the NimBLE host task while flushing runs on
 * the Arduino loop task, so the buffers are fixed-size arrays behind a spinlock (a std::vector
 * growing on one task while the other serializes it crashed the board seconds after connecting),
 * JSON is written straight into one pre-reserved String instead of a JSON document, the HTTPS
 * connection is kept alive between posts, and nothing network-related runs inside a BLE callback.
 *
 * Libraries required (Arduino IDE > Tools > Manage Libraries): NimBLE-Arduino (h2zero) and
 * WiFiManager (tzapu). WiFi/HTTPClient/WiFiClientSecure are built into the ESP32 Arduino core.
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <NimBLEDevice.h>

static const int BOOT_BUTTON_PIN = 0; // GPIO0 -- the BOOT button on every common ESP32 devkit

// Same project/key already hardcoded in BruxAI/index.html (SUPABASE_URL/SUPABASE_KEY) -- the
// publishable key is meant to be public client-side, protected by RLS; using it here matches
// the app's existing security posture, not a new exposure.
const char* SUPABASE_URL = "https://ukoswihzqpztfypqhdnc.supabase.co";
const char* SUPABASE_KEY = "sb_publishable_VTSRe2BT1ppguJKRhwQF3A_xr0rUtBt";
const char* CHANNEL_TOPIC = "patch-live";

static const unsigned long FLUSH_INTERVAL_MS = 500;
static const char*         PATCH_NAME_PREFIX = "BroxMon";
static const char*         PATCH_ADDR_PREFIX = "00:80:e1"; // ST's OUI -- see ScanCallbacks::onResult

// The mic is relayed at 8 kHz / MIC_DECIMATION. Plain subsampling keeps each second's standard
// deviation (what the app's per-second event detector uses) while cutting the upload 4x, which
// keeps the relay comfortably inside the ESP32's heap and Supabase's per-message limits.
static const uint32_t MIC_DECIMATION = 4;

// Caps hold ~2 s of data each, so a slow or failed post can never grow memory without bound.
static const size_t MIC_CAP = 4000;
static const size_t ACC_CAP = 1000;
static const size_t FSM_CAP = 1000;

// Confirmed against BroxMon_Firmware/.../App/custom_stm.c.
static const NimBLEUUID SERVICE_UUID("0000fe40-cc7a-482a-984a-7f2ed5b3e58f");
static const NimBLEUUID CHAR_ACC_UUID("0000fe41-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID CHAR_MIC_UUID("0000fe42-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID CHAR_FSM_UUID("0000fe43-0000-1000-8000-00805f9b34fb");

static NimBLEClient*           pClient         = nullptr;
static NimBLEAdvertisedDevice* targetDevice    = nullptr;
static volatile bool           doConnect       = false;
static volatile bool           patchConnected  = false;
static volatile bool           disconnectEvent = false; // set on the BLE task, handled in loop()
static bool                    scanning        = false;
static String                  patchDeviceName = "";

struct SampleBuf {
  uint16_t* data;
  size_t    cap;
  size_t    len;
  uint32_t  dropped;
};

static uint16_t micStore[MIC_CAP], accStore[ACC_CAP], fsmStore[FSM_CAP];
static uint16_t micOut[MIC_CAP],   accOut[ACC_CAP],   fsmOut[FSM_CAP];
static SampleBuf bufMic = { micStore, MIC_CAP, 0, 0 };
static SampleBuf bufAcc = { accStore, ACC_CAP, 0, 0 };
static SampleBuf bufFsm = { fsmStore, FSM_CAP, 0, 0 };
static portMUX_TYPE bufMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t micDecimationCounter = 0;

static WiFiClientSecure tlsClient;
static HTTPClient       http;
static unsigned long    lastFlush = 0;

void startScan();

// ── BLE notification handler (NimBLE host task): decode 182-byte / 91 x uint16-LE packets ──
void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  SampleBuf* buf = nullptr;
  bool isMic = false;
  if (pChar->getUUID().equals(CHAR_ACC_UUID)) buf = &bufAcc;
  else if (pChar->getUUID().equals(CHAR_MIC_UUID)) { buf = &bufMic; isMic = true; }
  else if (pChar->getUUID().equals(CHAR_FSM_UUID)) buf = &bufFsm;
  if (!buf) return;

  size_t count = length / 2;
  portENTER_CRITICAL(&bufMux);
  for (size_t i = 0; i < count; i++) {
    if (isMic && (micDecimationCounter++ % MIC_DECIMATION) != 0) continue;
    if (buf->len >= buf->cap) { buf->dropped++; continue; }
    buf->data[buf->len++] = (uint16_t)pData[i * 2] | ((uint16_t)pData[i * 2 + 1] << 8);
  }
  portEXIT_CRITICAL(&bufMux);
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    bool nameMatch = advertisedDevice->haveName() &&
                     advertisedDevice->getName().rfind(PATCH_NAME_PREFIX, 0) == 0;
    // The patch doesn't always put its name in the advertisement (seen on real hardware: BroxMon01
    // advertising as 00:80:e1:27:4e:14 with no name, invisible to every name-filtered scanner) --
    // fall back to its public address, which the firmware builds from ST's OUI (BleGetBdAddress()).
    bool addrMatch = advertisedDevice->getAddress().toString().rfind(PATCH_ADDR_PREFIX, 0) == 0;
    if (nameMatch || addrMatch) {
      Serial.printf("[BLE] found patch %s (%s)\n", advertisedDevice->getAddress().toString().c_str(),
                    nameMatch ? "by name" : "by address");
      NimBLEDevice::getScan()->stop();
      if (targetDevice) delete targetDevice;
      targetDevice = new NimBLEAdvertisedDevice(*advertisedDevice);
      patchDeviceName = nameMatch ? String(advertisedDevice->getName().c_str()) : String("BroxMon01");
      doConnect = true;
    }
  }
  void onScanEnd(const NimBLEScanResults& results, int reason) override {}
};

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* pclient, int reason) override {
    // Runs on the BLE host task -- only flag it; the HTTPS post and rescan happen in loop().
    patchConnected = false;
    disconnectEvent = true;
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

// ── Supabase Realtime Broadcast REST API ──
// POST {url}/realtime/v1/api/broadcast, header apikey: <key>,
// body {"messages":[{"topic":..., "event":..., "payload":{...}}]}. Confirmed on the live app page.
String broadcastBodyStart(const char* event, size_t payloadReserve) {
  String body;
  body.reserve(payloadReserve + 96);
  body += "{\"messages\":[{\"topic\":\"";
  body += CHANNEL_TOPIC;
  body += "\",\"event\":\"";
  body += event;
  body += "\",\"payload\":";
  return body;
}

bool postBroadcastBody(const char* event, const String& body) {
  if (WiFi.status() != WL_CONNECTED) return false;
  // Same HTTPClient + TLS client every time with reuse on, so posts ride one kept-alive
  // connection instead of paying a full TLS handshake (and its heap spike) twice a second.
  if (!http.begin(tlsClient, String(SUPABASE_URL) + "/realtime/v1/api/broadcast")) return false;
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();

  if (code < 200 || code >= 300) {
    Serial.printf("[Supabase] broadcast '%s' failed, HTTP %d (free heap %u)\n", event, code, ESP.getFreeHeap());
    return false;
  }
  return true;
}

void sendStatus(bool connected) {
  String body = broadcastBodyStart("status", 64);
  body += "{\"patchConnected\":";
  body += connected ? "true" : "false";
  if (connected) {
    body += ",\"deviceName\":\"";
    body += patchDeviceName;
    body += "\"";
  }
  body += "}}]}";
  postBroadcastBody("status", body);
}

static void appendArray(String& s, const char* key, const uint16_t* v, size_t n) {
  char num[8];
  s += '"';
  s += key;
  s += "\":[";
  for (size_t i = 0; i < n; i++) {
    if (i) s += ',';
    utoa(v[i], num, 10);
    s += num;
  }
  s += ']';
}

void flushBuffers() {
  // Take the samples under the lock (a memcpy -- microseconds), then do all slow work outside it.
  size_t micN, accN, fsmN;
  uint32_t dropped;
  portENTER_CRITICAL(&bufMux);
  micN = bufMic.len; accN = bufAcc.len; fsmN = bufFsm.len;
  memcpy(micOut, micStore, micN * sizeof(uint16_t));
  memcpy(accOut, accStore, accN * sizeof(uint16_t));
  memcpy(fsmOut, fsmStore, fsmN * sizeof(uint16_t));
  bufMic.len = bufAcc.len = bufFsm.len = 0;
  dropped = bufMic.dropped + bufAcc.dropped + bufFsm.dropped;
  bufMic.dropped = bufAcc.dropped = bufFsm.dropped = 0;
  portEXIT_CRITICAL(&bufMux);

  if (dropped) Serial.printf("[BLE] %u samples dropped (buffer full)\n", dropped);
  if (!micN && !accN && !fsmN) return;

  String body = broadcastBodyStart("sample", (micN + accN + fsmN) * 6 + 32);
  body += '{';
  appendArray(body, "mic", micOut, micN);
  body += ',';
  appendArray(body, "acc", accOut, accN);
  body += ',';
  appendArray(body, "fsm", fsmOut, fsmN);
  body += "}}]}";

  // A failed post loses that half-second of live data rather than retrying it -- this is a live
  // view, and retrying is what let memory grow until the board crashed.
  postBroadcastBody("sample", body);
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
  Serial.printf("[BLE] connected + subscribed (free heap %u)\n", ESP.getFreeHeap());
  sendStatus(true);
  return true;
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
  // Routers sometimes refuse the first association attempt ("Association refused too many times"
  // on real hardware) -- retry a few times before giving up on the saved network.
  wm.setConnectRetries(5);
  wm.setConnectTimeout(15);
  // Without a timeout a single refused attempt strands the device in the portal forever, even
  // though the saved network is fine -- time out, restart, and retry the saved network instead.
  wm.setConfigPortalTimeout(180);
  Serial.println("[WiFi] connecting (or opening the 'BroxMon-Setup' portal if no known network is in range)...");
  if (!wm.autoConnect("BroxMon-Setup")) {
    Serial.println("[WiFi] no network (portal timed out) -- restarting to retry the saved network");
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

  tlsClient.setInsecure(); // skip cert pinning: avoids a brittle hardcoded root CA that breaks on
                           // rotation, and this payload is non-sensitive sensor waveform data --
                           // same tradeoff many ESP32-to-cloud-API sketches make.
  http.setReuse(true);
  http.setTimeout(5000);

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

  if (disconnectEvent) {
    disconnectEvent = false;
    Serial.println("[BLE] patch disconnected");
    sendStatus(false);
    startScan();
  }

  if (doConnect) {
    doConnect = false;
    scanning = false;
    if (!connectToPatch()) {
      delay(1000);
      startScan();
    }
  }

  if (patchConnected && millis() - lastFlush >= FLUSH_INTERVAL_MS) {
    lastFlush = millis();
    flushBuffers();
  }

  delay(5);
}
