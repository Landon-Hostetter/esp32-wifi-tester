// ESP32 WiFi Tester - Step 7: Channel & Congestion screen, rescan
//
// Navigation tree:
//   MENU
//   └─ WiFi Scanner -> NETWORKS (names as broadcast, "(n)" marker if names collide)
//        └─ [network] -> DETAILS (full name, BSSID, channel, signal)
//             └─ OPTIONS
//                  ├─ Live Signal   (dBm, grade, bar graph - updates continuously)
//                  ├─ Channel & Congestion (channel, loss, congestion, device list)
//                  ├─ Latency
//                  └─ Activity      (Latency and Activity are placeholders for now)
//
// Controls:
//   UP / DOWN         : move the highlight (wraps around)
//   SELECT (short)    : open the highlighted item / continue
//   SELECT (hold 1 s) : go back one level
//
// The network list comes from a real 2.4 GHz WiFi scan, strongest signal first.
// A new scan starts each time you open WiFi Scanner from the menu.
//
// Layout for the two-color OLED: rows 0-15 are yellow (header only),
// rows 16-63 are blue (lists and graphs).

#include <Wire.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------- Hardware ----------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS  0x3C
#define PIN_SDA       21
#define PIN_SCL       22

const uint8_t PIN_UP     = 25;
const uint8_t PIN_DOWN   = 26;
const uint8_t PIN_SELECT = 27;

#define DEBOUNCE_MS   30
#define LONG_PRESS_MS 1000

// ---------- Types ----------
// Arduino inserts function prototypes above the first function in the file,
// so every type used in a function signature must be defined up here.
enum ButtonEvent { EVT_NONE, EVT_UP, EVT_DOWN, EVT_SELECT, EVT_BACK };

typedef const char* (*LabelFn)(int);   // returns the text for list row i

enum ScanKind { SCAN_NONE, SCAN_LIST, SCAN_LIVE };   // which WiFi scan is running

enum Screen {
  SCREEN_MENU,
  SCREEN_NETWORKS,
  SCREEN_DETAILS,
  SCREEN_OPTIONS,
  SCREEN_LIVE,
  SCREEN_CHANNEL,
  SCREEN_LATENCY,
  SCREEN_ACTIVITY
};

struct ListState {
  int cursor;   // highlighted item
  int scroll;   // first visible item
};

struct Net {
  char    ssid[33];   // exactly as broadcast (empty for a hidden network)
  int32_t rssi;
  uint8_t channel;
  uint8_t bssid[6];   // unique hardware address: how networks are told apart internally
};

struct Bucket {          // one second of sniffed traffic
  uint32_t airtimeUs;    // estimated time the frames occupied the air
  uint16_t dataFrames;   // data frames of the chosen network
  uint16_t retryFrames;  // ...of which were retransmissions
};

// ---------- Display and layout ----------
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

const int BLUE_TOP     = 16;   // first blue row
const int ROW_H        = 12;
const int VISIBLE_ROWS = 4;    // 4 x 12 = 48 rows of blue
const int LIST_W       = 124;  // highlight width (leaves room for the scrollbar)

// ---------- Menus ----------
Screen currentScreen = SCREEN_MENU;

// Main menu: add future features here (keep the two arrays in step)
const int MENU_COUNT = 1;
const char* MENU_ITEMS[MENU_COUNT]    = {"WiFi Scanner"};
const Screen MENU_TARGETS[MENU_COUNT] = {SCREEN_NETWORKS};

// Options shown after the details screen
const int OPTION_COUNT = 4;
const char* OPTION_ITEMS[OPTION_COUNT]    = {"Live Signal", "Channel & Congestion", "Latency", "Activity"};
const Screen OPTION_TARGETS[OPTION_COUNT] = {SCREEN_LIVE, SCREEN_CHANNEL, SCREEN_LATENCY, SCREEN_ACTIVITY};

ListState menuState = {0, 0};
ListState netState  = {0, 0};
ListState optState  = {0, 0};

// ---------- Networks ----------
const int MAX_NETS = 40;   // the 40 strongest networks are kept

Net  nets[MAX_NETS];
int  netCount = 0;
ScanKind scanKind     = SCAN_NONE;   // which scan is running right now
bool     wantListScan = false;       // a list scan was asked for and starts when the radio is free

char netLabels[MAX_NETS][24];   // list text: name, plus " (n)" if the name appears more than once
int  netOrder[MAX_NETS];        // this network's position among same-name networks (1, 2, ...)
int  netTotal[MAX_NETS];        // how many networks share this name

Net  selectedNet;
char selectedLabel[24] = "";
int  selectedOrder = 1;
int  selectedTotal = 1;

