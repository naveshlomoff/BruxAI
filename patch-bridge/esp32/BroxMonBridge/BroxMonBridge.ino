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
 * Mic level: the relayed mic samples are decimated, and the raw 12-bit signal moves only tens to
 * hundreds of counts for speech (measured on real hardware), which no waveform chart shows well.
 * So each flush also carries micRms -- the RMS of every received mic sample (full 8 kHz, not
 * decimated) after removing each 91-sample packet's own mean, so ADC offset drift between packets
 * doesn't count as sound -- which the app turns into a level meter.
 *
 * Data path (after a crash loop on real hardware): notifications arrive on the NimBLE host task
 * while flushing runs on the Arduino loop task, so buffers are fixed-size arrays behind a
 * spinlock, JSON is written straight into one pre-reserved String, and nothing network-related
 * runs inside a BLE callback.
 *
 * Wi-Fi is self-service and multi-network, never hardcoded (a password in source would sit in this
 * public repo forever): the board remembers up to 5 networks (home, office, a phone hotspot) and
 * joins whichever is in range, so at a demo it only needs power -- see setupWiFi().
 *
 * Libraries (Arduino IDE > Tools > Manage Libraries): NimBLE-Arduino (h2zero), WiFiManager
 * (tzapu), WebSockets (Markus Sattler), ArduinoJson (Benoit Blanchon).
 * Board: ESP32 Dev Module, Partition Scheme "Huge APP (3MB No OTA/1MB SPIFFS)".
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <utility>
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

// Wi-Fi setup portal state (see setupWiFi()); here because commands and status use it.
static bool          portalActive    = false;
static volatile bool portalRequested = false; // "Add Wi-Fi network" pressed in the app

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

