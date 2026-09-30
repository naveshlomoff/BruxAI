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
 * v4 (calibration): the relayed mic is plain subsampling -- aliased, so it can't answer "is this
 * sound in the bruxism band?". The bridge now measures the spectrum itself at the full 8 kHz and
 * sends 16 bands of 250 Hz every ~91 ms (see addPacketToFrame). Pages that ask for it (fmt 2 in their
 * connect/keepalive) also get payload v2: samples as base64 uint16 (less than half the bytes of
 * JSON numbers -- the WebSocket used to stall and drop about once a minute with all three sensors
 * streaming), a flush sequence number so a lost flush shows up as a gap, and bridge timestamps: the
 * accelerometer and pressure sensor arrive in 91-sample packets covering ~1-2 s each, so a sample's
 * time comes from its packet's arrival, not from when the flush reached the phone. Older pages keep
 * getting the v1 JSON arrays.
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

static const char* FIRMWARE_VERSION = "2026-09-30 link"; // in every status; the Patch tab shows it

static const unsigned long FLUSH_INTERVAL_MS        = 500;
static const unsigned long STATUS_INTERVAL_MS       = 3000;
static const unsigned long HEARTBEAT_INTERVAL_MS    = 25000; // Phoenix closes silent sockets
static const unsigned long APP_KEEPALIVE_TIMEOUT_MS = 90000; // app sends keepalive every 15 s
static const unsigned long V2_REQUEST_WINDOW_MS     = 60000; // payload v2 while a v2 page keeps asking

static const char* PATCH_NAME_PREFIX = "BroxMon";
static const char* PATCH_ADDR_PREFIX = "00:80:e1"; // ST's OUI -- see ScanCallbacks::onResult

// Connection parameters, set by the bridge from the start. ~1 s into every connection the patch asks
// for a 7.5-10 ms interval with a 5 s supervision timeout (BroxMon_Firmware's app_ble.c). The
// interval is what its 8 kHz audio needs and is used as is; the timeout goes to the BLE maximum: a
// radio shadow of over 5 s (lying with the head between patch and bridge) otherwise ends the
// connection, and the patch's firmware can hang for good while it handles a dropped link (27.09,
// 30.09). The patch's request is then declined, which also skips a connection-update procedure a
// weak link can lose.
static const uint16_t CONN_ITVL_MIN    = 6;    // x 1.25 ms = 7.5 ms
static const uint16_t CONN_ITVL_MAX    = 8;    // 10 ms
static const uint16_t CONN_SUPERVISION = 3200; // x 10 ms = 32 s
static const int8_t   BLE_TX_POWER_DBM = 9;    // the ESP32's maximum (default +3): a stronger downlink

// The mic is relayed at 8 kHz / MIC_DECIMATION. Plain subsampling keeps each second's standard
// deviation (what the app's per-second event detector uses) while cutting the upload 4x.
static const uint32_t MIC_DECIMATION = 4;

// Caps hold ~2 s of mic and ~10 s of acc/fsm, so a slow or failed send can never grow memory
// without bound.
static const size_t MIC_CAP = 4000;
static const size_t ACC_CAP = 1000;
static const size_t FSM_CAP = 1000;

// Sound bands. Each 91-sample mic packet is mean-removed, Hann-windowed, zero-padded to 128 and FFT'd
// (62.5 Hz bins); its power is summed into BAND_COUNT bands of 250 Hz, and FRAME_PACKETS packets
// (~91 ms of sound) make one frame. Packets are analysed one at a time because consecutive packets
// aren't guaranteed contiguous: the patch drops a packet whenever a BLE send fails. 16 narrow bands
// rather than the three analysis bands (<250 / 250-2000 / 2000-4000 Hz) so the edges can be retuned
// from calibration recordings without reflashing.
static const int    MIC_PACKET_SAMPLES = 91;  // the patch's packet size (mic_interface.h)
static const int    FFT_SIZE           = 128;
static const int    BAND_COUNT         = 16;  // 16 x 250 Hz = 0-4 kHz, the Nyquist range of 8 kHz
static const int    FRAME_PACKETS      = 8;
static const size_t FRAME_CAP          = 160; // ~14 s of frames: rides out a stalled send
static const size_t PKT_TIME_CAP       = 12;  // acc/fsm packets held per flush (a cap of 1000 = 10)

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
static unsigned long lastV2Request  = 0;      // last connect/keepalive from a page that reads v2

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
  uint32_t* times;    // acc/fsm: arrival millis() of each stored packet, one per 91 samples
  size_t    timesLen;
};