float liveRssi  = -100;   // smoothed signal shown on the Live Signal screen
int   missCount = 0;      // consecutive live scans that did not see the chosen network

// ---------- Channel & Congestion (sniffer) state ----------
const int      SNIFF_WINDOW_S    = 5;                    // seconds of traffic behind each reading
const int      SNIFF_RING        = SNIFF_WINDOW_S + 1;   // + the second currently being filled
const uint32_t BUCKET_MS         = 1000;
const int      MIN_LOSS_FRAMES   = 10;                   // fewer data frames than this: loss shows "--"
const int      MAX_DEVICES       = 24;
const float    CONG_LIGHT_MAX    = 30.0f;                // airtime below this % = Light
const float    CONG_MODERATE_MAX = 60.0f;                // below this = Moderate, otherwise Heavy

portMUX_TYPE sniffMux = portMUX_INITIALIZER_UNLOCKED;    // guards data shared with the WiFi task
bool   sniffing = false;
Bucket buckets[SNIFF_RING];
int    curBucket     = 0;
int    bucketsFilled = 0;          // completed seconds available (max SNIFF_WINDOW_S)
unsigned long bucketStartMs = 0;

uint8_t devices[MAX_DEVICES][6];   // MAC addresses seen on the chosen network
int     deviceCount = 0;

uint8_t   devSnap[MAX_DEVICES][6]; // copy used for drawing
int       devSnapCount = 0;
ListState devState = {0, 0};

float airtimePct = 0;              // channel busy time, percent
float lossPct    = 0;              // retransmission rate on the chosen network, percent
bool  lossKnown  = false;

// Text to show for a network's name. The name is used exactly as broadcast;
// characters the OLED font can't draw (emoji, accents) show as '?'.
// A hidden network broadcasts an empty name, so it is shown as "(hidden)".
void displayName(const Net &n, char *out, size_t outSize) {
  if (n.ssid[0] == '\0') {
    snprintf(out, outSize, "(hidden)");
    return;
  }
  size_t i = 0;
  for (; i < outSize - 1 && n.ssid[i] != '\0'; i++) {
    unsigned char c = (unsigned char)n.ssid[i];
    out[i] = (c >= 32 && c <= 126) ? (char)c : '?';
  }
  out[i] = '\0';
}

// Same name more than once -> "Name (1)", "Name (2)" ...; otherwise just the name.
void buildNetLabels() {
  char name[33];
  for (int i = 0; i < netCount; i++) {
    int total = 0;
    int order = 0;
    for (int j = 0; j < netCount; j++) {
      if (strcmp(nets[j].ssid, nets[i].ssid) == 0) {
        total++;
        if (j <= i) order++;
      }
    }
    netTotal[i] = total;
    netOrder[i] = order;

    displayName(nets[i], name, sizeof(name));
    if (total > 1) snprintf(netLabels[i], sizeof(netLabels[i]), "%.15s (%d)", name, order);
    else           snprintf(netLabels[i], sizeof(netLabels[i]), "%.20s", name);
  }
}

// ---------- Scanning ----------
// Only one scan runs at a time. pumpScan() (called from loop) collects the
// finished scan, then starts whichever is wanted next: the full list scan
// (when you open WiFi Scanner) or, while Live Signal is open, repeated scans
// of just the chosen network's channel.

const uint32_t LIVE_DWELL_MS   = 200;    // time spent listening on the channel per live scan
const float    SMOOTH_ALPHA    = 0.5f;   // 1.0 = raw readings, lower = smoother
const int      LIVE_MISS_LIMIT = 4;      // scans in a row without a sighting before "No signal"

bool listScanBusy() { return wantListScan || scanKind == SCAN_LIST; }

// Called when the list is opened: clears the list and asks for a fresh scan
void startScan() {
  netCount = 0;
  netState.cursor = 0;
  netState.scroll = 0;
  wantListScan = true;
}

// Called when Live Signal is opened
void resetLive() {
  liveRssi  = selectedNet.rssi;   // start from the value seen in the list
  missCount = 0;
}

void beginListScan() {
  wantListScan = false;
  WiFi.scanDelete();               // clear old results
  WiFi.scanNetworks(true, true);   // async, include hidden networks
  scanKind = SCAN_LIST;
}

void beginLiveScan() {
  WiFi.scanDelete();
  // async, include hidden, active scan, LIVE_DWELL_MS on the channel, only the chosen channel
  WiFi.scanNetworks(true, true, false, LIVE_DWELL_MS, selectedNet.channel);
  scanKind = SCAN_LIVE;
}

