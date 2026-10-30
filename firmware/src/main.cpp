// Sensor Remote ID dla Heltec WiFi LoRa 32 V3 (ESP32-S3).
//
// Odbiera ramki Open Drone ID (ASTM F3411 / EN 4709-002):
//   - Wi-Fi Beacon  (Vendor IE, OUI FA:0B:BC, typ 0x0D)
//   - Wi-Fi NAN     (Service Discovery Frame, Service Info z Message Pack)
//   - BLE 4 / BLE 5 Long Range (Service Data UUID 0xFFFA)
// i wypisuje je na USB jako linie JSON (921600 bod), np.:
//   {"t":"wifi_beacon","mac":"60:60:1f:aa:00:01","rssi":-61,"ch":6,"d":"fa0bbc0d..."}
// Agent PC (agent/rid_agent.py --serial COMx) przekazuje je do serwera RID Decoder.
//
// Polecenia przez port szeregowy:
//   ID        - przedstawienie się (info + status), używane do autodetekcji
//   CH 0      - skakanie po kanałach (domyślnie, z przewagą kanału 6)
//   CH 1..13  - stały kanał Wi-Fi
//   TIME <s>  - czas Unix z PC; zapamiętany dla bieżącego uruchomienia płytki
//   PING      - podtrzymanie połączenia z agentem (bez odpowiedzi)
//   DUMP      - wypisuje zapis z pamięci: {"dump_begin":..}, {"mem":..}..., {"dump_end":..}
//   CLEAR     - kasuje zapis z pamięci
//   TAG <id> own|foreign|-  - oznaczenie drona (swoje nie wywołują alarmów LoRa)
//   TAGCLR / TAGS           - kasuje / wypisuje oznaczenia ({"tags":{...}})
//   MESHCFG k=v;...         - alarmy Meshtastic: on, ch, psk, preset, region, pwr, freq
//   MESHTEST / MESH         - wiadomość testowa / stan nadajnika ({"mesh":{...}})
//   TAGPEND / TAGPENDCLR    - oznaczenia zmienione na telefonie (BLE) / potwierdzenie przeniesienia
//
// Telefon: usługa BLE Nordic UART „RID-Heltec-XXXX” dla strony phone/index.html (phone.h).
//
// Pamięć: gdy przez HOST_TIMEOUT_MS nie przyszło żadne polecenie (praca bez komputera),
// ramki RID są zapisywane do LittleFS (najwyżej 1 ramka danego typu z jednego MAC
// na LOG_MIN_INTERVAL_MS). Przy zapełnieniu najstarsza połowa zapisu jest usuwana.

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include "SSD1306Wire.h"

// ---- piny Heltec V3
#define PIN_VEXT 36      // zasilanie OLED, aktywne LOW
#define PIN_OLED_RST 21
#define PIN_OLED_SDA 17
#define PIN_OLED_SCL 18
#define PIN_LED 35

enum Transport : uint8_t { T_BEACON, T_NAN, T_BLE };
static const char *TRANSPORT_NAMES[] = {"wifi_beacon", "wifi_nan", "ble"};

struct Frame {
  uint8_t transport;
  uint8_t channel;
  int8_t rssi;
  uint8_t mac[6];
  uint16_t len;
  uint8_t data[256];
};

static QueueHandle_t frameQueue;

#include "drones.h"
#include "wifidrone.h"
#include "hostout.h"  // od tego miejsca Serial = USB + kanał PC przez BLE

// Surowe ramki do analizy: wszystkie ramki akcji vendor-specific (m.in. NAN) oraz
// ramki zarządzania od adresów MAC DJI. Limit: 1 ramka / s na nadawcę.
struct Diag {
  char kind[6];
  uint8_t channel;
  int8_t rssi;
  uint8_t mac[6];
  uint16_t len;   // pełna długość ramki
  uint8_t n;      // ile bajtów w data
  uint8_t data[160];
};
static QueueHandle_t diagQueue;
static const uint8_t DJI_OUIS[][3] = {
    {0x60, 0x60, 0x1F}, {0x34, 0xD2, 0x62}, {0x48, 0x1C, 0xB9}, {0x58, 0xB8, 0x58},
    {0xE4, 0x7A, 0x2C}, {0x0C, 0x9A, 0xE6}, {0x04, 0xA8, 0x5A}};
static SSD1306Wire display(0x3c, PIN_OLED_SDA, PIN_OLED_SCL, GEOMETRY_128_64);

static volatile uint32_t counts[3] = {0, 0, 0};
static volatile uint32_t dropped = 0;
// diagnostyka: wszystko, co odbiera radio (nie tylko Remote ID)
static volatile uint32_t allBeacon = 0, allAction = 0, allNan = 0, allBle = 0, diagSent = 0, allPkts = 0;
static volatile int8_t bestBeaconRssi = -127;
static volatile uint8_t currentChannel = 6;
static volatile uint8_t fixedChannel = 0;  // 0 = skakanie
static char lastId[21] = "";
static int8_t lastRssi = 0;
static uint32_t lastFrameMs = 0;

static const uint8_t ODID_OUI[3] = {0xFA, 0x0B, 0xBC};
static const uint8_t WFA_OUI[3] = {0x50, 0x6F, 0x9A};
// SHA-256("org.opendroneid.remoteid")[0..5]
static const uint8_t ODID_NAN_SERVICE_ID[6] = {0x88, 0x69, 0x19, 0x9D, 0x92, 0x09};

