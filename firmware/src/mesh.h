// Alarmy o dronach przez LoRa w formacie Meshtastic (SX1262 na Heltec V3).
// Płytka jest tylko nadawcą: wysyła NodeInfo („RID Heltec”), wiadomości tekstowe i punkty
// (waypoint) na kanale skonfigurowanym poleceniem MESHCFG. Aplikacje Meshtastic drużyny
// pokazują je jak zwykłe wiadomości i punkty na mapie; węzły przekazują je dalej (hop limit 3).
// Dołączany z main.cpp po drones.h.
#pragma once

#include <RadioLib.h>
#include <SPI.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <esp_mac.h>

// ---- piny SX1262 na Heltec V3
#define LORA_NSS 8
#define LORA_SCK 9
#define LORA_MOSI 10
#define LORA_MISO 11
#define LORA_RST 12
#define LORA_BUSY 13
#define LORA_DIO1 14

#define MESH_PORT_TEXT 1
#define MESH_PORT_NODEINFO 4
#define MESH_PORT_WAYPOINT 8
#define MESH_HW_HELTEC_V3 43
#define MESH_NODEINFO_MS (3UL * 3600 * 1000)
#define MESH_WAYPOINT_MS 60000UL
#define MESH_REALERT_MS (5UL * 60 * 1000)  // dron, którego nie było 5 min, alarmuje ponownie
#define MESH_DUTY 10                        // % czasu nadawania (EU 869,4-869,65 MHz)