// Full-rate mic energy since the last flush (see the header note on micRms). Guarded by bufMux.
static double   micEnergySum   = 0;
static uint32_t micEnergyCount = 0;
static uint16_t micPeakDev     = 0;
static float    lastMicRms     = 0; // for the serial log only

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

  double packetEnergy = 0;
  uint16_t packetPeak = 0;
  if (isMic && count) {
    // Mean-remove per packet, then sum squares -- done before taking the lock (pure arithmetic).
    uint32_t sum = 0;
    for (size_t i = 0; i < count; i++) sum += (uint16_t)pData[i * 2] | ((uint16_t)pData[i * 2 + 1] << 8);
    float mean = (float)sum / count;
    for (size_t i = 0; i < count; i++) {
      float dev = ((uint16_t)pData[i * 2] | ((uint16_t)pData[i * 2 + 1] << 8)) - mean;
      packetEnergy += dev * dev;
      uint16_t absDev = (uint16_t)(dev < 0 ? -dev : dev);
      if (absDev > packetPeak) packetPeak = absDev;
    }
  }

  portENTER_CRITICAL(&bufMux);
  if (isMic && count) {
    micEnergySum += packetEnergy;
    micEnergyCount += count;
    if (packetPeak > micPeakDev) micPeakDev = packetPeak;
  }
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
  s += "\",\"wifiSetupOpen\":";
  s += portalActive ? "true" : "false";
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
  } else if (strcmp(action, "wifi-setup") == 0) {
    // "Add Wi-Fi network" in the app: open the portal without dropping the current network.
    // Only opens the portal -- credentials are typed on the board's own local network, never sent
    // over this public channel.
    Serial.println("[CMD] wifi-setup");
    portalRequested = true;
    return;
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
  double energy;
  uint32_t energyCount;
  uint16_t peak;
  portENTER_CRITICAL(&bufMux);
  micN = bufMic.len; accN = bufAcc.len; fsmN = bufFsm.len;
  memcpy(micOut, micStore, micN * sizeof(uint16_t));
  memcpy(accOut, accStore, accN * sizeof(uint16_t));
  memcpy(fsmOut, fsmStore, fsmN * sizeof(uint16_t));
  bufMic.len = bufAcc.len = bufFsm.len = 0;
  dropped = bufMic.dropped + bufAcc.dropped + bufFsm.dropped;
  bufMic.dropped = bufAcc.dropped = bufFsm.dropped = 0;
  energy = micEnergySum; energyCount = micEnergyCount; peak = micPeakDev;
  micEnergySum = 0; micEnergyCount = 0; micPeakDev = 0;
  portEXIT_CRITICAL(&bufMux);

  if (dropped) Serial.printf("[BLE] %u samples dropped (buffer full)\n", dropped);
  if (!micN && !accN && !fsmN) return;

  String s = broadcastStart("sample", (micN + accN + fsmN) * 6 + 64);
  s += '{';
  appendArray(s, "mic", micOut, micN);
  s += ',';
  appendArray(s, "acc", accOut, accN);
  s += ',';
  appendArray(s, "fsm", fsmOut, fsmN);
  if (energyCount) {
    lastMicRms = sqrt(energy / energyCount);
    char level[48];
    snprintf(level, sizeof(level), ",\"micRms\":%.1f,\"micPeak\":%u", lastMicRms, peak);
    s += level;
  }
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

// ── Wi-Fi: several remembered networks, joins whichever is in range ──
// The board travels (home, office, a phone hotspot at demos) and has to just work when plugged in.
// It keeps up to MAX_KNOWN_NETWORKS credentials in its own flash (never in this repo), newest first.
// On power-up, and whenever the link drops, it scans and joins the strongest known network in range.
// The "BroxMon-Setup" portal only opens when none is, and closes by itself once one appears (e.g. the
// phone hotspot gets switched on). "Add Wi-Fi network" in the app (or a short press on BOOT) opens it
// to ADD a network without dropping the current one; a network can be added even when it isn't in
// range (the office from home, or the phone's own hotspot -- a phone can't host a hotspot while it's
// on the portal). Holding BOOT at power-on forgets every network.
static const int           MAX_KNOWN_NETWORKS   = 5;
static const char*         SETUP_AP_NAME        = "BroxMon-Setup";
static const unsigned long WIFI_RETRY_MS        = 10000;  // offline: rescan for known networks
static const unsigned long PORTAL_IDLE_CLOSE_MS = 600000; // an on-demand portal nobody used

struct KnownNetwork { String ssid; String pass; };
static KnownNetwork  knownNetworks[MAX_KNOWN_NETWORKS];
static int           knownCount     = 0;

static WiFiManager   wm;
static bool          portalOnDemand = false; // opened to add a network while already online
static unsigned long portalOpenedAt = 0;
static volatile bool portalSaved    = false; // set by WiFiManager's save callback

void loadKnownNetworks() {
  Preferences prefs;
  prefs.begin("wifi", true);
  knownCount = std::max(0, std::min((int)prefs.getInt("n", 0), MAX_KNOWN_NETWORKS));
  for (int i = 0; i < knownCount; i++) {
    knownNetworks[i].ssid = prefs.getString(("s" + String(i)).c_str(), "");
    knownNetworks[i].pass = prefs.getString(("p" + String(i)).c_str(), "");
  }
  prefs.end();
}

void saveKnownNetworks() {
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.putInt("n", knownCount);
  for (int i = 0; i < knownCount; i++) {
    prefs.putString(("s" + String(i)).c_str(), knownNetworks[i].ssid);
    prefs.putString(("p" + String(i)).c_str(), knownNetworks[i].pass);
  }
  prefs.end();
}

// Newest first: re-adding a network (e.g. with a corrected password) replaces the old entry and moves
// it to the front; when the list is full the oldest one drops off.
void rememberNetwork(const String& ssid, const String& pass) {
  if (!ssid.length()) return;
  for (int i = 0; i < knownCount; i++) {
    if (knownNetworks[i].ssid != ssid) continue;
    for (int j = i; j < knownCount - 1; j++) knownNetworks[j] = knownNetworks[j + 1];
    knownCount--;
    break;
  }
  if (knownCount == MAX_KNOWN_NETWORKS) knownCount--;
  for (int i = knownCount; i > 0; i--) knownNetworks[i] = knownNetworks[i - 1];
  knownNetworks[0].ssid = ssid;
  knownNetworks[0].pass = pass;
  knownCount++;
  saveKnownNetworks();
  Serial.printf("[WiFi] remembered '%s' (%d known networks)\n", ssid.c_str(), knownCount);
}

// One scan, then each remembered network that's in range, strongest first. Blocks while trying (up to
// ~12 s per network); BLE notifications keep landing in their buffers meanwhile.
bool connectKnownNetwork() {
  if (!knownCount) return false;
  int found = WiFi.scanNetworks();
  int order[MAX_KNOWN_NETWORKS], rssi[MAX_KNOWN_NETWORKS], candidates = 0;
  for (int k = 0; k < knownCount && found > 0; k++) {
    int best = -1000;
    for (int i = 0; i < found; i++) {
      if (WiFi.SSID(i) == knownNetworks[k].ssid && WiFi.RSSI(i) > best) best = WiFi.RSSI(i);
    }
    if (best > -1000) { order[candidates] = k; rssi[candidates] = best; candidates++; }
  }
  WiFi.scanDelete();
  for (int i = 1; i < candidates; i++) {
    for (int j = i; j > 0 && rssi[j] > rssi[j - 1]; j--) {
      std::swap(rssi[j], rssi[j - 1]);
      std::swap(order[j], order[j - 1]);
    }
  }
  if (!candidates) {
    Serial.printf("[WiFi] none of the %d known networks is in range\n", knownCount);
    return false;
  }
  for (int c = 0; c < candidates; c++) {
    const KnownNetwork& net = knownNetworks[order[c]];
    Serial.printf("[WiFi] trying '%s' (rssi %d)\n", net.ssid.c_str(), rssi[c]);
    WiFi.begin(net.ssid.c_str(), net.pass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) delay(100);
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WiFi] connected to '%s', IP = %s\n", net.ssid.c_str(), WiFi.localIP().toString().c_str());
      return true;
    }
    WiFi.disconnect();
  }
  return false;
}