static void enqueue(uint8_t transport, const uint8_t *mac, int8_t rssi, const uint8_t *data, size_t len) {
  if (len > sizeof(Frame::data)) return;
  Frame f;
  f.transport = transport;
  f.channel = transport == T_BLE ? 0 : currentChannel;
  f.rssi = rssi;
  memcpy(f.mac, mac, 6);
  f.len = len;
  memcpy(f.data, data, len);
  if (xQueueSend(frameQueue, &f, 0) == pdTRUE) {
    counts[transport]++;
  } else {
    dropped++;
  }
}

static struct { uint8_t mac[6]; uint32_t ms; } diagSeen[16];

static void sendDiag(const char *kind, const uint8_t *p, size_t len, int8_t rssi) {
  const uint8_t *mac = p + 10;
  uint32_t now = millis();
  int slot = -1, oldest = 0;
  for (int i = 0; i < 16; i++) {
    if (memcmp(diagSeen[i].mac, mac, 6) == 0) { slot = i; break; }
    if (diagSeen[i].ms < diagSeen[oldest].ms) oldest = i;
  }
  if (slot >= 0 && now - diagSeen[slot].ms < 1000) return;
  if (slot < 0) { slot = oldest; memcpy(diagSeen[slot].mac, mac, 6); }
  diagSeen[slot].ms = now;
  Diag d;
  strncpy(d.kind, kind, sizeof(d.kind));
  d.channel = currentChannel;
  d.rssi = rssi;
  memcpy(d.mac, mac, 6);
  d.len = len;
  d.n = len < sizeof(d.data) ? len : sizeof(d.data);
  memcpy(d.data, p, d.n);
  if (xQueueSend(diagQueue, &d, 0) == pdTRUE) diagSent++;
}

static bool isDjiMac(const uint8_t *mac) {
  for (auto &o : DJI_OUIS)
    if (memcmp(mac, o, 3) == 0) return true;
  return false;
}

// ---------------------------------------------------------------- Wi-Fi

// true = beacon z Remote ID
static bool handleBeacon(const uint8_t *p, size_t len, int8_t rssi) {
  // nagłówek MAC 24 B + timestamp 8 + interwał 2 + capability 2
  size_t i = 36;
  while (i + 2 <= len) {
    uint8_t id = p[i], ln = p[i + 1];
    if (i + 2 + ln > len) break;
    const uint8_t *body = p + i + 2;
    if (id == 221 && ln > 5 && memcmp(body, ODID_OUI, 3) == 0 && body[3] == 0x0D) {
      enqueue(T_BEACON, p + 10, rssi, body, ln);  // adres nadawcy (addr2)
      wdNoteOdid(p + 10);
      return true;
    }
    i += 2 + ln;
  }
  return false;
}

static void handleAction(const uint8_t *p, size_t len, int8_t rssi) {
  // Public Action (4), Vendor Specific (9), OUI WFA, typ 0x13 = NAN Service Discovery
  if (len < 31) return;
  const uint8_t *b = p + 24;
  bool isNan = b[0] == 4 && b[1] == 9 && memcmp(b + 2, WFA_OUI, 3) == 0 && b[5] == 0x13;
  // kategorie 4/9 (Public Vendor Specific) i 127 (Vendor Specific) - do podglądu
  if ((b[0] == 4 && b[1] == 9) || b[0] == 127) sendDiag(isNan ? "nan" : "vact", p, len, rssi);
  if (!isNan) return;
  allNan++;
  size_t i = 30;
  while (i + 3 <= len) {
    uint8_t attr = p[i];
    uint16_t alen = p[i + 1] | (p[i + 2] << 8);
    if (i + 3 + alen > len) break;
    const uint8_t *a = p + i + 3;
    if (attr == 0x03 && alen >= 9) {  // Service Descriptor Attribute
      uint8_t ctrl = a[8];
      size_t k = 9;
      if (ctrl & 0x40) k += 2;                         // Binding Bitmap
      if ((ctrl & 0x04) && k < alen) k += 1 + a[k];    // Matching Filter
      if ((ctrl & 0x08) && k < alen) k += 1 + a[k];    // Service Response Filter
      if ((ctrl & 0x10) && k < alen) {                 // Service Info
        uint8_t silen = a[k];
        const uint8_t *si = a + k + 1;
        if (k + 1 + silen > alen || silen < 4) return;
        bool idMatch = memcmp(a, ODID_NAN_SERVICE_ID, 6) == 0;
        bool packLike = (si[1] >> 4) == 0xF && si[2] == 25;
        if (idMatch || packLike) enqueue(T_NAN, p + 10, rssi, si, silen);
      }
      return;
    }
    i += 3 + alen;
  }
}

static void IRAM_ATTR wifiSniffer(void *buf, wifi_promiscuous_pkt_type_t type) {
  allPkts++;
  if (type != WIFI_PKT_MGMT) return;
  auto *pkt = (const wifi_promiscuous_pkt_t *)buf;
  const uint8_t *p = pkt->payload;
  size_t len = pkt->rx_ctrl.sig_len;
  if (len > 4) len -= 4;  // FCS
  if (len < 24) return;
  int8_t rssi = pkt->rx_ctrl.rssi;
  if (p[0] == 0x80) {
    allBeacon++;
    if (rssi > bestBeaconRssi) bestBeaconRssi = rssi;
  }
  else if (p[0] == 0xD0) allAction++;
  if (isDjiMac(p + 10) && p[0] != 0xD0) sendDiag("dji", p, len, rssi);
  if (p[0] == 0x80) {
    if (!handleBeacon(p, len, rssi)) wdCheck(p, len, rssi, currentChannel);
  } else if (p[0] == 0x50) {
    wdCheck(p, len, rssi, currentChannel);  // odpowiedź na sondowanie (dron Wi-Fi)
  } else if (p[0] == 0xD0) {
    handleAction(p, len, rssi);
  }
}