// One ~91 ms slice of sound, sent to the app as raw bytes (payload v2 "bf").
struct BandFrame {
  uint32_t t;                // bridge millis() when the frame's last packet arrived
  uint8_t  code[BAND_COUNT]; // band power in 0.5 dB steps, see encodeBandCode()
};
static_assert(sizeof(BandFrame) == 4 + BAND_COUNT, "BandFrame is sent as raw bytes");

static uint16_t micStore[MIC_CAP], accStore[ACC_CAP], fsmStore[FSM_CAP];
static uint16_t micOut[MIC_CAP],   accOut[ACC_CAP],   fsmOut[FSM_CAP];
static uint32_t accTimes[PKT_TIME_CAP],    fsmTimes[PKT_TIME_CAP];
static uint32_t accTimesOut[PKT_TIME_CAP], fsmTimesOut[PKT_TIME_CAP];
static SampleBuf bufMic = { micStore, MIC_CAP, 0, 0, nullptr, 0 };
static SampleBuf bufAcc = { accStore, ACC_CAP, 0, 0, accTimes, 0 };
static SampleBuf bufFsm = { fsmStore, FSM_CAP, 0, 0, fsmTimes, 0 };
static BandFrame frameStore[FRAME_CAP], frameOut[FRAME_CAP];
static size_t    frameLen      = 0;
static uint32_t  framesDropped = 0;
static uint32_t  micPackets    = 0; // mic packets since the last flush (the patch makes ~88/s)
static portMUX_TYPE bufMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t micDecimationCounter = 0;
static uint32_t flushSeq = 0;       // payload v2 "q": a missing number = a flush that never arrived

// Full-rate mic energy since the last flush (see the header note on micRms). Guarded by bufMux.
static double   micEnergySum   = 0;
static uint32_t micEnergyCount = 0;
static uint16_t micPeakDev     = 0;
static float    lastMicRms     = 0; // for the serial log only

// Raw notification counts per characteristic, logged every few seconds -- tells "the patch never
// sends this channel" apart from "the bridge drops it" without any debugger on the patch.
static volatile uint32_t notifAcc = 0, notifMic = 0, notifFsm = 0, framesMade = 0;

void sendStatus();

// ── Sound bands: FFT state, used only on the NimBLE host task (the notification callback) ──
static float   fftRe[FFT_SIZE], fftIm[FFT_SIZE];
static float   twiddleCos[FFT_SIZE / 2], twiddleSin[FFT_SIZE / 2];
static uint8_t bitReverse[FFT_SIZE];
static float   hannWindow[MIC_PACKET_SAMPLES];
static float   spectrumScale = 0;           // |X[k]|^2 -> that bin's share of the packet's variance
static float   bandPower[BAND_COUNT];       // summed over the current frame's packets
static int     framePackets  = 0;
static volatile bool spectrumReset = false; // set on (re)connect: don't mix in a stale partial frame

void setupSpectrum() {
  float windowPower = 0;
  for (int n = 0; n < MIC_PACKET_SAMPLES; n++) {
    hannWindow[n] = 0.5f - 0.5f * cosf(2.0f * PI * n / (MIC_PACKET_SAMPLES - 1));
    windowPower += hannWindow[n] * hannWindow[n];
  }
  // Parseval, corrected for the window: sum over all bins of |X[k]|^2 / (N_fft * sum(w^2)) is the
  // packet's variance in counts^2 -- so bands add up to (about) micRms^2.
  spectrumScale = 1.0f / (FFT_SIZE * windowPower);
  for (int k = 0; k < FFT_SIZE / 2; k++) {
    twiddleCos[k] = cosf(2.0f * PI * k / FFT_SIZE);
    twiddleSin[k] = -sinf(2.0f * PI * k / FFT_SIZE);
  }
  for (int i = 0; i < FFT_SIZE; i++) {
    int r = 0;
    for (int b = 0; (1 << b) < FFT_SIZE; b++) if (i & (1 << b)) r |= (FFT_SIZE >> 1) >> b;
    bitReverse[i] = r;
  }
}