// Keeps the strongest MAX_NETS networks, sorted strongest first.
void processListScan(int n) {
  netCount = 0;

  for (int i = 0; i < n; i++) {        // n is negative if the scan failed, so this is skipped
    int rssi = WiFi.RSSI(i);
    if (netCount == MAX_NETS && rssi <= nets[MAX_NETS - 1].rssi) continue;

    // Find this network's place in the sorted list (strongest first)
    int pos = (netCount < MAX_NETS) ? netCount : MAX_NETS - 1;
    while (pos > 0 && nets[pos - 1].rssi < rssi) {
      nets[pos] = nets[pos - 1];
      pos--;
    }

    String name = WiFi.SSID(i);        // empty for a hidden network
    strncpy(nets[pos].ssid, name.c_str(), sizeof(nets[pos].ssid) - 1);
    nets[pos].ssid[sizeof(nets[pos].ssid) - 1] = '\0';
    nets[pos].rssi    = rssi;
    nets[pos].channel = WiFi.channel(i);
    memcpy(nets[pos].bssid, WiFi.BSSID(i), 6);
    if (netCount < MAX_NETS) netCount++;
  }

  WiFi.scanDelete();
  buildNetLabels();

  Serial.print("Scan done: ");
  Serial.print(netCount);
  Serial.println(" networks");
  for (int i = 0; i < netCount; i++) {
    Serial.print(nets[i].ssid);
    Serial.print("  ");
    Serial.print(nets[i].rssi);
    Serial.print(" dBm  ch ");
    Serial.println(nets[i].channel);
  }
}

// Looks for the chosen access point (matched by BSSID) in a channel scan
void processLiveScan(int n) {
  bool found = false;

  for (int i = 0; i < n; i++) {        // n is negative if the scan failed, so this is skipped
    if (memcmp(WiFi.BSSID(i), selectedNet.bssid, 6) == 0) {
      float r = WiFi.RSSI(i);
      if (missCount >= LIVE_MISS_LIMIT) liveRssi = r;             // was lost: jump straight to the new reading
      else liveRssi += SMOOTH_ALPHA * (r - liveRssi);             // otherwise smooth
      missCount = 0;
      found = true;
      break;
    }
  }

  if (!found && missCount < 100) missCount++;
  WiFi.scanDelete();
}

// ---------- Sniffer (Channel & Congestion) ----------
// While the screen is open the radio listens on the chosen network's channel.
// Only frame headers are used (never the contents):
//   airtime  - estimated busy time of ALL traffic on the channel
//   loss     - share of the chosen network's data frames flagged as retries
//   devices  - MAC addresses exchanging data with the chosen network

// Estimated time a frame occupied the air, in microseconds.
uint32_t frameAirtimeUs(uint8_t sigMode, uint8_t rateCode, uint8_t mcs, uint8_t cwb, uint8_t sgi, uint32_t lenBytes) {
  uint32_t rate10    = 60;   // data rate in units of 0.1 Mbps
  uint32_t preambleUs = 20;  // OFDM preamble + signal field

  if (sigMode == 0) {        // legacy 802.11b/g
    switch (rateCode) {
      case 0x00: rate10 = 10;  preambleUs = 192; break;   // 1 Mbps, long preamble
      case 0x01: rate10 = 20;  preambleUs = 192; break;   // 2 Mbps
      case 0x02: rate10 = 55;  preambleUs = 192; break;   // 5.5 Mbps
      case 0x03: rate10 = 110; preambleUs = 192; break;   // 11 Mbps
      case 0x05: rate10 = 20;  preambleUs = 96;  break;   // short preamble versions
      case 0x06: rate10 = 55;  preambleUs = 96;  break;
      case 0x07: rate10 = 110; preambleUs = 96;  break;
      case 0x08: rate10 = 480; break;                     // 48 Mbps
      case 0x09: rate10 = 240; break;                     // 24 Mbps
      case 0x0A: rate10 = 120; break;                     // 12 Mbps
      case 0x0B: rate10 = 60;  break;                     // 6 Mbps
      case 0x0C: rate10 = 540; break;                     // 54 Mbps
      case 0x0D: rate10 = 360; break;                     // 36 Mbps
      case 0x0E: rate10 = 180; break;                     // 18 Mbps
      case 0x0F: rate10 = 90;  break;                     // 9 Mbps
      default: break;
    }
  } else {                   // 802.11n (HT)
    static const uint16_t HT20[8] = {65, 130, 195, 260, 390, 520, 585, 650};
    static const uint16_t HT40[8] = {135, 270, 405, 540, 810, 1080, 1215, 1350};
    uint32_t base    = cwb ? HT40[mcs & 7] : HT20[mcs & 7];
    uint32_t streams = 1 + ((mcs >> 3) & 3);
    rate10 = base * streams;
    if (sgi) rate10 = rate10 * 10 / 9;
    preambleUs = 36;
  }
  return preambleUs + (lenBytes * 8 + 22) * 10 / rate10;
}