static void channelTask(void *) {
  // kanał 6 to kanał socjalny NAN i najczęstszy kanał beaconów RID - spędzamy na nim
  // większość czasu, a co chwilę zaglądamy na pozostałe kanały 2,4 GHz
  static const uint8_t others[] = {1, 11, 2, 7, 3, 8, 4, 9, 5, 10, 12, 13};
  size_t idx = 0;
  for (;;) {
    if (fixedChannel) {
      if (currentChannel != fixedChannel) {
        currentChannel = fixedChannel;
        esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    currentChannel = 6;
    esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
    vTaskDelay(pdMS_TO_TICKS(600));
    if (fixedChannel) continue;
    currentChannel = others[idx];
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    idx = (idx + 1) % sizeof(others);
    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

// ---------------------------------------------------------------- BLE

class OdidScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *dev) override {
    allBle++;
    const uint8_t *p = dev->getPayload();
    size_t len = dev->getPayloadLength();
    size_t i = 0;
    while (i + 1 < len) {
      uint8_t ln = p[i];
      if (ln == 0 || i + 1 + ln > len) break;
      uint8_t type = p[i + 1];
      const uint8_t *d = p + i + 2;
      // Service Data 16-bit UUID 0xFFFA, app code 0x0D
      if (type == 0x16 && ln >= 5 && d[0] == 0xFA && d[1] == 0xFF && d[2] == 0x0D) {
        // adres BLE w NimBLE jest w odwrotnej kolejności bajtów
        const uint8_t *a = dev->getAddress().getNative();
        uint8_t mac[6] = {a[5], a[4], a[3], a[2], a[1], a[0]};
        enqueue(T_BLE, mac, dev->getRSSI(), d + 2, ln - 3);
        return;
      }
      i += 1 + ln;
    }
  }
};

static void restartScan(NimBLEScanResults) {
  NimBLEDevice::getScan()->start(0, restartScan, false);
}

// ---------------------------------------------------------------- wyjście i UI

static void printFrame(const Frame &f) {
  static const char hex[] = "0123456789abcdef";
  static char line[700];
  int n = snprintf(line, sizeof(line),
                   "{\"t\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"rssi\":%d,\"ch\":%u,\"d\":\"",
                   TRANSPORT_NAMES[f.transport], f.mac[0], f.mac[1], f.mac[2], f.mac[3], f.mac[4],
                   f.mac[5], f.rssi, f.channel);
  for (size_t i = 0; i < f.len && n < (int)sizeof(line) - 4; i++) {
    line[n++] = hex[f.data[i] >> 4];
    line[n++] = hex[f.data[i] & 0x0F];
  }
  line[n++] = '"';
  line[n++] = '}';
  line[n] = 0;
  Serial.println(line);
}

// ---------------------------------------------------------------- pamięć (praca bez komputera)

#define LOG_FILE "/rid.bin"
#define LOG_OLD "/rid_old.bin"
#define LOG_MAGIC 0xA5
#define LOG_MIN_INTERVAL_MS 3000
#define LOG_FLUSH_MS 5000
#define HOST_TIMEOUT_MS 30000
#define BOOT_SLOTS 32

struct __attribute__((packed)) LogHdr {
  uint8_t magic;
  uint8_t transport;
  int8_t rssi;
  uint8_t channel;
  uint8_t mac[6];
  uint16_t boot;
  uint32_t up;  // sekundy od startu płytki
  uint16_t len;
};

// czas Unix startu płytki, znany gdy w danym uruchomieniu był podłączony agent
struct BootTime {
  uint16_t boot;
  uint32_t base;
};

static Preferences prefs;
static bool fsOk = false;
static File logFile;
static uint16_t bootId = 0;
static uint32_t logCountCur = 0, logCountOld = 0;
static size_t logMaxFile = 0;
static bool logDirty = false;
static uint32_t lastFlushMs = 0;
static uint32_t lastHostMs = 0;
static bool hostSeen = false;
static struct { uint8_t mac[6]; uint8_t transport, type; uint32_t ms; } logSeen[32];

static File dumpFile;
static uint8_t dumpStage = 0;  // 0 = brak, 1 = starszy plik, 2 = bieżący
static uint32_t dumpCount = 0;

static bool hostConnected() { return hostSeen && millis() - lastHostMs < HOST_TIMEOUT_MS; }

static void bootKey(char *key, uint16_t boot) { snprintf(key, 6, "b%u", boot % BOOT_SLOTS); }

static void logSaveCounts() {
  prefs.putUInt("cnt", logCountCur);
  prefs.putUInt("cnto", logCountOld);
}

static void logOpen() { logFile = LittleFS.open(LOG_FILE, FILE_APPEND); }

static void logInit() {
  prefs.begin("rid", false);
  bootId = prefs.getUShort("boot", 0) + 1;
  prefs.putUShort("boot", bootId);
  logCountCur = prefs.getUInt("cnt", 0);
  logCountOld = prefs.getUInt("cnto", 0);
  fsOk = LittleFS.begin(true);  // pierwsze uruchomienie formatuje partycję
  if (!fsOk) return;
  logMaxFile = LittleFS.totalBytes() * 45 / 100;
  if (!LittleFS.exists(LOG_FILE)) logCountCur = 0;
  if (!LittleFS.exists(LOG_OLD)) logCountOld = 0;
  logOpen();
}

static void logRotate() {
  logFile.close();
  LittleFS.remove(LOG_OLD);
  LittleFS.rename(LOG_FILE, LOG_OLD);
  logCountOld = logCountCur;
  logCountCur = 0;
  logSaveCounts();
  logOpen();
}

static void logFrame(const Frame &f) {
  if (!fsOk || !logFile || hostConnected()) return;
  // BLE 4 niesie jedną wiadomość na reklamę (app code, licznik, nagłówek), więc
  // limit liczymy osobno dla każdego typu wiadomości; Wi-Fi niesie cały Message Pack
  uint8_t type = (f.transport == T_BLE && f.len >= 3) ? (f.data[2] >> 4) : 0xF;
  uint32_t now = millis();
  int slot = -1, oldest = 0;
  for (int i = 0; i < 32; i++) {
    if (logSeen[i].transport == f.transport && logSeen[i].type == type &&
        memcmp(logSeen[i].mac, f.mac, 6) == 0) { slot = i; break; }
    if (logSeen[i].ms < logSeen[oldest].ms) oldest = i;
  }
  if (slot >= 0 && now - logSeen[slot].ms < LOG_MIN_INTERVAL_MS) return;
  if (slot < 0) {
    slot = oldest;
    memcpy(logSeen[slot].mac, f.mac, 6);
    logSeen[slot].transport = f.transport;
    logSeen[slot].type = type;
  }
  logSeen[slot].ms = now;

  if (logFile.size() + sizeof(LogHdr) + f.len > logMaxFile) logRotate();
  LogHdr h = {LOG_MAGIC, f.transport, f.rssi, f.channel, {}, bootId, now / 1000, f.len};
  memcpy(h.mac, f.mac, 6);
  logFile.write((const uint8_t *)&h, sizeof(h));
  logFile.write(f.data, f.len);
  logCountCur++;
  logDirty = true;
}

static void logTick() {
  if (logDirty && millis() - lastFlushMs > LOG_FLUSH_MS) {
    lastFlushMs = millis();
    logDirty = false;
    logFile.flush();
    logSaveCounts();
  }
}

static void setBootTime(uint32_t unix) {
  BootTime bt = {bootId, unix - millis() / 1000};
  char key[6];
  bootKey(key, bootId);
  prefs.putBytes(key, &bt, sizeof(bt));
}

static void logClear() {
  if (dumpStage || !fsOk) return;
  if (logFile) logFile.close();
  LittleFS.remove(LOG_FILE);
  LittleFS.remove(LOG_OLD);
  logCountCur = logCountOld = 0;
  logSaveCounts();
  logOpen();
  Serial.println("{\"info\":\"pamiec wyczyszczona\"}");
}

static bool dumpOpen(const char *path) {
  if (!LittleFS.exists(path)) return false;
  dumpFile = LittleFS.open(path, FILE_READ);
  return (bool)dumpFile;
}

static void dumpStart() {
  if (dumpStage) return;
  if (logFile) {
    logFile.flush();
    logFile.close();
  }
  Serial.printf("{\"dump_begin\":{\"records\":%u,\"boot\":%u,\"up\":%u,\"fs\":%s,\"boots\":{",
                logCountCur + logCountOld, bootId, millis() / 1000, fsOk ? "true" : "false");
  bool first = true;
  for (int i = 0; i < BOOT_SLOTS; i++) {
    char key[6];
    snprintf(key, sizeof(key), "b%d", i);
    BootTime bt;
    if (prefs.isKey(key) && prefs.getBytes(key, &bt, sizeof(bt)) == sizeof(bt)) {
      Serial.printf("%s\"%u\":%u", first ? "" : ",", bt.boot, bt.base);
      first = false;
    }
  }
  Serial.println("}}}");
  dumpCount = 0;
  dumpStage = 1;
  if (!fsOk || !dumpOpen(LOG_OLD)) dumpFile = File();
}

// wysyła kilka rekordów na obieg pętli, żeby nasłuch na żywo działał dalej
static void dumpStep() {
  static const char hex[] = "0123456789abcdef";
  static char line[720];
  static uint8_t data[256];
  for (int k = 0; k < 20 && dumpStage; k++) {
    LogHdr h;
    bool ok = dumpFile && dumpFile.read((uint8_t *)&h, sizeof(h)) == sizeof(h) &&
              h.magic == LOG_MAGIC && h.transport <= T_BLE && h.len <= sizeof(data) &&
              dumpFile.read(data, h.len) == h.len;
    if (!ok) {  // koniec pliku (albo urwany ostatni rekord po zaniku zasilania)
      if (dumpFile) dumpFile.close();
      if (dumpStage == 1) {
        dumpStage = 2;
        if (!fsOk || !dumpOpen(LOG_FILE)) dumpFile = File();
        continue;
      }
      dumpStage = 0;
      Serial.printf("{\"dump_end\":{\"records\":%u}}\n", dumpCount);
      if (fsOk) logOpen();
      return;
    }
    int n = snprintf(line, sizeof(line),
                     "{\"mem\":{\"t\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"rssi\":%d,"
                     "\"ch\":%u,\"b\":%u,\"up\":%u,\"d\":\"",
                     TRANSPORT_NAMES[h.transport], h.mac[0], h.mac[1], h.mac[2], h.mac[3], h.mac[4],
                     h.mac[5], h.rssi, h.channel, h.boot, h.up);
    for (size_t i = 0; i < h.len && n < (int)sizeof(line) - 5; i++) {
      line[n++] = hex[data[i] >> 4];
      line[n++] = hex[data[i] & 0x0F];
    }
    line[n++] = '"';
    line[n++] = '}';
    line[n++] = '}';
    line[n] = 0;
    Serial.println(line);
    dumpCount++;
  }
}

// ---------------------------------------------------------------- oznaczenia i Meshtastic

#include "mesh.h"

static void tagsLoad() {
  if (prefs.getBytes("tags", tags, sizeof(tags)) != sizeof(tags)) memset(tags, 0, sizeof(tags));
}

static void tagsSave() { prefs.putBytes("tags", tags, sizeof(tags)); }

static void tagSet(const char *id, uint8_t st) {
  if (!id[0] || strlen(id) > 20) return;
  TagEntry *slot = nullptr;
  for (auto &t : tags) {
    if (t.status && strcmp(t.id, id) == 0) { slot = &t; break; }
    if (!t.status && !slot) slot = &t;
  }
  if (!slot) return;  // brak miejsca (MAX_TAGS)
  strlcpy(slot->id, id, sizeof(slot->id));
  slot->status = st;
}

static void tagsPrint() {
  Serial.print("{\"tags\":{");
  bool first = true;
  for (auto &t : tags) {
    if (!t.status) continue;
    Serial.printf("%s\"%s\":\"%s\"", first ? "" : ",", t.id, t.status == TAG_OWN ? "own" : "foreign");
    first = false;
  }
  Serial.println("}}");
}

#include "hist.h"

// ---------------------------------------------------------------- bateria

// Dzielnik 390k/100k na GPIO1, włączany pinem GPIO37: aktywny LOW w V3.0/3.1,
// HIGH w V3.2. Poziom wykrywamy sami: przy złym poziomie ADC widzi ~0 V.
#define PIN_VBAT 1
#define PIN_VBAT_CTRL 37
#define VBAT_MIN_MV 2500  // poniżej: brak baterii (albo zły poziom sterowania)
#define USB_MV 4300       // powyżej: USB bez baterii (z baterią ładowarka trzyma najwyżej ~4,2 V)
#define CHG_JUMP_MV 40    // skok napięcia w ciągu ~15 s: podłączenie / odłączenie ładowarki
#define CHG_TREND_MV 25   // zmiana w ciągu minuty: na samej baterii napięcie nigdy nie rośnie
#define CHG_FULL_MV 4180  // ładowarka w fazie stałego napięcia (koniec ładowania)

// Płytka nie ma sygnału ładowania (dioda ładowarki nie jest podłączona do procesora), więc
// ładowanie rozpoznajemy po napięciu. Podczas ładowania napięcie jest zawyżone przez ładowarkę,
// więc procent nie jest wtedy liczony (batPct = -1, chyba że bateria jest już pełna).
static int8_t vbatCtrl = -1;  // -1 nieznany, LOW albo HIGH
static uint16_t batMv = 0;
static int8_t batPct = -1;    // -1 = brak baterii albo ładowanie (procent nieznany)
static bool batChg = false;   // podłączona ładowarka (USB)
static bool batFull = false;  // ładowarka podłączona, bateria pełna
static int8_t batPctBefore = -1;  // procent tuż przed podłączeniem ładowarki (-1 nieznany)
static uint16_t batRaw[12];       // surowe odczyty co 5 s (ostatnia minuta)
static uint16_t batMin[31];       // odczyt co minutę (ostatnie 30 minut)
static uint8_t batRawN = 0, batMinN = 0;

static uint32_t readVbat(uint8_t level) {
  pinMode(PIN_VBAT_CTRL, OUTPUT);
  digitalWrite(PIN_VBAT_CTRL, level);
  delay(10);
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_VBAT);
  digitalWrite(PIN_VBAT_CTRL, !level);  // dzielnik wyłączony między pomiarami (mniejszy pobór)
  // 4,9 (dzielnik) x 1,045 - poprawka jak w Meshtastic dla Heltec V3: samo 4,9 zaniża
  // odczyt o ok. 4-5% (pełna bateria pokazywała ~4,06 V / 85%)
  return sum / 8 * 512 / 100;
}

static int8_t vbatPercent(uint16_t mv) {
  // krzywa rozładowania ogniwa Li-ion pod obciążeniem płytki (~100 mA: nasłuch Wi-Fi i BLE).
  // Pomiar 1.8: pełna bateria (ładowarka po minucie w fazie stałego napięcia) zaraz po odłączeniu
  // USB pokazuje ~4,06 V, więc 100% to 4,08 V, a nie 4,2 V jak dla ogniwa bez obciążenia.
  static const uint16_t MV[] = {4080, 4000, 3930, 3870, 3820, 3770, 3720, 3680, 3620, 3550, 3400, 3300};
  static const uint8_t PCT[] = {100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 5, 0};
  if (mv >= MV[0]) return 100;
  for (int i = 1; i < 12; i++)
    if (mv >= MV[i]) return PCT[i] + (PCT[i - 1] - PCT[i]) * (mv - MV[i]) / (MV[i - 1] - MV[i]);
  return 0;
}

static void updateBattery() {
  uint32_t mv = 0;
  if (vbatCtrl < 0) {
    for (uint8_t level : {LOW, HIGH}) {
      mv = readVbat(level);
      if (mv > VBAT_MIN_MV) {
        vbatCtrl = level;
        break;
      }
    }
  } else {
    mv = readVbat(vbatCtrl);
  }
  if (mv < VBAT_MIN_MV) {  // brak baterii; poziom sterowania sprawdzimy ponownie
    vbatCtrl = -1;
    batMv = 0;
    batPct = -1;
    batChg = batFull = false;
    batRawN = batMinN = 0;
    return;
  }
  if (mv > USB_MV) {  // USB bez baterii
    batMv = mv;
    batPct = -1;
    batChg = true;
    batFull = false;
    batRawN = batMinN = 0;
    return;
  }
  // historia odczytów
  if (batRawN == 12) memmove(batRaw, batRaw + 1, 11 * sizeof(uint16_t));
  else batRawN++;
  batRaw[batRawN - 1] = mv;
  static uint8_t tick = 0;
  if (tick++ % 12 == 0) {
    if (batMinN == 31) memmove(batMin, batMin + 1, 30 * sizeof(uint16_t));
    else batMinN++;
    batMin[batMinN - 1] = mv;
  }
  auto med3 = [](const uint16_t *p) {
    uint16_t a = p[0], b = p[1], c = p[2];
    return max(min(a, b), min(max(a, b), c));
  };
  auto avg4 = [](const uint16_t *p) { return (uint16_t)((p[0] + p[1] + p[2] + p[3]) / 4); };
  uint16_t cur = batRawN >= 3 ? med3(batRaw + batRawN - 3) : mv;  // mediana odcina chwilowe spadki przy nadawaniu
  bool was = batChg;
  if (!batMv) batChg = mv >= 4150;  // start już na USB: skoku nie widać, zakładamy ładowanie
  if (batRawN >= 6) {  // skok: podłączenie lub odłączenie
    int d = (int)cur - med3(batRaw + batRawN - 6);
    if (d >= CHG_JUMP_MV) batChg = true;
    if (d <= -CHG_JUMP_MV) batChg = false;
  }
  if (batRawN == 12) {  // trend w ciągu minuty
    int d = (int)avg4(batRaw + 8) - avg4(batRaw);
    if (d >= CHG_TREND_MV) batChg = true;
    if (d <= -CHG_TREND_MV) batChg = false;
  }
  // przy ładowaniu prądem napięcie stale rośnie; stoi tylko w fazie stałego napięcia (~4,2 V),
  // która przy niepełnej baterii trwa jeszcze kilkadziesiąt minut - stąd "pełna" dopiero po 30 min
  bool flat5 = batMinN >= 6 && (int)cur - batMin[batMinN - 6] < 10;
  bool flat30 = batMinN == 31 && (int)cur - batMin[0] < 15;
  if (batChg && flat5 && cur < CHG_FULL_MV) batChg = false;
  batFull = batChg && flat30 && cur >= CHG_FULL_MV;
  if (batChg && !was) {
    batPctBefore = batMv ? batPct : -1;
  } else if (!batChg && was) {
    batMv = 0;  // po odłączeniu: bez wygładzania od zawyżonego napięcia ładowania
    batRawN = 0;
    batRaw[batRawN++] = mv;
  }
  batMv = batMv ? (batMv * 3 + mv) / 4 : mv;  // wygładzanie
  batPct = batFull ? 100 : batChg ? -1 : vbatPercent(batMv);
}

static void drawBattery() {
  const int x = 104, y = 1;  // ikona 18x9 w prawym górnym rogu
  display.drawRect(x, y, 16, 9);
  display.fillRect(x + 16, y + 2, 2, 5);
  if (batChg) {  // błyskawica w ikonie
    if (batFull) display.fillRect(x + 2, y + 2, 12, 5);
    display.setColor(batFull ? BLACK : WHITE);
    display.drawLine(x + 9, y + 1, x + 6, y + 4);
    display.drawLine(x + 6, y + 4, x + 10, y + 4);
    display.drawLine(x + 10, y + 4, x + 7, y + 7);
    display.setColor(WHITE);
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(101, 0, batFull ? "pelna" : "LAD");
  } else if (batPct < 0) {
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(101, 0, batMv ? "" : "USB");
  } else {
    display.fillRect(x + 2, y + 2, 12 * batPct / 100, 5);
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(101, 0, String(batPct) + "%");
  }
  display.setTextAlignment(TEXT_ALIGN_LEFT);
}

#include "phone.h"
#include "ui.h"

// ekran główny (jak w poprzednich wersjach): liczniki, kanał, ostatni dron
static void drawMain() {
  String head = hostConnected() ? "PC" : fsOk ? "ZAPIS " + String(logCountCur + logCountOld) : "BRAK PAMIECI";
  if (phoneConnected) head += " TEL";
  display.drawString(0, 0, head);
  drawBattery();
  display.drawString(0, 13, "Beacon " + String(counts[T_BEACON]) + "  NAN " + String(counts[T_NAN]));
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(127, 13, "CH" + String(currentChannel) + (fixedChannel ? "" : "*"));
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 25, "BLE " + String(counts[T_BLE]) + (dropped ? "  utr. " + String(dropped) : ""));
  if (lastId[0]) {
    uint32_t age = (millis() - lastFrameMs) / 1000;
    uint8_t tag = tagStatus(lastId);
    display.drawString(0, 39, tag ? String(tagText(tag)) + " " + lastId : String(lastId));
    display.drawString(0, 51, String(lastRssi) + " dBm, " + String(age) + " s temu");
  } else {
    display.drawString(0, 45, "brak dronow w zasiegu");
  }
}