// In-place iterative radix-2 FFT of fftRe/fftIm.
static void fft() {
  for (int i = 0; i < FFT_SIZE; i++) {
    int j = bitReverse[i];
    if (j > i) { std::swap(fftRe[i], fftRe[j]); std::swap(fftIm[i], fftIm[j]); }
  }
  for (int len = 2; len <= FFT_SIZE; len <<= 1) {
    int half = len >> 1, step = FFT_SIZE / len;
    for (int start = 0; start < FFT_SIZE; start += len) {
      for (int k = 0; k < half; k++) {
        float wr = twiddleCos[k * step], wi = twiddleSin[k * step];
        int a = start + k, b = a + half;
        float xr = fftRe[b] * wr - fftIm[b] * wi;
        float xi = fftRe[b] * wi + fftIm[b] * wr;
        fftRe[b] = fftRe[a] - xr; fftIm[b] = fftIm[a] - xi;
        fftRe[a] += xr;           fftIm[a] += xi;
      }
    }
  }
}

// Band power (counts^2) -> 0.5 dB steps, 0 = 0.01 counts^2: code = 20*log10(power) + 40, so
// power = 10^((code - 40) / 20). Measured: a quiet band ~40-60, a tap on the patch ~150.
static uint8_t encodeBandCode(float power) {
  if (power <= 0.01f) return 0;
  float code = 20.0f * log10f(power) + 40.0f;
  return code >= 254.5f ? 255 : (uint8_t)(code + 0.5f);
}

// Adds one mic packet to the current frame; returns true, with `frame` filled, when it completes one.
static bool addPacketToFrame(const uint16_t* samples, size_t count, float mean, uint32_t now, BandFrame& frame) {
  if (spectrumReset) {
    spectrumReset = false;
    framePackets = 0;
    for (int b = 0; b < BAND_COUNT; b++) bandPower[b] = 0;
  }
  if (count != (size_t)MIC_PACKET_SAMPLES) return false; // the window is sized for the patch's packets
  for (int n = 0; n < FFT_SIZE; n++) {
    fftRe[n] = n < MIC_PACKET_SAMPLES ? (samples[n] - mean) * hannWindow[n] : 0.0f;
    fftIm[n] = 0.0f;
  }
  fft();
  const int binsPerBand = FFT_SIZE / 2 / BAND_COUNT;
  for (int k = 1; k <= FFT_SIZE / 2; k++) { // bin 0 is the (removed) mean
    float power = (fftRe[k] * fftRe[k] + fftIm[k] * fftIm[k]) * spectrumScale;
    if (k < FFT_SIZE / 2) power *= 2.0f;    // one-sided: fold in the mirrored negative frequency
    bandPower[std::min(k / binsPerBand, BAND_COUNT - 1)] += power;
  }
  if (++framePackets < FRAME_PACKETS) return false;
  frame.t = now;
  for (int b = 0; b < BAND_COUNT; b++) {
    frame.code[b] = encodeBandCode(bandPower[b] / FRAME_PACKETS);
    bandPower[b] = 0;
  }
  framePackets = 0;
  return true;
}