// Remember a device (caller holds the lock)
void addDevice(const uint8_t *mac) {
  for (int i = 0; i < deviceCount; i++) {
    if (memcmp(devices[i], mac, 6) == 0) return;
  }
  if (deviceCount < MAX_DEVICES) {
    memcpy(devices[deviceCount], mac, 6);
    deviceCount++;
  }
}

// Handles one received frame (caller holds the lock). f = start of the 802.11 header.
void handleFrame(const uint8_t *f, uint32_t sigLen, uint32_t airtimeUs) {
  buckets[curBucket].airtimeUs += airtimeUs;      // every frame heard on the channel counts

  if (sigLen < 28) return;                        // too short to hold a full address header
  uint8_t type    = (f[0] >> 2) & 0x03;           // 0 = management, 1 = control, 2 = data
  uint8_t subtype = f[0] >> 4;
  if (type != 0 && type != 2) return;

  const uint8_t *a1 = f + 4;
  const uint8_t *a2 = f + 10;
  const uint8_t *a3 = f + 16;
  const uint8_t *bssid = selectedNet.bssid;
  const uint8_t *station = NULL;                  // the client end of this frame, if any
  bool ours = false;                              // frame belongs to the chosen network

  if (type == 2) {                                // data
    bool toDs   = f[1] & 0x01;
    bool fromDs = f[1] & 0x02;
    if (toDs && !fromDs && memcmp(a1, bssid, 6) == 0) {          // client -> access point
      ours = true;
      station = a2;
    } else if (!toDs && fromDs && memcmp(a2, bssid, 6) == 0) {   // access point -> client
      ours = true;
      station = a1;
    }
  } else {                                        // management: only joining/leaving frames
    bool joinLeave = (subtype <= 3) || (subtype >= 10 && subtype <= 12);
    if (joinLeave && memcmp(a3, bssid, 6) == 0) {
      station = (memcmp(a2, bssid, 6) != 0) ? a2 : a1;
    }
  }

  if (ours) {
    buckets[curBucket].dataFrames++;
    if (f[1] & 0x08) buckets[curBucket].retryFrames++;   // retry bit
  }

  // Skip group/broadcast addresses and the access point itself
  if (station != NULL && !(station[0] & 0x01) && memcmp(station, bssid, 6) != 0) {
    addDevice(station);
  }
}

// Runs in the WiFi task for every frame: keep it short, no printing.
void sniffCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type == WIFI_PKT_MISC) return;              // damaged or partial frames
  const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
  uint32_t sigLen = pkt->rx_ctrl.sig_len;
  if (sigLen == 0) return;

  uint32_t air = frameAirtimeUs(pkt->rx_ctrl.sig_mode, pkt->rx_ctrl.rate, pkt->rx_ctrl.mcs,
                                pkt->rx_ctrl.cwb, pkt->rx_ctrl.sgi, sigLen);

  portENTER_CRITICAL(&sniffMux);
  handleFrame(pkt->payload, sigLen, air);
  portEXIT_CRITICAL(&sniffMux);
}

void resetSniffStats() {
  portENTER_CRITICAL(&sniffMux);
  for (int i = 0; i < SNIFF_RING; i++) {
    buckets[i].airtimeUs   = 0;
    buckets[i].dataFrames  = 0;
    buckets[i].retryFrames = 0;
  }
  curBucket     = 0;
  bucketsFilled = 0;
  deviceCount   = 0;
  portEXIT_CRITICAL(&sniffMux);

  bucketStartMs = millis();
  airtimePct    = 0;
  lossPct       = 0;
  lossKnown     = false;
  devSnapCount  = 0;
  devState.cursor = 0;
  devState.scroll = 0;
}

void startSniff() {
  resetSniffStats();
  esp_wifi_set_promiscuous(false);

  wifi_promiscuous_filter_t filt;
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_CTRL;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&sniffCallback);
  esp_wifi_set_channel(selectedNet.channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(true);

  sniffing = true;
}

void stopSniff() {
  if (!sniffing) return;
  esp_wifi_set_promiscuous(false);
  sniffing = false;
}