static void drawDisplay() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  if (phoneConnected && !phoneSecure) {  // telefon się paruje - pokazujemy PIN do wpisania
    display.drawString(0, 0, "Parowanie telefonu");
    display.drawString(0, 13, "Wpisz w telefonie PIN:");
    display.setFont(ArialMT_Plain_24);
    display.drawString(0, 30, String(phonePin));
    display.display();
    return;
  }
  if (!displayOn) return;  // ekran wygaszony (dwuklik albo bezczynność) - nasłuch działa dalej
  if (screen == SCR_LIST) drawList();
  else if (screen == SCR_DETAIL) drawDetail();
  else if (screen == SCR_STATUS) drawStatus();
  else if (screen == SCR_SETTINGS) drawSettings();
  else drawMain();
  drawToast();
  display.display();
}

static void printStatus() {
  Serial.printf("{\"status\":{\"beacon\":%u,\"nan\":%u,\"ble\":%u,\"dropped\":%u,\"ch\":%u,"
                "\"all_beacon\":%u,\"all_action\":%u,\"all_nan\":%u,\"all_ble\":%u,\"diag\":%u,"
                "\"all_pkts\":%u,\"best_beacon_rssi\":%d,"
                "\"mem\":%u,\"mem_kb\":%u,\"mem_total_kb\":%u,\"fs\":%s,\"boot\":%u,\"up\":%u,"
                "\"bat_mv\":%u,\"bat_pct\":%d,\"bat_chg\":%d,\"pin\":%u,\"bonds\":%d,\"phone\":%s,\"wifi_drone\":%u,"
                "\"ble_name\":\"%s\",\"pc_ble\":%s,\"rst\":%d}}\n",
                counts[T_BEACON], counts[T_NAN], counts[T_BLE], dropped, currentChannel,
                allBeacon, allAction, allNan, allBle, diagSent, allPkts, bestBeaconRssi,
                logCountCur + logCountOld, fsOk ? (unsigned)(LittleFS.usedBytes() / 1024) : 0,
                fsOk ? (unsigned)(LittleFS.totalBytes() / 1024) : 0, fsOk ? "true" : "false",
                bootId, millis() / 1000, batMv, batPct, batFull ? 2 : batChg ? 1 : 0, phonePin, phoneBonds(),
                phoneConnected ? (phoneSecure ? "\"ok\"" : "\"parowanie\"") : "null", wdSeen,
                phoneName, hostBleOn ? "true" : "false",
                (int)esp_reset_reason());  // przyczyna restartu: 1 zasilanie, 4-7 błąd/watchdog, 9 spadek napięcia
}

