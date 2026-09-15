/*
 * BroxMon Bridge — BLE-to-Supabase relay for the BroxMon patch.
 *
 * Why a bridge at all: iOS has no Web Bluetooth, and a page served over HTTPS (GitHub Pages) can
 * never talk to a device on the local network (mixed content). So this ESP32 is the only
 * Bluetooth central: it talks to the patch directly and relays everything through Supabase
 * Realtime, which the SAME naveshlomoff.github.io/BruxAI/ page already subscribes to -- from any
 * device, on any network.
 *
 * Architecture:
 *   BroxMon01 (patch) --BLE (NimBLE central)--> ESP32 --wss (Supabase Realtime, Phoenix
 *   protocol)--> Supabase --wss (supabase-js)--> BruxAI Patch tab, on any device.
 *
 * v3: connect on demand. The bridge no longer grabs the patch at boot. It joins the Realtime
 * channel, announces itself every few seconds, and connects to the patch only when the app sends
 * a "connect" command (the Patch tab's Connect button). "disconnect" -- or 90 s without the app's
 * keepalive, e.g. the page was closed -- first turns the patch's mic off (its firmware never stops
 * mic sampling on a plain disconnect, which drains the battery) and then drops the link. One
 * WebSocket carries both directions, replacing the earlier per-flush HTTPS posts.
 *
 * Data path (after a crash loop on real hardware): notifications arrive on the NimBLE host task
 * while flushing runs on the Arduino loop task, so buffers are fixed-size arrays behind a
 * spinlock, JSON is written straight into one pre-reserved String, and nothing network-related
 * runs inside a BLE callback.
 *
 * Wi-Fi setup is self-service, not hardcoded (the board moves between home and office, and a
 * password in source would sit in this public repo forever): WiFiManager opens a "BroxMon-Setup"
 * captive portal when no known network is reachable -- hold BOOT ~2 s at power-on to pick again.
 *
 * Libraries (Arduino IDE > Tools > Manage Libraries): NimBLE-Arduino (h2zero), WiFiManager
 * (tzapu), WebSockets (Markus Sattler), ArduinoJson (Benoit Blanchon).
 * Board: ESP32 Dev Module, Partition Scheme "Huge APP (3MB No OTA/1MB SPIFFS)".
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

static const int BOOT_BUTTON_PIN = 0; // GPIO0 -- the BOOT button on every common ESP32 devkit

// Same project/key already hardcoded in BruxAI/index.html -- the publishable key is meant to be
// public client-side; using it here matches the app's existing security posture.
const char* SUPABASE_HOST = "ukoswihzqpztfypqhdnc.supabase.co";
const char* SUPABASE_KEY  = "sb_publishable_VTSRe2BT1ppguJKRhwQF3A_xr0rUtBt";
const char* CHANNEL_TOPIC = "realtime:patch-live"; // supabase.channel('patch-live') in index.html
const char* JOIN_REF      = "1";

static const unsigned long FLUSH_INTERVAL_MS        = 500;
static const unsigned long STATUS_INTERVAL_MS       = 3000;
static const unsigned long HEARTBEAT_INTERVAL_MS    = 25000; // Phoenix closes silent sockets
static const unsigned long APP_KEEPALIVE_TIMEOUT_MS = 90000; // app sends keepalive every 15 s

static const char* PATCH_NAME_PREFIX = "BroxMon";
static const char* PATCH_ADDR_PREFIX = "00:80:e1"; // ST's OUI -- see ScanCallbacks::onResult

// The mic is relayed at 8 kHz / MIC_DECIMATION. Plain subsampling keeps each second's standard
// deviation (what the app's per-second event detector uses) while cutting the upload 4x.
static const uint32_t MIC_DECIMATION = 4;

// Caps hold ~2 s of data each, so a slow or failed send can never grow memory without bound.
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
static bool                    disconnecting   = false;
static String                  patchDeviceName = "";

// What the app asked for. Commands arrive over the WebSocket and are applied in loop().
static bool          wantConnected  = false;
static unsigned long lastAppContact = 0;
static const char*   lastCommand    = "none"; // echoed in status so pages can tell who disconnected

static WebSocketsClient ws;
static bool             wsJoined = false;
static uint32_t         wsRef    = 1;

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

// Raw notification counts per characteristic, logged every few seconds -- tells "the patch never
// sends this channel" apart from "the bridge drops it" without any debugger on the patch.
static volatile uint32_t notifAcc = 0, notifMic = 0, notifFsm = 0;

void sendStatus();

// ── BLE notification handler (NimBLE host task): decode 182-byte / 91 x uint16-LE packets ──
void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  SampleBuf* buf = nullptr;
  bool isMic = false;
  if (pChar->getUUID().equals(CHAR_ACC_UUID)) { buf = &bufAcc; notifAcc++; }
  else if (pChar->getUUID().equals(CHAR_MIC_UUID)) { buf = &bufMic; isMic = true; notifMic++; }
  else if (pChar->getUUID().equals(CHAR_FSM_UUID)) { buf = &bufFsm; notifFsm++; }
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
    // Runs on the BLE host task -- only flag it; status and rescan happen in loop().
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

void stopScan() {
  if (!scanning) return;
  NimBLEDevice::getScan()->stop();
  scanning = false;
  Serial.println("[BLE] scan stopped");
}

// ── Supabase Realtime over WebSocket (Phoenix protocol, vsn 1.0.0) ──
// Join:      {"topic":"realtime:patch-live","event":"phx_join","payload":{"config":{...}},"ref":"1","join_ref":"1"}
// Broadcast: {"topic":...,"event":"broadcast","payload":{"type":"broadcast","event":E,"payload":{...}},"ref":N,"join_ref":"1"}
// Verified end to end (join, send, receive, REST-to-socket) from Node before porting here.
String nextRef() {
  wsRef++;
  if (wsRef < 2) wsRef = 2; // "1" is reserved for the join reply
  return String(wsRef);
}

String broadcastStart(const char* event, size_t payloadReserve) {
  String s;
  s.reserve(payloadReserve + 160);
  s += "{\"topic\":\"";
  s += CHANNEL_TOPIC;
  s += "\",\"event\":\"broadcast\",\"payload\":{\"type\":\"broadcast\",\"event\":\"";
  s += event;
  s += "\",\"payload\":";
  return s;
}

bool broadcastFinish(String& s) {
  s += "},\"ref\":\"";
  s += nextRef();
  s += "\",\"join_ref\":\"";
  s += JOIN_REF;
  s += "\"}";
  if (!wsJoined) return false;
  return ws.sendTXT(s);
}

const char* bridgeState() {
  if (patchConnected) return "connected";
  return wantConnected ? "connecting" : "idle";
}

void sendStatus() {
  String s = broadcastStart("status", 96);
  s += "{\"state\":\"";
  s += bridgeState();
  s += "\",\"patchConnected\":";
  s += patchConnected ? "true" : "false";
  s += ",\"lastCommand\":\"";
  s += lastCommand;
  s += "\"";
  if (patchConnected) {
    s += ",\"deviceName\":\"";
    s += patchDeviceName;
    s += "\"";
  }
  s += "}";
  broadcastFinish(s);
}

void handleCommand(const char* action) {
  if (strcmp(action, "connect") == 0) {
    lastAppContact = millis();
    lastCommand = "connect";
    if (!wantConnected) Serial.println("[CMD] connect");
    wantConnected = true;
  } else if (strcmp(action, "keepalive") == 0) {
    // Only extends a connection someone asked for -- never revives one another page ended.
    if (wantConnected) lastAppContact = millis();
  } else if (strcmp(action, "disconnect") == 0) {
    lastCommand = "disconnect";
    if (wantConnected) Serial.println("[CMD] disconnect");
    wantConnected = false;
  } else {
    return;
  }
  sendStatus();
}

void handleWsText(const uint8_t* payload, size_t length) {
  // Only commands and small protocol replies reach us (broadcast "self" is off), but filter anyway
  // so an unexpected large message can't blow the heap.
  JsonDocument filter;
  filter["event"] = true;
  filter["ref"] = true;
  filter["payload"]["status"] = true;
  filter["payload"]["event"] = true;
  filter["payload"]["payload"]["action"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, (const char*)payload, length, DeserializationOption::Filter(filter))) return;

  const char* event = doc["event"] | "";
  if (strcmp(event, "phx_reply") == 0) {
    if (!wsJoined && strcmp(doc["ref"] | "", JOIN_REF) == 0) {
      wsJoined = strcmp(doc["payload"]["status"] | "", "ok") == 0;
      Serial.printf("[WS] channel join %s\n", wsJoined ? "ok" : "refused");
      if (wsJoined) sendStatus();
    }
    return;
  }
  if (strcmp(event, "broadcast") == 0 && strcmp(doc["payload"]["event"] | "", "command") == 0) {
    handleCommand(doc["payload"]["payload"]["action"] | "");
  }
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      Serial.println("[WS] connected, joining channel");
      wsJoined = false;
      String join = String("{\"topic\":\"") + CHANNEL_TOPIC +
                    "\",\"event\":\"phx_join\",\"payload\":{\"config\":{\"broadcast\":{\"ack\":false,\"self\":false}," +
                    "\"presence\":{\"key\":\"\"},\"postgres_changes\":[],\"private\":false}},\"ref\":\"" + JOIN_REF +
                    "\",\"join_ref\":\"" + JOIN_REF + "\"}";
      ws.sendTXT(join);
      break;
    }
    case WStype_DISCONNECTED:
      if (wsJoined) Serial.println("[WS] disconnected -- reconnecting");
      wsJoined = false;
      break;
    case WStype_TEXT:
      handleWsText(payload, length);
      break;
    default:
      break;
  }
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

  String s = broadcastStart("sample", (micN + accN + fsmN) * 6 + 32);
  s += '{';
  appendArray(s, "mic", micOut, micN);
  s += ',';
  appendArray(s, "acc", accOut, accN);
  s += ',';
  appendArray(s, "fsm", fsmOut, fsmN);
  s += '}';
  // A failed send loses that half-second of live data rather than retrying it -- this is a live
  // view, and retrying is what once let memory grow until the board crashed.
  broadcastFinish(s);
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
  // (BroxMon_Firmware's app_ble.c). Waiting past that mark before touching services avoids a
  // service-discovery race confirmed on real hardware.
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
  sendStatus();
  return true;
}

void disconnectPatch() {
  if (disconnecting || !pClient) return;
  disconnecting = true;
  Serial.println("[BLE] disconnecting -- mic off first");
  // Writing 0 to the mic's CCCD is the only thing that runs Mic_Stop() on the patch; its mic ADC
  // otherwise keeps sampling after the link drops, draining the battery until it's power-cycled.
  NimBLERemoteService* svc = pClient->getService(SERVICE_UUID);
  NimBLERemoteCharacteristic* mic = svc ? svc->getCharacteristic(CHAR_MIC_UUID) : nullptr;
  if (mic) mic->unsubscribe();
  pClient->disconnect(); // onDisconnect -> disconnectEvent finishes the job in loop()
}

// Brings the BLE side in line with what the app asked for.
void applyDesiredState() {
  if (wantConnected && millis() - lastAppContact > APP_KEEPALIVE_TIMEOUT_MS) {
    Serial.println("[CMD] no keepalive from the app for 90 s -- disconnecting");
    wantConnected = false;
    lastCommand = "timeout";
    sendStatus();
  }

  if (wantConnected) {
    if (!patchConnected && !doConnect) startScan();
  } else {
    stopScan();
    doConnect = false;
    if (patchConnected) disconnectPatch();
  }
}

// Tries the last-saved network first (fast, silent if already known); only if that fails does it
// fall back to opening the "BroxMon-Setup" portal.
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
  Serial.println("\n=== BroxMon Bridge (connect on demand) starting ===");

  setupWiFi();

  // No fingerprint/CA -> the library calls setInsecure(): avoids a brittle pinned root CA that
  // breaks on rotation; the payload is non-sensitive sensor waveform data. Empty protocol string
  // omits the Sec-WebSocket-Protocol header, which Supabase doesn't use.
  String path = String("/realtime/v1/websocket?apikey=") + SUPABASE_KEY + "&vsn=1.0.0";
  ws.beginSSL(SUPABASE_HOST, 443, path.c_str(), "", "");
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(3000);

  NimBLEDevice::init("");

  Serial.println("=== Ready -- waiting for Connect in the app ===");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] connection lost, reconnecting...");
    setupWiFi();
  }

  ws.loop();

  if (disconnectEvent) {
    disconnectEvent = false;
    disconnecting = false;
    Serial.println("[BLE] patch disconnected");
    sendStatus();
  }

  if (doConnect) {
    doConnect = false;
    scanning = false;
    if (wantConnected && !connectToPatch()) delay(1000); // applyDesiredState rescans
  }

  applyDesiredState();

  static unsigned long lastHeartbeat = 0;
  if (ws.isConnected() && millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = millis();
    String hb = String("{\"topic\":\"phoenix\",\"event\":\"heartbeat\",\"payload\":{},\"ref\":\"") + nextRef() + "\"}";
    ws.sendTXT(hb);
  }

  // Broadcasts aren't stored, so a page opened later only learns the bridge's state from the next
  // status -- re-announce it (idle included, so the app can tell "bridge offline" from "idle").
  static unsigned long lastStatus = 0;
  if (wsJoined && millis() - lastStatus >= STATUS_INTERVAL_MS) {
    lastStatus = millis();
    sendStatus();
  }

  static unsigned long lastFlush = 0;
  if (patchConnected && millis() - lastFlush >= FLUSH_INTERVAL_MS) {
    lastFlush = millis();
    flushBuffers();
  }

  static unsigned long lastNotifLog = 0;
  if (patchConnected && millis() - lastNotifLog >= 5000) {
    lastNotifLog = millis();
    Serial.printf("[BLE] notifications last 5s: acc=%u mic=%u fsm=%u (free heap %u)\n",
                  notifAcc, notifMic, notifFsm, ESP.getFreeHeap());
    notifAcc = notifMic = notifFsm = 0;
  }

  delay(5);
}