// Called from loop() while sniffing. Once a second, closes the current bucket and
// recomputes airtime and loss over the last SNIFF_WINDOW_S seconds.
// Returns true when new numbers are ready.
bool sniffTick() {
  unsigned long now = millis();
  if (now - bucketStartMs < BUCKET_MS) return false;
  bucketStartMs = now;

  uint32_t air = 0, data = 0, retry = 0;
  int filled;

  portENTER_CRITICAL(&sniffMux);
  if (bucketsFilled < SNIFF_WINDOW_S) bucketsFilled++;
  curBucket = (curBucket + 1) % SNIFF_RING;
  buckets[curBucket].airtimeUs   = 0;
  buckets[curBucket].dataFrames  = 0;
  buckets[curBucket].retryFrames = 0;
  for (int k = 1; k <= bucketsFilled; k++) {
    int idx = (curBucket - k + SNIFF_RING) % SNIFF_RING;
    air   += buckets[idx].airtimeUs;
    data  += buckets[idx].dataFrames;
    retry += buckets[idx].retryFrames;
  }
  filled = bucketsFilled;
  portEXIT_CRITICAL(&sniffMux);

  airtimePct = 100.0f * (float)air / ((float)filled * 1000000.0f);
  if (airtimePct > 100.0f) airtimePct = 100.0f;

  lossKnown = (data >= (uint32_t)MIN_LOSS_FRAMES);
  lossPct   = lossKnown ? 100.0f * (float)retry / (float)data : 0;
  return true;
}

// Copy the device list so drawing never reads it while the WiFi task is adding to it
void snapshotDevices() {
  portENTER_CRITICAL(&sniffMux);
  devSnapCount = deviceCount;
  memcpy(devSnap, devices, sizeof(devices));
  portEXIT_CRITICAL(&sniffMux);
}

const char* congestionGrade(float airtimePercent) {
  if (airtimePercent < CONG_LIGHT_MAX)    return "Light";
  if (airtimePercent < CONG_MODERATE_MAX) return "Moderate";
  return "Heavy";
}

// Call from loop(). Returns true when the screen needs a redraw.
bool pumpScan() {
  if (scanKind != SCAN_NONE) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return false;

    ScanKind finished = scanKind;
    scanKind = SCAN_NONE;

    if (finished == SCAN_LIST) {
      processListScan(n);
      return currentScreen == SCREEN_NETWORKS;
    }
    processLiveScan(n);
    return currentScreen == SCREEN_LIVE;
  }

  // Nothing running: start whatever is wanted
  if (wantListScan) beginListScan();
  else if (currentScreen == SCREEN_LIVE) beginLiveScan();
  else if (currentScreen == SCREEN_CHANNEL && !sniffing) startSniff();   // starts once the radio is free
  return false;
}

// ---------- Button events ----------
const uint8_t BTN_PINS[3] = {PIN_UP, PIN_DOWN, PIN_SELECT};   // 0=UP, 1=DOWN, 2=SELECT

bool rawState[3]    = {false, false, false};   // true = pressed (pin reads LOW)
bool stableState[3] = {false, false, false};
unsigned long lastChangeMs[3] = {0, 0, 0};
unsigned long selectPressMs   = 0;
bool selectLongFired          = false;

// Returns at most one event per call. UP/DOWN fire on press,
// SELECT fires on release (short) or after 1 s held (long = BACK).
ButtonEvent readButtons() {
  unsigned long now = millis();
  ButtonEvent evt = EVT_NONE;

  for (int i = 0; i < 3; i++) {
    bool raw = (digitalRead(BTN_PINS[i]) == LOW);

    if (raw != rawState[i]) {
      rawState[i] = raw;
      lastChangeMs[i] = now;
    }

    if ((now - lastChangeMs[i]) > DEBOUNCE_MS && raw != stableState[i]) {
      stableState[i] = raw;

      if (raw) {                                   // just pressed
        if (i == 0) evt = EVT_UP;
        else if (i == 1) evt = EVT_DOWN;
        else { selectPressMs = now; selectLongFired = false; }
      } else if (i == 2 && !selectLongFired) {     // SELECT released, no long press
        evt = EVT_SELECT;
      }
    }
  }

  // Long press on SELECT: fire once while still held
  if (stableState[2] && !selectLongFired && (now - selectPressMs) >= LONG_PRESS_MS) {
    selectLongFired = true;
    evt = EVT_BACK;
  }

  return evt;
}

// ---------- List helpers ----------
const char* menuLabel(int i)   { return MENU_ITEMS[i]; }
const char* optionLabel(int i) { return OPTION_ITEMS[i]; }
const char* netLabel(int i)    { return netLabels[i]; }

void moveListRows(ListState &st, int count, int dir, int visible) {
  if (count <= 0) return;
  st.cursor = (st.cursor + dir + count) % count;
  if (st.cursor < st.scroll) st.scroll = st.cursor;
  if (st.cursor >= st.scroll + visible) st.scroll = st.cursor - visible + 1;
}

void moveList(ListState &st, int count, int dir) {
  moveListRows(st, count, dir, VISIBLE_ROWS);
}