static void handleSerialCommand() {
  static char buf[220];  // MESHCFG z kluczem PSK
  static size_t n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      buf[n] = 0;
      if (n) {  // każde polecenie = komputer jest podłączony, wstrzymujemy zapis
        hostSeen = true;
        lastHostMs = millis();
      }
      if (strcasecmp(buf, "ID") == 0) {  // agent PC rozpoznaje płytkę po tej odpowiedzi
        Serial.println("{\"info\":\"RID sensor Heltec V3\",\"fw\":\"1.8\"}");
        printStatus();
        meshPrintStatus(Serial);
      } else if (strncasecmp(buf, "TIME ", 5) == 0) {
        uint32_t t = strtoul(buf + 5, nullptr, 10);
        if (t > 1700000000UL) {
          setBootTime(t);
          unixBase = t - millis() / 1000;
        }
      } else if (strncasecmp(buf, "TAG ", 4) == 0) {
        char *id = buf + 4, *st = strchr(id, ' ');
        if (st) {
          *st++ = 0;
          tagSet(id, !strcasecmp(st, "own") ? TAG_OWN : !strcasecmp(st, "foreign") ? TAG_FOREIGN : TAG_NONE);
          tagsSave();
        }
      } else if (strcasecmp(buf, "TAGCLR") == 0) {
        memset(tags, 0, sizeof(tags));
        tagsSave();
      } else if (strcasecmp(buf, "TAGS") == 0) {
        tagsPrint();
      } else if (strcasecmp(buf, "TAGPEND") == 0) {  // oznaczenia zmienione na telefonie
        char out[700];
        tagsJson(out, sizeof(out), true);
        Serial.print(out);
      } else if (strcasecmp(buf, "TAGPENDCLR") == 0) {
        tagPendingCount = 0;
      } else if (strncasecmp(buf, "PIN ", 4) == 0) {  // nowy PIN parowania telefonów (6 cyfr)
        phoneSetPin(strtoul(buf + 4, nullptr, 10));
        printStatus();
#ifndef RID_NO_BLE
      } else if (strcasecmp(buf, "BONDCLR") == 0) {  // zapomina sparowane telefony
        NimBLEDevice::deleteAllBonds();
        printStatus();
#endif
      } else if (strncasecmp(buf, "MESHCFG ", 8) == 0) {
        meshApplyCfg(buf + 8, true);
        meshPrintStatus(Serial);
      } else if (strcasecmp(buf, "MESHTEST") == 0) {
        if (meshRadioOk) meshSendText("RID Heltec: test alarmow o dronach");
        meshPrintStatus(Serial);
      } else if (strcasecmp(buf, "MESH") == 0) {
        meshPrintStatus(Serial);
      } else if (strcasecmp(buf, "DUMP") == 0) {
        dumpStart();
      } else if (strcasecmp(buf, "CLEAR") == 0) {
        logClear();
      } else if (n >= 3 && strncasecmp(buf, "CH", 2) == 0) {
        int ch = atoi(buf + 2);
        if (ch >= 0 && ch <= 13) {
          fixedChannel = ch;
          Serial.printf("{\"info\":\"kanal %s\"}\n", ch ? String(ch).c_str() : "skakanie");
        }
      }
      n = 0;
    } else if (n < sizeof(buf) - 1) {
      buf[n++] = c;
    }
  }
}

