// Historia wykrytych dronów (trwała, w LittleFS) - żeby telefon pokazał to samo, co strona
// RID Decoder po imporcie pamięci: kto latał, kiedy (od-do), maks. wysokość, ostatnia pozycja
// drona i operatora oraz ślad lotu.
//   /hist.bin             - zestawienie do HIST_MAX dronów (zapis co HIST_SAVE_MS)
//   /trk.bin, /trk_old.bin - punkty śladu (co HIST_TRACK_MS na drona), rotacja jak zapis ramek
// Czas zapisujemy jako (uruchomienie płytki, sekundy od startu); na czas Unix przeliczamy przy
// wysyłce: bieżące uruchomienie - unixBase, wcześniejsze - czasy startu z NVS (BootTime).
// Pełne ramki dalej idą do zapisu /rid.bin i na komputer (DUMP); CLEAR nie kasuje historii.
// Dołączany z main.cpp po mesh.h (djb2) i części z zapisem (prefs, BootTime).
#pragma once

#define HIST_MAX 32
#define HIST_FILE "/hist.bin"
#define HIST_TMP "/hist.tmp"
#define TRK_FILE "/trk.bin"
#define TRK_OLD "/trk_old.bin"
#define TRK_MAX_FILE (200 * 1024)
#define HIST_TRACK_MS 10000
#define HIST_SAVE_MS 30000

struct HistEntry {
  char id[21];       // UAS ID albo "mac:xxxxxxxxxxxx" dla drona bez ID
  uint8_t used;
  uint16_t firstBoot, lastBoot;
  uint32_t firstUp, lastUp;
  int32_t lat, lon, opLat, opLon;
  uint8_t hasLoc, hasOp, transports;  // transports: bit 0 beacon, 1 NAN, 2 BLE
  int8_t bestRssi;
  float maxH;
  uint32_t frames;
};

struct __attribute__((packed)) TrackPoint {
  uint32_t idHash;
  uint16_t boot;
  uint32_t up;
  int32_t lat, lon;
  int16_t alt;  // m, -32768 = brak
};

static HistEntry hist[HIST_MAX];
static uint32_t histLastTrackMs[HIST_MAX];
static bool histDirty = false;
static uint32_t histLastSave = 0;
static bool histBootTimeSaved = false;

static void histLoad() {
  memset(hist, 0, sizeof(hist));
  if (!fsOk || !LittleFS.exists(HIST_FILE)) return;
  File f = LittleFS.open(HIST_FILE, FILE_READ);
  if (f.size() == sizeof(hist)) f.read((uint8_t *)hist, sizeof(hist));
  f.close();
}

static void histSave() {
  if (!fsOk) return;
  File f = LittleFS.open(HIST_TMP, FILE_WRITE);
  if (!f) return;
  f.write((const uint8_t *)hist, sizeof(hist));
  f.close();
  LittleFS.remove(HIST_FILE);
  LittleFS.rename(HIST_TMP, HIST_FILE);  // zanik zasilania w trakcie nie psuje poprzedniej wersji
}

static void histKey(const DroneInfo &d, char *out) {
  if (d.id[0]) strlcpy(out, d.id, 21);
  else snprintf(out, 21, "mac:%02x%02x%02x%02x%02x%02x", d.macs[0][0], d.macs[0][1], d.macs[0][2],
                d.macs[0][3], d.macs[0][4], d.macs[0][5]);
}

static int histSlot(const char *key) {
  int freeSlot = -1, oldest = 0;
  for (int i = 0; i < HIST_MAX; i++) {
    if (hist[i].used && !strcmp(hist[i].id, key)) return i;
    if (!hist[i].used && freeSlot < 0) freeSlot = i;
    if (hist[i].used && (hist[i].lastBoot < hist[oldest].lastBoot ||
                         (hist[i].lastBoot == hist[oldest].lastBoot && hist[i].lastUp < hist[oldest].lastUp)))
      oldest = i;
  }
  int s = freeSlot >= 0 ? freeSlot : oldest;  // pełna historia: wypada najdawniej widziany dron
  memset(&hist[s], 0, sizeof(HistEntry));
  strlcpy(hist[s].id, key, sizeof(hist[s].id));
  hist[s].used = 1;
  hist[s].firstBoot = bootId;
  hist[s].firstUp = millis() / 1000;
  hist[s].bestRssi = -127;
  histLastTrackMs[s] = 0;
  return s;
}

static void histTrackAppend(const HistEntry &h, const DroneInfo &d) {
  if (!fsOk) return;
  if (LittleFS.exists(TRK_FILE)) {
    File f = LittleFS.open(TRK_FILE, FILE_READ);
    size_t size = f.size();
    f.close();
    if (size + sizeof(TrackPoint) > TRK_MAX_FILE) {
      LittleFS.remove(TRK_OLD);
      LittleFS.rename(TRK_FILE, TRK_OLD);
    }
  }
  TrackPoint p = {djb2(h.id), bootId, millis() / 1000, d.lat, d.lon,
                  (int16_t)(isnan(d.altGeo) ? (isnan(d.height) ? -32768 : d.height) : d.altGeo)};
  File f = LittleFS.open(TRK_FILE, FILE_APPEND);
  if (f) {
    f.write((const uint8_t *)&p, sizeof(p));
    f.close();
  }
}