// Header in the yellow band, up to 4 rows in the blue area, scrollbar if the list is longer
void drawList(const char* header, int count, const ListState &st, LabelFn label, const char* emptyText) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 4);
  display.print(header);

  if (count <= 0) {
    display.setCursor(4, 30);
    display.print(emptyText);
    display.display();
    return;
  }

  for (int i = 0; i < VISIBLE_ROWS; i++) {
    int idx = st.scroll + i;
    if (idx >= count) break;

    int rowTop = BLUE_TOP + i * ROW_H;   // 16, 28, 40, 52
    if (idx == st.cursor) {
      display.fillRect(0, rowTop, LIST_W, ROW_H, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(2, rowTop + 2);
    display.print(label(idx));
  }

  // Scrollbar (2 px wide at the right edge) when there is more than one page
  if (count > VISIBLE_ROWS) {
    int trackH = VISIBLE_ROWS * ROW_H;   // 48
    int thumbH = trackH * VISIBLE_ROWS / count;
    if (thumbH < 4) thumbH = 4;
    int thumbY = BLUE_TOP + (trackH - thumbH) * st.scroll / (count - VISIBLE_ROWS);
    display.fillRect(126, thumbY, 2, thumbH, SSD1306_WHITE);
  }

  display.display();
}

// ---------- Drawing ----------
// Full name (wrapped over two lines), BSSID, channel and signal for the chosen network
void drawDetails() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Header (yellow): shows which of several same-name networks this is
  display.setCursor(0, 4);
  display.print("Details");
  if (selectedTotal > 1) {
    display.print(" (");
    display.print(selectedOrder);
    display.print(")");
  }

  // Name: 21 characters per line, up to 32 characters total
  char name[33];
  displayName(selectedNet, name, sizeof(name));

  char line[22];
  snprintf(line, sizeof(line), "%.21s", name);
  display.setCursor(0, 18);
  display.print(line);

  display.setCursor(0, 27);
  if (selectedNet.ssid[0] == '\0') {
    display.print("name not broadcast");
  } else if (strlen(name) > 21) {
    snprintf(line, sizeof(line), "%.21s", name + 21);
    display.print(line);
  }

  char buf[24];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           selectedNet.bssid[0], selectedNet.bssid[1], selectedNet.bssid[2],
           selectedNet.bssid[3], selectedNet.bssid[4], selectedNet.bssid[5]);
  display.setCursor(0, 36);
  display.print(buf);

  snprintf(buf, sizeof(buf), "ch %d    %d dBm", selectedNet.channel, (int)selectedNet.rssi);
  display.setCursor(0, 45);
  display.print(buf);

  display.setCursor(0, 54);
  display.print("SELECT: continue");
  display.display();
}

// ---------- Live Signal screen ----------
// Big dBm value, a grade, and a bar graph. The bar runs from SCALE_MIN (empty,
// left) to SCALE_MAX (full, right); divider lines mark the grade boundaries.
const int SCALE_MIN = -100;
const int SCALE_MAX = 0;
const int DIVIDERS[4] = {-80, -70, -60, -30};   // poor|spotty, spotty|good, good|excellent, top of excellent

const int BAR_X = 4;
const int BAR_W = 120;
const int BAR_Y = 44;
const int BAR_H = 12;

const char* gradeFor(int dbm) {
  if (dbm >= -60) return "Excellent";   // -30 to -60 (and anything stronger)
  if (dbm >= -70) return "Good";        // -60 to -70
  if (dbm >= -80) return "Spotty";      // -70 to -80
  return "Poor";                        // weaker than -80
}

// x position on the bar for a dBm value
int barX(int dbm) {
  if (dbm < SCALE_MIN) dbm = SCALE_MIN;
  if (dbm > SCALE_MAX) dbm = SCALE_MAX;
  return BAR_X + (dbm - SCALE_MIN) * BAR_W / (SCALE_MAX - SCALE_MIN);
}