void setup() {
  Serial.begin(921600);
  pinMode(PIN_LED, OUTPUT);

  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);
  delay(50);
  pinMode(PIN_OLED_RST, OUTPUT);
  digitalWrite(PIN_OLED_RST, LOW);
  delay(20);
  digitalWrite(PIN_OLED_RST, HIGH);
  display.init();
  display.flipScreenVertically();

  logInit();
  histLoad();
  uiBegin();
  tagsLoad();
  meshApplyCfg(prefs.getString("mesh", "").c_str(), false);

  frameQueue = xQueueCreate(32, sizeof(Frame));
  diagQueue = xQueueCreate(16, sizeof(Diag));
  wdBegin();

  // Wi-Fi w trybie nasłuchu (bez łączenia z siecią)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
#ifdef RID_NO_BLE
  // bez usypiania radia; przy włączonym BLE ESP-IDF wymaga modem sleep (inaczej abort)
  esp_wifi_set_ps(WIFI_PS_NONE);
#endif
  wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL};
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&wifiSniffer);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
  xTaskCreatePinnedToCore(channelTask, "chan", 2048, nullptr, 1, nullptr, 0);

#ifndef RID_NO_BLE
  // BLE: pasywne skanowanie ciągłe, 1M + Coded PHY (Long Range)
  NimBLEDevice::init("");
  // serwer GATT dla telefonu musi powstać przed startem skanowania - inaczej NimBLE
  // nie zarejestruje usług (assert w ble_svc_gap_init i restart płytki w kółko)
  phoneBegin();  // podgląd na telefonie przez BLE (phone/index.html)
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new OdidScanCallbacks(), true);
  scan->setActiveScan(false);
  scan->setDuplicateFilter(false);
  scan->setMaxResults(0);
  scan->setInterval(100);
  scan->setWindow(50);  // reszta czasu dla Wi-Fi (wspólne radio)
  scan->start(0, restartScan, false);