// ── BLE notification handler (NimBLE host task): decode 182-byte / 91 x uint16-LE packets ──
void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  uint32_t now = millis();
  SampleBuf* buf = nullptr;
  bool isMic = false;
  if (pChar->getUUID().equals(CHAR_ACC_UUID)) { buf = &bufAcc; notifAcc++; }
  else if (pChar->getUUID().equals(CHAR_MIC_UUID)) { buf = &bufMic; isMic = true; notifMic++; }
  else if (pChar->getUUID().equals(CHAR_FSM_UUID)) { buf = &bufFsm; notifFsm++; }
  if (!buf) return;

  uint16_t samples[128];
  size_t count = std::min(length / 2, sizeof(samples) / sizeof(samples[0]));
  if (!count) return;
  for (size_t i = 0; i < count; i++) samples[i] = (uint16_t)pData[i * 2] | ((uint16_t)pData[i * 2 + 1] << 8);

  // Mic arithmetic happens before taking the lock: mean-removed energy/peak for micRms, and the
  // packet's spectrum for the band frames.
  double packetEnergy = 0;
  uint16_t packetPeak = 0;
  BandFrame frame;
  bool frameReady = false;
  if (isMic) {
    uint32_t sum = 0;
    for (size_t i = 0; i < count; i++) sum += samples[i];
    float mean = (float)sum / count;
    for (size_t i = 0; i < count; i++) {
      float dev = samples[i] - mean;
      packetEnergy += dev * dev;
      uint16_t absDev = (uint16_t)(dev < 0 ? -dev : dev);
      if (absDev > packetPeak) packetPeak = absDev;
    }
    frameReady = addPacketToFrame(samples, count, mean, now, frame);
    if (frameReady) framesMade++;
  }

  portENTER_CRITICAL(&bufMux);
  if (isMic) {
    micEnergySum += packetEnergy;
    micEnergyCount += count;
    if (packetPeak > micPeakDev) micPeakDev = packetPeak;
    micPackets++;
    for (size_t i = 0; i < count; i++) {
      if ((micDecimationCounter++ % MIC_DECIMATION) != 0) continue;
      if (buf->len >= buf->cap) { buf->dropped++; continue; }
      buf->data[buf->len++] = samples[i];
    }
    if (frameReady) {
      if (frameLen < FRAME_CAP) frameStore[frameLen++] = frame;
      else framesDropped++;
    }
  } else if (buf->len + count <= buf->cap && buf->timesLen < PKT_TIME_CAP) {
    // Whole packets only, so every 91 samples line up with one arrival time.
    memcpy(buf->data + buf->len, samples, count * sizeof(uint16_t));
    buf->len += count;
    buf->times[buf->timesLen++] = now;
  } else {
    buf->dropped += count;
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

static volatile int  lastDisconnectReason = 0;     // NimBLE: 0x200 + HCI reason (0x208 = supervision timeout)
static volatile bool connParamsDeclined   = false;

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* pclient, int reason) override {
    // Runs on the BLE host task -- only flag it; status and rescan happen in loop().
    patchConnected = false;
    lastDisconnectReason = reason;
    disconnectEvent = true;
  }
  bool onConnParamsUpdateRequest(NimBLEClient* pclient, const ble_gap_upd_params* params) override {
    connParamsDeclined = true;
    return false; // keep CONN_ITVL_* / CONN_SUPERVISION -- see there
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
  s += ",\"fw\":\"";
  s += FIRMWARE_VERSION;
  s += "\"";
  if (lastDisconnectReason) { // why the link last dropped, readable without the serial port
    s += ",\"disc\":";
    s += lastDisconnectReason;
  }
  if (patchConnected) {
    s += ",\"deviceName\":\"";
    s += patchDeviceName;
    s += "\"";
  }
  s += "}";
  broadcastFinish(s);
}

// fmt: the sample payload version the sending page reads (absent = 1, pages from before v2).
bool appWantsV2() {
  return lastV2Request && millis() - lastV2Request < V2_REQUEST_WINDOW_MS;
}