void drawLive() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Header (yellow): which network
  display.setTextSize(1);
  display.setCursor(0, 4);
  display.print(selectedLabel);

  bool lost = (missCount >= LIVE_MISS_LIMIT);
  int dbm = (int)lroundf(liveRssi);

  // Big dBm number
  char num[8];
  if (lost) snprintf(num, sizeof(num), "--");
  else      snprintf(num, sizeof(num), "%d", dbm);

  display.setTextSize(2);
  display.setCursor(0, 18);
  display.print(num);

  // "dBm" after the number, and the grade at the right edge (small text, bottom-aligned)
  display.setTextSize(1);
  display.setCursor((int)strlen(num) * 12 + 4, 26);
  display.print("dBm");

  const char* grade = lost ? "No signal" : gradeFor(dbm);
  display.setCursor(SCREEN_WIDTH - (int)strlen(grade) * 6, 26);
  display.print(grade);

  // Bar: frame (its left and right edges are the base and the max), then the fill
  int fillEnd = lost ? BAR_X : barX(dbm);
  display.drawRect(BAR_X, BAR_Y, BAR_W + 1, BAR_H, SSD1306_WHITE);
  display.fillRect(BAR_X, BAR_Y, fillEnd - BAR_X + 1, BAR_H, SSD1306_WHITE);

  // Grade dividers: drawn inverted, so they show dark on the fill and light on the empty part
  for (int i = 0; i < 4; i++) {
    display.drawFastVLine(barX(DIVIDERS[i]), BAR_Y - 3, BAR_H + 6, SSD1306_INVERSE);
  }

  display.display();
}

// ---------- Channel & Congestion screen ----------
// Top (yellow band + one blue line): channel, loss, congestion, airtime, device count.
// Below: scrollable list of the MAC addresses heard on the chosen network.
const int CH_LIST_TOP = 27;   // first list row, below the summary
const int CH_VISIBLE  = 3;    // 3 rows of 12 px

void drawChannel() {
  snapshotDevices();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  char line[24];

  // Yellow band, line 1: channel (left) and packet loss (right)
  snprintf(line, sizeof(line), "Channel %d", selectedNet.channel);
  display.setCursor(0, 0);
  display.print(line);

  if (lossKnown) snprintf(line, sizeof(line), "Loss %d%%", (int)lroundf(lossPct));
  else           snprintf(line, sizeof(line), "Loss --");
  display.setCursor(SCREEN_WIDTH - (int)strlen(line) * 6, 0);
  display.print(line);

  // Yellow band, line 2: congestion grade
  if (bucketsFilled == 0) snprintf(line, sizeof(line), "Congestion: ...");
  else                    snprintf(line, sizeof(line), "Congestion: %s", congestionGrade(airtimePct));
  display.setCursor(0, 8);
  display.print(line);

  // Blue: device count (left) and airtime (right), then a divider
  snprintf(line, sizeof(line), "Devices: %d", devSnapCount);
  display.setCursor(0, 17);
  display.print(line);

  if (bucketsFilled == 0) snprintf(line, sizeof(line), "--%% air");
  else                    snprintf(line, sizeof(line), "%d%% air", (int)lroundf(airtimePct));
  display.setCursor(SCREEN_WIDTH - (int)strlen(line) * 6, 17);
  display.print(line);

  display.drawFastHLine(0, 26, SCREEN_WIDTH, SSD1306_WHITE);

  // Device list
  if (devSnapCount == 0) {
    display.setCursor(4, 38);
    display.print(bucketsFilled < 3 ? "Listening..." : "No devices heard");
    display.display();
    return;
  }

  for (int i = 0; i < CH_VISIBLE; i++) {
    int idx = devState.scroll + i;
    if (idx >= devSnapCount) break;

    int rowTop = CH_LIST_TOP + i * ROW_H;   // 27, 39, 51
    if (idx == devState.cursor) {
      display.fillRect(0, rowTop, LIST_W, ROW_H, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }

    snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X",
             devSnap[idx][0], devSnap[idx][1], devSnap[idx][2],
             devSnap[idx][3], devSnap[idx][4], devSnap[idx][5]);
    display.setCursor(2, rowTop + 2);
    display.print(line);
  }

  // Scrollbar when there are more devices than rows
  if (devSnapCount > CH_VISIBLE) {
    int trackH = CH_VISIBLE * ROW_H;   // 36
    int thumbH = trackH * CH_VISIBLE / devSnapCount;
    if (thumbH < 4) thumbH = 4;
    int thumbY = CH_LIST_TOP + (trackH - thumbH) * devState.scroll / (devSnapCount - CH_VISIBLE);
    display.fillRect(126, thumbY, 2, thumbH, SSD1306_WHITE);
  }

  display.display();
}

// Shown when a scan finishes with nothing found
void drawNoNetworks() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 4);
  display.print("Networks: 0");

  display.setCursor(4, 22);
  display.print("No networks found");
  display.setCursor(4, 38);
  display.print("SELECT: rescan");
  display.setCursor(4, 50);
  display.print("Hold SELECT: exit");
  display.display();
}

void drawToolPlaceholder(const char* title) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 4);
  display.print(title);

  display.setCursor(0, 18);
  display.print(selectedLabel);

  char info[24];
  snprintf(info, sizeof(info), "ch %d   %d dBm", selectedNet.channel, (int)selectedNet.rssi);
  display.setCursor(0, 30);
  display.print(info);

  display.setCursor(0, 42);
  display.print("Coming soon");

  display.setCursor(0, 54);
  display.print("Hold SELECT: back");
  display.display();
}