void startSetupPortal(bool onDemand) {
  if (portalActive) return;
  portalOnDemand = onDemand;
  portalOpenedAt = millis();
  portalSaved = false;
  Serial.printf("[WiFi] opening '%s' (%s)\n", SETUP_AP_NAME,
                onDemand ? "requested -- add a network" : "no known network in range");
  wm.startConfigPortal(SETUP_AP_NAME);
  portalActive = true;
  sendStatus();
}

void closeSetupPortal(const char* why) {
  if (!portalActive) return;
  if (wm.getConfigPortalActive()) wm.stopConfigPortal();
  portalActive = false;
  Serial.printf("[WiFi] '%s' closed (%s)\n", SETUP_AP_NAME, why);
  sendStatus();
}

// Called every loop(): runs the portal, remembers what was saved in it, and rejoins a known network
// whenever the link is down.
void maintainWiFi() {
  if (portalRequested) {
    portalRequested = false;
    startSetupPortal(true);
  }
  if (portalActive) {
    wm.process();
    if (portalSaved) {
      portalSaved = false;
      rememberNetwork(wm.getWiFiSSID(true), wm.getWiFiPass(true));
      closeSetupPortal("network saved");
    }
  }

  // Never scan while a phone is on the portal -- it would interrupt someone typing a password.
  bool someoneOnPortal = portalActive && WiFi.softAPgetStationNum() > 0;

  if (WiFi.status() == WL_CONNECTED) {
    if (portalActive && !someoneOnPortal) {
      if (!portalOnDemand) closeSetupPortal("joined a known network");
      else if (millis() - portalOpenedAt > PORTAL_IDLE_CLOSE_MS) closeSetupPortal("unused for 10 minutes");
    }
    return;
  }

  static unsigned long lastTry = 0;
  if (someoneOnPortal || (lastTry && millis() - lastTry < WIFI_RETRY_MS)) return;
  lastTry = millis();
  if (connectKnownNetwork()) {
    if (portalActive && !portalOnDemand) closeSetupPortal("joined a known network");
  } else if (!portalActive) {
    startSetupPortal(false);
  }
}

// A short press on BOOT (GPIO0) while running also opens the portal to add a network.
void checkBootButton() {
  static unsigned long pressedSince = 0;
  if (digitalRead(BOOT_BUTTON_PIN) != LOW) { pressedSince = 0; return; }
  if (!pressedSince) pressedSince = millis();
  else if (millis() - pressedSince > 800 && !portalActive) startSetupPortal(true);
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  // The driver only reports "failed"; log why, so a bad password (15 / 204 handshake timeout,
  // 202 auth fail) can be told apart from a missing network (201).
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    Serial.printf("[WiFi] link down, reason %u\n", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  wm.setConfigPortalBlocking(false); // the bridge keeps running (and keeps looking) while it's open
  wm.setConnectTimeout(15);
  wm.setBreakAfterConfig(true);      // remember a saved network even if it isn't in range right now
  wm.setSaveConfigCallback([]() { portalSaved = true; });

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  loadKnownNetworks();
  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    Serial.println("[WiFi] BOOT held at power-on -- forgetting all networks");
    knownCount = 0;
    saveKnownNetworks();
    wm.resetSettings();
    WiFi.mode(WIFI_STA);
  } else if (knownCount == 0) {
    // First boot after the single-network firmware: its one network lives in the driver's storage.
    String ssid = wm.getWiFiSSID(true);
    if (ssid.length()) rememberNetwork(ssid, wm.getWiFiPass(true));
  }
  Serial.printf("[WiFi] %d known network(s)\n", knownCount);
  if (!connectKnownNetwork()) startSetupPortal(false);
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
  checkBootButton();
  maintainWiFi();

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
    Serial.printf("[BLE] notifications last 5s: acc=%u mic=%u fsm=%u micRms=%.1f (free heap %u)\n",
                  notifAcc, notifMic, notifFsm, lastMicRms, ESP.getFreeHeap());
    notifAcc = notifMic = notifFsm = 0;
  }

  delay(5);
}