struct MeshPreset {
  const char *id, *name;  // name - do skrótu kanału, gdy kanał nie ma nazwy
  float bw;
  uint8_t sf, cr;
};
static const MeshPreset MESH_PRESETS[] = {
    {"LONG_FAST", "LongFast", 250, 11, 5},     {"LONG_SLOW", "LongSlow", 125, 12, 8},
    {"LONG_MODERATE", "LongMod", 125, 11, 8},  {"MEDIUM_FAST", "MediumFast", 250, 9, 5},
    {"MEDIUM_SLOW", "MediumSlow", 250, 10, 5}, {"SHORT_FAST", "ShortFast", 250, 7, 5},
    {"SHORT_SLOW", "ShortSlow", 250, 8, 5},    {"SHORT_TURBO", "ShortTurbo", 500, 7, 5},
};
static const uint8_t MESH_DEFAULT_PSK[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                             0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

struct MeshCfg {
  bool on;
  char channel[12];  // pusty = kanał domyślny presetu
  char pskB64[48];   // jak w aplikacji Meshtastic; "AQ==" = klucz domyślny
  char preset[16];
  char region[8];    // EU_868 | EU_433
  float freq;        // >0 = ręcznie ustawiona częstotliwość MHz
  int8_t power;      // dBm
};

static MeshCfg meshCfg = {false, "", "AQ==", "LONG_FAST", "EU_868", 0, 20};
static SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
static bool meshRadioOk = false;
static float meshFreq = 0;
static uint8_t meshKey[32];
static uint8_t meshKeyLen = 0;  // 0 = bez szyfrowania
static uint8_t meshChanHash = 0;
static uint32_t meshNode = 0;
static uint32_t meshPacketId = 0;
static uint32_t meshTxCount = 0, meshDropped = 0;
static uint32_t meshLastNodeInfo = 0;
static volatile bool meshTxDone = false;
static bool meshTxBusy = false;
static uint32_t meshNextTxMs = 0;
static char meshError[40] = "";

struct MeshPacket {
  uint8_t len;
  uint8_t buf[255];
};
#define MESH_QUEUE 6
static MeshPacket meshQueue[MESH_QUEUE];
static uint8_t meshQHead = 0, meshQCount = 0;

static void IRAM_ATTR meshOnDio1() { meshTxDone = true; }

static const MeshPreset &meshPreset() {
  for (auto &p : MESH_PRESETS)
    if (strcasecmp(p.id, meshCfg.preset) == 0) return p;
  return MESH_PRESETS[0];
}

static uint32_t djb2(const char *s) {
  uint32_t h = 5381;
  while (*s) h += (h << 5) + (uint8_t)*s++;
  return h;
}

// ---- konfiguracja: "on=1;ch=OSP;psk=base64;preset=LONG_FAST;region=EU_868;pwr=20;freq=0"

static void meshParseCfg(const char *s) {
  char buf[200];
  strncpy(buf, s, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  for (char *tok = strtok(buf, ";"); tok; tok = strtok(nullptr, ";")) {
    char *eq = strchr(tok, '=');
    if (!eq) continue;
    *eq = 0;
    const char *k = tok, *v = eq + 1;
    if (!strcmp(k, "on")) meshCfg.on = atoi(v) != 0;
    else if (!strcmp(k, "ch")) strlcpy(meshCfg.channel, v, sizeof(meshCfg.channel));
    else if (!strcmp(k, "psk")) strlcpy(meshCfg.pskB64, v, sizeof(meshCfg.pskB64));
    else if (!strcmp(k, "preset")) strlcpy(meshCfg.preset, v, sizeof(meshCfg.preset));
    else if (!strcmp(k, "region")) strlcpy(meshCfg.region, v, sizeof(meshCfg.region));
    else if (!strcmp(k, "freq")) meshCfg.freq = atof(v);
    else if (!strcmp(k, "pwr")) meshCfg.power = constrain(atoi(v), -9, 22);
  }
}

static bool meshDeriveKey() {
  uint8_t raw[40];
  size_t n = 0;
  if (mbedtls_base64_decode(raw, sizeof(raw), &n, (const uint8_t *)meshCfg.pskB64, strlen(meshCfg.pskB64)) != 0)
    return false;
  if (n == 0 || (n == 1 && raw[0] == 0)) {
    meshKeyLen = 0;  // kanał bez szyfrowania
  } else if (n == 1) {  // skrócony klucz domyślny: AQ== = 1, kolejne indeksy zmieniają ostatni bajt
    memcpy(meshKey, MESH_DEFAULT_PSK, 16);
    meshKey[15] += raw[0] - 1;
    meshKeyLen = 16;
  } else if (n == 16 || n == 32) {
    memcpy(meshKey, raw, n);
    meshKeyLen = n;
  } else {
    return false;
  }
  return true;
}

static void meshBegin() {
  meshRadioOk = false;
  meshError[0] = 0;
  if (!meshDeriveKey()) {
    strlcpy(meshError, "zly klucz PSK", sizeof(meshError));
    return;
  }
  const MeshPreset &p = meshPreset();
  const char *chName = meshCfg.channel[0] ? meshCfg.channel : p.name;
  // skrót kanału: XOR bajtów nazwy i klucza (jak w Meshtastic)
  uint8_t h = 0;
  for (const char *c = chName; *c; c++) h ^= (uint8_t)*c;
  for (int i = 0; i < meshKeyLen; i++) h ^= meshKey[i];
  meshChanHash = h;
  if (meshCfg.freq > 0) {
    meshFreq = meshCfg.freq;
  } else {
    float start = 869.4f, end = 869.65f;
    if (!strcasecmp(meshCfg.region, "EU_433")) { start = 433.0f; end = 434.0f; }
    uint32_t channels = max(1, (int)floorf((end - start) / (p.bw / 1000.0f)));
    meshFreq = start + p.bw / 2000.0f + (djb2(chName) % channels) * (p.bw / 1000.0f);
  }
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  meshNode = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
  if (!meshPacketId) meshPacketId = esp_random();

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  int st = radio.begin(meshFreq, p.bw, p.sf, p.cr, 0x2B, meshCfg.power, 16, 1.8, false);
  if (st != RADIOLIB_ERR_NONE) {
    snprintf(meshError, sizeof(meshError), "radio blad %d", st);
    return;
  }
  radio.setDio2AsRfSwitch(true);
  radio.setCurrentLimit(140);
  radio.setCRC(true);
  radio.setDio1Action(meshOnDio1);
  radio.sleep();
  meshRadioOk = true;
  meshTxBusy = false;
  meshLastNodeInfo = 0;  // przedstawiamy się od razu
}

static void meshApplyCfg(const char *s, bool save) {
  meshParseCfg(s);
  if (save) prefs.putString("mesh", s);
  if (meshCfg.on) meshBegin();
  else if (meshRadioOk) {
    radio.sleep();
    meshRadioOk = false;
  }
}

// ---- protobuf (tylko to, czego potrzebujemy)

static size_t pbVarint(uint8_t *p, uint32_t v) {
  size_t n = 0;
  do {
    p[n] = v & 0x7F;
    v >>= 7;
    if (v) p[n] |= 0x80;
    n++;
  } while (v);
  return n;
}
static size_t pbTagVarint(uint8_t *p, uint8_t field, uint32_t v) {
  p[0] = field << 3;
  return 1 + pbVarint(p + 1, v);
}
static size_t pbTagBytes(uint8_t *p, uint8_t field, const void *data, size_t len) {
  p[0] = (field << 3) | 2;
  size_t n = 1 + pbVarint(p + 1, len);
  memcpy(p + n, data, len);
  return n + len;
}
static size_t pbTagFixed32(uint8_t *p, uint8_t field, uint32_t v) {
  p[0] = (field << 3) | 5;
  memcpy(p + 1, &v, 4);
  return 5;
}

// pakiet: nagłówek 16 B + zaszyfrowany Data{portnum, payload}
static void meshSend(uint32_t port, const uint8_t *payload, size_t len) {
  if (!meshRadioOk) return;
  uint8_t data[240];
  size_t n = pbTagVarint(data, 1, port);
  if (len > sizeof(data) - n - 3) len = sizeof(data) - n - 3;
  n += pbTagBytes(data + n, 2, payload, len);
  if (n > 255 - 16) return;

  if (meshQCount == MESH_QUEUE) {  // kolejka pełna - wypada najstarszy
    meshQHead = (meshQHead + 1) % MESH_QUEUE;
    meshQCount--;
    meshDropped++;
  }
  MeshPacket &pk = meshQueue[(meshQHead + meshQCount) % MESH_QUEUE];
  uint32_t id = ++meshPacketId;
  uint32_t to = 0xFFFFFFFF;
  memcpy(pk.buf, &to, 4);
  memcpy(pk.buf + 4, &meshNode, 4);
  memcpy(pk.buf + 8, &id, 4);
  pk.buf[12] = 3 | (3 << 5);  // hop_limit 3, hop_start 3
  pk.buf[13] = meshChanHash;
  pk.buf[14] = 0;             // next_hop
  pk.buf[15] = meshNode & 0xFF;  // relay_node
  if (meshKeyLen) {
    uint8_t nonce[16] = {0}, stream[16];
    memcpy(nonce, &id, 4);         // packet id jako uint64
    memcpy(nonce + 8, &meshNode, 4);
    size_t off = 0;
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, meshKey, meshKeyLen * 8);
    mbedtls_aes_crypt_ctr(&aes, n, &off, nonce, stream, data, pk.buf + 16);
    mbedtls_aes_free(&aes);
  } else {
    memcpy(pk.buf + 16, data, n);
  }
  pk.len = 16 + n;
  meshQCount++;
}

static void meshSendText(const char *text) { meshSend(MESH_PORT_TEXT, (const uint8_t *)text, strlen(text)); }

static void meshSendNodeInfo() {
  uint8_t u[80];
  char id[12];
  snprintf(id, sizeof(id), "!%08x", meshNode);
  size_t n = pbTagBytes(u, 1, id, strlen(id));
  n += pbTagBytes(u + n, 2, "RID Heltec", 10);
  n += pbTagBytes(u + n, 3, "RID", 3);
  n += pbTagVarint(u + n, 5, MESH_HW_HELTEC_V3);
  meshSend(MESH_PORT_NODEINFO, u, n);
}

static void meshSendWaypoint(const DroneInfo &d, const char *name, const char *desc) {
  uint8_t w[200];
  uint32_t wid = djb2(d.id[0] ? d.id : name) | 1;
  size_t n = pbTagVarint(w, 1, wid);
  n += pbTagFixed32(w + n, 2, (uint32_t)d.lat);
  n += pbTagFixed32(w + n, 3, (uint32_t)d.lon);
  uint32_t now = unixNow();
  if (now) n += pbTagVarint(w + n, 4, now + 15 * 60);  // punkt znika sam po 15 min
  n += pbTagBytes(w + n, 6, name, strnlen(name, 30));
  n += pbTagBytes(w + n, 7, desc, strnlen(desc, 100));
  n += pbTagFixed32(w + n, 8, 0x1F681);  // ikona 🚁
  meshSend(MESH_PORT_WAYPOINT, w, n);
}

// ---- alarmy: nowy dron nieoznaczony jako swój

static void meshDescribe(const DroneInfo &d, char *out, size_t size, bool longForm) {
  const char *kind = tagStatus(d.id) == TAG_FOREIGN ? "OBCY" : "nieoznaczony";
  if (d.wifiKind) {  // dron Wi-Fi bez Remote ID: bez pozycji, tylko nazwa sieci i siła sygnału
    snprintf(out, size, "DRON Wi-Fi %s %s (%s), kanal %u, %d dBm", kind, d.id, d.wifiKind, d.wifiCh, d.rssi);
    return;
  }
  size_t n = longForm ? snprintf(out, size, "DRON %s %s", kind, d.id[0] ? d.id : "bez ID")
                      : snprintf(out, size, "%s", kind);
// dopisuje kolejne pola, dopóki jest miejsce
#define MESH_ADD(...) \
  if (n < size) n += snprintf(out + n, size - n, __VA_ARGS__)
  if (!isnan(d.height)) { MESH_ADD(", wys %.0f m", d.height); }
  else if (!isnan(d.altGeo)) { MESH_ADD(", %.0f m npm", d.altGeo); }
  if (!isnan(d.speed)) { MESH_ADD(", %.0f m/s", d.speed); }
  if (longForm && d.hasLoc) { MESH_ADD("\n%.5f, %.5f", d.lat * 1e-7, d.lon * 1e-7); }
  if (d.hasOp && d.hasLoc) {
    MESH_ADD("%soperator %.0f m", longForm ? "\n" : ", ", distM(d.lat, d.lon, d.opLat, d.opLon));
  }
  if (longForm && d.hasOp) { MESH_ADD(" (%.5f, %.5f)", d.opLat * 1e-7, d.opLon * 1e-7); }
#undef MESH_ADD
}

static void meshCheckDrone(DroneInfo &d) {
  if (!meshRadioOk || tagStatus(d.id) == TAG_OWN) return;
  uint32_t now = millis();
  // dron bez ID: czekamy chwilę na Basic ID, żeby nie alarmować dwa razy
  if (!d.id[0] && now - d.firstMs < 10000) return;
  if (!d.alerted) {
    char text[200];
    meshDescribe(d, text, sizeof(text), true);
    meshSendText(text);
    d.alerted = true;
  }
  if (d.hasLoc && now - d.lastWaypointMs > MESH_WAYPOINT_MS) {
    char name[31], desc[101];
    snprintf(name, sizeof(name), "DRON %s", d.id[0] ? d.id + max(0, (int)strlen(d.id) - 8) : "?");
    meshDescribe(d, desc, sizeof(desc), false);
    meshSendWaypoint(d, name, desc);
    d.lastWaypointMs = now;
  }
}

static void meshTick() {
  if (!meshRadioOk) return;
  uint32_t now = millis();
  if (!meshLastNodeInfo || now - meshLastNodeInfo > MESH_NODEINFO_MS) {
    meshLastNodeInfo = now;
    meshSendNodeInfo();
  }
  for (auto &d : drones) {
    if (!d.used) continue;
    if (d.alerted && now - d.lastMs > MESH_REALERT_MS) d.alerted = false;
    if (now - d.lastMs < 5000) meshCheckDrone(d);
  }
  if (meshTxBusy) {
    if (!meshTxDone) return;
    meshTxDone = false;
    meshTxBusy = false;
    radio.finishTransmit();
    radio.sleep();
    meshTxCount++;
  }
  if (meshQCount && (int32_t)(now - meshNextTxMs) >= 0) {
    MeshPacket &pk = meshQueue[meshQHead];
    uint32_t toaMs = radio.getTimeOnAir(pk.len) / 1000;
    meshTxDone = false;
    if (radio.startTransmit(pk.buf, pk.len) == RADIOLIB_ERR_NONE) {
      meshTxBusy = true;
      // limit czasu nadawania: po pakiecie przerwa 9x dłuższa niż jego nadawanie
      meshNextTxMs = now + toaMs * (100 / MESH_DUTY);
    }
    meshQHead = (meshQHead + 1) % MESH_QUEUE;
    meshQCount--;
  }
}

static void meshPrintStatus(Print &out) {
  out.printf("{\"mesh\":{\"on\":%s,\"ok\":%s,\"freq\":%.3f,\"preset\":\"%s\",\"ch\":\"%s\",\"hash\":%u,"
             "\"node\":\"!%08x\",\"tx\":%u,\"queued\":%u,\"dropped\":%u,\"error\":\"%s\"}}\n",
             meshCfg.on ? "true" : "false", meshRadioOk ? "true" : "false", meshFreq, meshPreset().id,
             meshCfg.channel, meshChanHash, meshNode, meshTxCount, meshQCount, meshDropped, meshError);
}