void draw() {
  char header[24];
  switch (currentScreen) {
    case SCREEN_MENU:
      drawList("MENU", MENU_COUNT, menuState, menuLabel, "Nothing here");
      break;
    case SCREEN_NETWORKS:
      if (listScanBusy()) {
        drawList("Scanning...", 0, netState, netLabel, "Please wait");
      } else if (netCount == 0) {
        drawNoNetworks();
      } else {
        snprintf(header, sizeof(header), "Networks: %d", netCount);
        drawList(header, netCount, netState, netLabel, "No networks found");
      }
      break;
    case SCREEN_DETAILS:
      drawDetails();
      break;
    case SCREEN_OPTIONS:
      drawList(selectedLabel, OPTION_COUNT, optState, optionLabel, "Nothing here");
      break;
    case SCREEN_LIVE:     drawLive(); break;
    case SCREEN_CHANNEL:  drawChannel(); break;
    case SCREEN_LATENCY:  drawToolPlaceholder(OPTION_ITEMS[2]); break;
    case SCREEN_ACTIVITY: drawToolPlaceholder(OPTION_ITEMS[3]); break;
  }
}

// ---------- Logic ----------
void selectNetwork() {
  if (netCount == 0) return;
  int i = netState.cursor;

  selectedNet   = nets[i];
  selectedOrder = netOrder[i];
  selectedTotal = netTotal[i];
  strncpy(selectedLabel, netLabels[i], sizeof(selectedLabel) - 1);
  selectedLabel[sizeof(selectedLabel) - 1] = '\0';

  currentScreen = SCREEN_DETAILS;
}

void handleEvent(ButtonEvent evt) {
  switch (currentScreen) {
    case SCREEN_MENU:
      if (evt == EVT_UP)     moveList(menuState, MENU_COUNT, -1);
      if (evt == EVT_DOWN)   moveList(menuState, MENU_COUNT, +1);
      if (evt == EVT_SELECT) {
        currentScreen = MENU_TARGETS[menuState.cursor];
        if (currentScreen == SCREEN_NETWORKS) startScan();   // scan on entry
      }
      break;

    case SCREEN_NETWORKS:
      if (evt == EVT_UP)     moveList(netState, netCount, -1);
      if (evt == EVT_DOWN)   moveList(netState, netCount, +1);
      if (evt == EVT_SELECT) {
        if (netCount > 0)          selectNetwork();
        else if (!listScanBusy())  startScan();   // nothing found: short press rescans
      }
      if (evt == EVT_BACK)   currentScreen = SCREEN_MENU;
      break;

    case SCREEN_DETAILS:
      if (evt == EVT_SELECT) {
        optState.cursor = 0;
        optState.scroll = 0;
        currentScreen = SCREEN_OPTIONS;
      }
      if (evt == EVT_BACK)   currentScreen = SCREEN_NETWORKS;
      break;

    case SCREEN_OPTIONS:
      if (evt == EVT_UP)     moveList(optState, OPTION_COUNT, -1);
      if (evt == EVT_DOWN)   moveList(optState, OPTION_COUNT, +1);
      if (evt == EVT_SELECT) {
        currentScreen = OPTION_TARGETS[optState.cursor];
        if (currentScreen == SCREEN_LIVE) resetLive();
        if (currentScreen == SCREEN_CHANNEL) resetSniffStats();
      }
      if (evt == EVT_BACK)   currentScreen = SCREEN_DETAILS;
      break;

    case SCREEN_CHANNEL:
      if (evt == EVT_UP)   moveListRows(devState, devSnapCount, -1, CH_VISIBLE);
      if (evt == EVT_DOWN) moveListRows(devState, devSnapCount, +1, CH_VISIBLE);
      if (evt == EVT_BACK) {
        stopSniff();
        currentScreen = SCREEN_OPTIONS;
      }
      break;

    default:   // the remaining tool screens
      if (evt == EVT_BACK)   currentScreen = SCREEN_OPTIONS;
      break;
  }
  draw();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);   // faster display updates

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("SSD1306 not found - check wiring and address");
    while (true) delay(1000);
  }

  for (int i = 0; i < 3; i++) pinMode(BTN_PINS[i], INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  draw();
}

void loop() {
  ButtonEvent evt = readButtons();
  if (evt != EVT_NONE) handleEvent(evt);

  // Scans run in the background; redraw when new results arrive
  if (pumpScan()) draw();

  // Channel & Congestion: refresh the numbers once a second
  if (currentScreen == SCREEN_CHANNEL && sniffing && sniffTick()) draw();
}