void handleCommand(const char* action, int fmt) {
  // v2 wins while any v2 page keeps asking, so an old page left open elsewhere can't flip the
  // format back and forth every keepalive.
  if (fmt >= 2 && (strcmp(action, "connect") == 0 || strcmp(action, "keepalive") == 0)) lastV2Request = millis();
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
  filter["payload"]["payload"]["fmt"] = true;

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
    handleCommand(doc["payload"]["payload"]["action"] | "", doc["payload"]["payload"]["fmt"] | 1);
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

static const char BASE64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void appendBase64(String& s, const uint8_t* data, size_t len) {
  char chunk[192];
  size_t used = 0;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = (uint32_t)data[i] << 16;
    if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
    if (i + 2 < len) v |= data[i + 2];
    chunk[used++] = BASE64_CHARS[(v >> 18) & 63];
    chunk[used++] = BASE64_CHARS[(v >> 12) & 63];
    chunk[used++] = i + 1 < len ? BASE64_CHARS[(v >> 6) & 63] : '=';
    chunk[used++] = i + 2 < len ? BASE64_CHARS[v & 63] : '=';
    if (used > sizeof(chunk) - 4) { s.concat(chunk, used); used = 0; }
  }
  if (used) s.concat(chunk, used);
}

// ,"key":"<base64>" -- the ESP32 is little-endian, so uint16/uint32 arrays go out as LE bytes.
static void appendBase64Field(String& s, const char* key, const void* data, size_t bytes) {
  s += ",\"";
  s += key;
  s += "\":\"";
  appendBase64(s, (const uint8_t*)data, bytes);
  s += '"';
}

static void appendTimes(String& s, const char* key, const uint32_t* t, size_t n) {
  s += ",\"";
  s += key;
  s += "\":[";
  for (size_t i = 0; i < n; i++) {
    if (i) s += ',';
    s += t[i];
  }
  s += ']';
}