#endif

  Serial.println("{\"info\":\"RID sensor Heltec V3 gotowy\"}");
}

void loop() {
  static uint32_t lastDraw = 0, lastStatus = 0, lastBattery = 0;
  Frame f;
  while (xQueueReceive(frameQueue, &f, pdMS_TO_TICKS(20)) == pdTRUE) {
    printFrame(f);
    logFrame(f);
    DroneInfo *dr = parseFrame(f);
    if (dr) histUpdate(*dr, f);
    if (dr && !dr->shown) {  // nowy dron budzi wygaszony ekran
      dr->shown = true;
      uiWake();
    }
    if (dr && dr->id[0]) strlcpy(lastId, dr->id, sizeof(lastId));
    lastRssi = f.rssi;
    lastFrameMs = millis();
    digitalWrite(PIN_LED, HIGH);
  }
  WdEvent we;
  while (xQueueReceive(wdQueue, &we, 0) == pdTRUE) {  // dron Wi-Fi bez Remote ID
    DroneInfo *dr = wdApply(we);
    if (!dr->shown) {
      dr->shown = true;
      uiWake();
      if (tagStatus(dr->id) != TAG_OWN) uiToast((String("DRON WI-FI ") + dr->id).c_str());
    }
    wdPrint(Serial, *dr);
    strlcpy(lastId, dr->id, sizeof(lastId));
    lastRssi = we.rssi;
    lastFrameMs = millis();
  }
  digitalWrite(PIN_LED, LOW);
  Diag d;
  while (xQueueReceive(diagQueue, &d, 0) == pdTRUE) {
    static const char hex[] = "0123456789abcdef";
    static char line[420];
    int n = snprintf(line, sizeof(line),
                     "{\"diag\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"rssi\":%d,\"ch\":%u,\"len\":%u,\"d\":\"",
                     d.kind, d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5], d.rssi, d.channel, d.len);
    for (size_t i = 0; i < d.n && n < (int)sizeof(line) - 4; i++) {
      line[n++] = hex[d.data[i] >> 4];
      line[n++] = hex[d.data[i] & 0x0F];
    }
    line[n++] = '"';
    line[n++] = '}';
    line[n] = 0;
    Serial.println(line);
  }
  handleSerialCommand();
  if (dumpStage) dumpStep();
  logTick();
  meshTick();
  histTick();
  uiButtonTick();
  phoneTick();

  uint32_t now = millis();
  if (now - lastBattery > 5000 || !lastBattery) {
    lastBattery = now;
    updateBattery();
  }
  if (now - lastDraw > 500) {
    lastDraw = now;
    drawDisplay();
  }
  if (now - lastStatus > 10000) {
    lastStatus = now;
    printStatus();
    meshPrintStatus(Serial);
  }
}