// po każdej ramce: aktualizacja zestawienia (w RAM) i co HIST_TRACK_MS punkt śladu
static void histUpdate(const DroneInfo &d, const Frame &fr) {
  if (!d.id[0] && !d.hasLoc) return;  // sam MAC bez danych - nie zaśmiecamy historii
  char key[21];
  histKey(d, key);
  // dron bez ID dostał później Basic ID - przenosimy wpis z klucza MAC
  int s = -1;
  if (d.id[0]) {
    char mk[21];
    snprintf(mk, sizeof(mk), "mac:%02x%02x%02x%02x%02x%02x", d.macs[0][0], d.macs[0][1], d.macs[0][2],
             d.macs[0][3], d.macs[0][4], d.macs[0][5]);
    for (int i = 0; i < HIST_MAX; i++)
      if (hist[i].used && !strcmp(hist[i].id, mk)) {
        bool exists = false;
        for (int j = 0; j < HIST_MAX; j++) exists |= hist[j].used && !strcmp(hist[j].id, key);
        if (!exists) { strlcpy(hist[i].id, key, sizeof(hist[i].id)); s = i; }
        else hist[i].used = 0;
      }
  }
  if (s < 0) s = histSlot(key);
  HistEntry &h = hist[s];
  h.lastBoot = bootId;
  h.lastUp = millis() / 1000;
  h.frames++;
  h.transports |= 1 << fr.transport;
  if (fr.rssi > h.bestRssi) h.bestRssi = fr.rssi;
  if (!isnan(d.height) && d.height > h.maxH) h.maxH = d.height;
  if (d.hasLoc) { h.hasLoc = 1; h.lat = d.lat; h.lon = d.lon; }
  if (d.hasOp) { h.hasOp = 1; h.opLat = d.opLat; h.opLon = d.opLon; }
  histDirty = true;
  uint32_t now = millis();
  if (d.hasLoc && (!histLastTrackMs[s] || now - histLastTrackMs[s] >= HIST_TRACK_MS)) {
    histLastTrackMs[s] = now;
    histTrackAppend(h, d);
  }
}

static uint32_t bootBase(uint16_t boot) {
  if (boot == bootId && unixBase) return unixBase;
  char key[6];
  bootKey(key, boot);
  BootTime bt;
  if (prefs.isKey(key) && prefs.getBytes(key, &bt, sizeof(bt)) == sizeof(bt) && bt.boot == boot) return bt.base;
  return 0;
}

static uint32_t histTime(uint16_t boot, uint32_t up) {
  uint32_t b = bootBase(boot);
  return b ? b + up : 0;  // 0 = czas nieznany
}

static void histTick() {
  // czas z Remote ID drona (System) zapamiętujemy dla tego uruchomienia - przyda się później
  if (unixBase && !histBootTimeSaved) {
    histBootTimeSaved = true;
    setBootTime(unixNow());
  }
  if (histDirty && millis() - histLastSave > HIST_SAVE_MS) {
    histLastSave = millis();
    histDirty = false;
    histSave();
  }
}

static void histClear() {
  memset(hist, 0, sizeof(hist));
  memset(histLastTrackMs, 0, sizeof(histLastTrackMs));
  if (!fsOk) return;
  LittleFS.remove(HIST_FILE);
  LittleFS.remove(TRK_FILE);
  LittleFS.remove(TRK_OLD);
}

static int histCount() {
  int n = 0;
  for (auto &h : hist) n += h.used;
  return n;
}

static void histEntryJson(const HistEntry &h, char *out, size_t size) {
  size_t n = snprintf(out, size, "{\"h\":{\"id\":\"%s\",\"hk\":%u,\"first\":%u,\"last\":%u,\"n\":%u,\"rssi\":%d,\"tr\":%u",
                      h.id, djb2(h.id), histTime(h.firstBoot, h.firstUp), histTime(h.lastBoot, h.lastUp), h.frames,
                      h.bestRssi, h.transports);
#define HJ_ADD(...) \
  if (n < size) n += snprintf(out + n, size - n, __VA_ARGS__)
  uint8_t tag = tagStatus(h.id);
  if (tag) { HJ_ADD(",\"tag\":\"%s\"", tag == TAG_OWN ? "own" : "foreign"); }
  if (h.maxH > 0) { HJ_ADD(",\"maxh\":%.0f", h.maxH); }
  if (h.hasLoc) { HJ_ADD(",\"lat\":%.7f,\"lon\":%.7f", h.lat * 1e-7, h.lon * 1e-7); }
  if (h.hasOp) { HJ_ADD(",\"olat\":%.7f,\"olon\":%.7f", h.opLat * 1e-7, h.opLon * 1e-7); }
  HJ_ADD("}}\n");
#undef HJ_ADD
}