// Payload v2: {"v":2, "q": flush sequence, "t": bridge millis() at this flush, "mp": mic packets
// received since the last flush, "d": samples dropped on the bridge (only when nonzero),
// "mic"/"acc"/"fsm": base64 uint16 LE, "at"/"ft": arrival millis() of each 91-sample acc/fsm packet,
// "bf": base64 band frames (20 bytes each: uint32 LE millis + BAND_COUNT codes), micRms, micPeak}.
void flushBuffers() {
  // Take the samples under the lock (a memcpy -- microseconds), then do all slow work outside it.
  size_t micN, accN, fsmN, accTN, fsmTN, frameN;
  uint32_t dropped, framesLost, packets;
  double energy;
  uint32_t energyCount;
  uint16_t peak;
  portENTER_CRITICAL(&bufMux);
  micN = bufMic.len; accN = bufAcc.len; fsmN = bufFsm.len;
  memcpy(micOut, micStore, micN * sizeof(uint16_t));
  memcpy(accOut, accStore, accN * sizeof(uint16_t));
  memcpy(fsmOut, fsmStore, fsmN * sizeof(uint16_t));
  accTN = bufAcc.timesLen; fsmTN = bufFsm.timesLen;
  memcpy(accTimesOut, accTimes, accTN * sizeof(uint32_t));
  memcpy(fsmTimesOut, fsmTimes, fsmTN * sizeof(uint32_t));
  frameN = frameLen;
  memcpy(frameOut, frameStore, frameN * sizeof(BandFrame));
  bufMic.len = bufAcc.len = bufFsm.len = 0;
  bufAcc.timesLen = bufFsm.timesLen = 0;
  frameLen = 0;
  dropped = bufMic.dropped + bufAcc.dropped + bufFsm.dropped;
  bufMic.dropped = bufAcc.dropped = bufFsm.dropped = 0;
  framesLost = framesDropped; framesDropped = 0;
  packets = micPackets; micPackets = 0;
  energy = micEnergySum; energyCount = micEnergyCount; peak = micPeakDev;
  micEnergySum = 0; micEnergyCount = 0; micPeakDev = 0;
  portEXIT_CRITICAL(&bufMux);

  if (dropped) Serial.printf("[BLE] %u samples dropped (buffer full)\n", dropped);
  if (framesLost) Serial.printf("[BLE] %u band frames dropped (buffer full)\n", framesLost);
  if (!micN && !accN && !fsmN) return;
  flushSeq++; // counted even if the send fails below, so the app sees the gap

  String s;
  if (appWantsV2()) {
    size_t bytes = (micN + accN + fsmN) * 2 + frameN * sizeof(BandFrame);
    s = broadcastStart("sample", bytes * 4 / 3 + (accTN + fsmTN) * 11 + 160);
    s += "{\"v\":2,\"q\":";
    s += flushSeq;
    s += ",\"t\":";
    s += (uint32_t)millis();
    s += ",\"mp\":";
    s += packets;
    if (dropped) {
      s += ",\"d\":";
      s += dropped;
    }
    appendBase64Field(s, "mic", micOut, micN * sizeof(uint16_t));
    appendBase64Field(s, "acc", accOut, accN * sizeof(uint16_t));
    appendBase64Field(s, "fsm", fsmOut, fsmN * sizeof(uint16_t));
    appendTimes(s, "at", accTimesOut, accTN);
    appendTimes(s, "ft", fsmTimesOut, fsmTN);
    appendBase64Field(s, "bf", frameOut, frameN * sizeof(BandFrame));
  } else {
    s = broadcastStart("sample", (micN + accN + fsmN) * 6 + 64);
    s += '{';
    appendArray(s, "mic", micOut, micN);
    s += ',';
    appendArray(s, "acc", accOut, accN);
    s += ',';
    appendArray(s, "fsm", fsmOut, fsmN);
  }
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
    pClient->setConnectionParams(CONN_ITVL_MIN, CONN_ITVL_MAX, 0, CONN_SUPERVISION);
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

  spectrumReset = true;
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
static volatile uint32_t staDisconnects = 0;  // counted by the event handler in setupWiFi()
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
    unsigned long start = millis(), lastBegin = start;
    uint32_t seenDisconnects = staDisconnects;
    int begins = 1;
    while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
      delay(100);
      // The home router refuses the first association after a reboot ("Association refused too
      // many times", reason 208) and the driver then stops trying -- ask again instead of sitting
      // out the window and falling back to the setup portal (seen: ~25 s to get online).
      if (staDisconnects != seenDisconnects && begins < 3 && millis() - lastBegin > 1500) {
        Serial.println("[WiFi] refused -- asking again");
        WiFi.disconnect();
        delay(200);
        seenDisconnects = staDisconnects;
        WiFi.begin(net.ssid.c_str(), net.pass.c_str());
        lastBegin = millis();
        begins++;
      }
    }
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
    staDisconnects++;
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
  Serial.printf("\n=== BroxMon Bridge (connect on demand, fw %s) starting ===\n", FIRMWARE_VERSION);

  setupSpectrum();
  setupWiFi();

  // No fingerprint/CA -> the library calls setInsecure(): avoids a brittle pinned root CA that
  // breaks on rotation; the payload is non-sensitive sensor waveform data. Empty protocol string
  // omits the Sec-WebSocket-Protocol header, which Supabase doesn't use.
  String path = String("/realtime/v1/websocket?apikey=") + SUPABASE_KEY + "&vsn=1.0.0";
  ws.beginSSL(SUPABASE_HOST, 443, path.c_str(), "", "");
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(3000);

  NimBLEDevice::init("");
  NimBLEDevice::setPower(BLE_TX_POWER_DBM);

  Serial.println("=== Ready -- waiting for Connect in the app ===");
}

void loop() {
  checkBootButton();
  maintainWiFi();

  ws.loop();

  if (disconnectEvent) {
    disconnectEvent = false;
    disconnecting = false;
    Serial.printf("[BLE] patch disconnected (reason 0x%x)\n", lastDisconnectReason);
    sendStatus();
  }
  if (connParamsDeclined) {
    connParamsDeclined = false;
    Serial.println("[BLE] declined the patch's connection-parameter request (keeping 7.5-10 ms, 32 s)");
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
    Serial.printf("[BLE] notifications last 5s: acc=%u mic=%u fsm=%u frames=%u micRms=%.1f payload v%d (free heap %u)\n",
                  notifAcc, notifMic, notifFsm, framesMade, lastMicRms, appWantsV2() ? 2 : 1, ESP.getFreeHeap());
    notifAcc = notifMic = notifFsm = framesMade = 0;
  }

  delay(5);
}
