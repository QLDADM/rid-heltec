// Podgląd na telefonie przez BLE (Nordic UART Service).
// Strona phone/index.html łączy się przez Web Bluetooth (Chrome na Androidzie, Bluefy na iPhonie)
// i dostaje linie JSON zakończone \n:
//   {"d":{...}}     - dron w zasięgu (co 1 s)
//   {"s":{...}}     - stan płytki (co 5 s)
//   {"tags":{...}}  - oznaczenia swój/obcy (po połączeniu i po zmianie)
//   {"h":{...}}     - historia: dron z pamięci (po HIST), {"hp":[[hk,t,lat,lon,alt],...]} - ślad,
//   {"hend":n}      - koniec historii
// Polecenia z telefonu: TAG <id> own|foreign|-, CH <n>, HIST, HISTCLR.
// Zabezpieczenie: parowanie z PIN-em (6 cyfr, losowany przy pierwszym starcie, w NVS). Telefon
// wpisuje go w systemowym oknie parowania; PIN widać na OLED w trakcie parowania i na stronie
// RID Decoder. Bez parowania płytka nic nie wysyła i nie przyjmuje poleceń. Sparowane telefony
// (do CONFIG_BT_NIMBLE_MAX_BONDS) łączą się potem bez PIN-u; BONDCLR przez USB je kasuje.
// Oznaczenia z telefonu trafiają też na listę TAGPEND, którą agent przenosi na serwer.
// Dołączany z main.cpp po mesh.h; w wersji bez BLE (RID_NO_BLE) pusty.
#pragma once

#ifndef RID_NO_BLE

#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define PHONE_DRONE_MS 1000
#define PHONE_STATUS_MS 5000
#define PHONE_DRONE_MAX_AGE_MS 60000  // telefon dostaje drony widziane w ostatniej minucie

static NimBLEServer *bleServer = nullptr;
static NimBLECharacteristic *bleTx = nullptr;
static volatile bool phoneConnected = false, phoneSubscribed = false, phoneWantTags = false;
static volatile bool phoneSecure = false;  // połączenie zaszyfrowane po parowaniu z PIN-em
static uint32_t phonePin = 0;
static volatile uint16_t phoneMtu = 23;
static char phoneCmd[64];
static volatile bool phoneCmdReady = false;
static char phoneName[20];

// oznaczenia zmienione na telefonie, czekające na przeniesienie na serwer (status 0 = usunięte)
#define MAX_TAG_PENDING 16
static TagEntry tagPending[MAX_TAG_PENDING];
static uint8_t tagPendingCount = 0;

class PhoneServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, ble_gap_conn_desc *desc) override {
    phoneConnected = true;
    phoneSecure = desc->sec_state.encrypted && desc->sec_state.authenticated;
    phoneMtu = 23;
  }
  void onDisconnect(NimBLEServer *) override {
    phoneConnected = false;
    phoneSubscribed = false;
    phoneSecure = false;
  }
  void onMTUChange(uint16_t mtu, ble_gap_conn_desc *) override { phoneMtu = mtu; }
  uint32_t onPassKeyRequest() override { return phonePin; }
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    // tylko parowanie z PIN-em (MITM) - zwykłe "Just Works" nie wystarcza
    phoneSecure = desc->sec_state.encrypted && desc->sec_state.authenticated;
    if (!phoneSecure) NimBLEDevice::getServer()->disconnect(desc->conn_handle);
    else phoneWantTags = true;
  }
};

class PhoneRxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    if (phoneCmdReady) return;  // poprzednie polecenie jeszcze nieprzetworzone
    std::string v = c->getValue();
    size_t n = min(v.size(), sizeof(phoneCmd) - 1);
    memcpy(phoneCmd, v.data(), n);
    while (n && (phoneCmd[n - 1] == '\n' || phoneCmd[n - 1] == '\r')) n--;
    phoneCmd[n] = 0;
    phoneCmdReady = true;
  }
};

class PhoneTxCb : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *, ble_gap_conn_desc *, uint16_t sub) override {
    phoneSubscribed = sub != 0;
    if (sub) phoneWantTags = true;
  }
};

static void phoneAdvertise() {
  NimBLEExtAdvertisement adv;
  adv.setLegacyAdvertising(true);  // zwykła reklama - widzą ją wszystkie telefony
  adv.setConnectable(true);
  adv.setScannable(true);
  adv.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  adv.setName(phoneName);
  adv.setMinInterval(800);  // 500-625 ms: mało czasu radia zabranego nasłuchowi
  adv.setMaxInterval(1000);
  NimBLEExtAdvertisement resp;
  resp.setLegacyAdvertising(true);
  resp.setCompleteServices(NimBLEUUID(NUS_SERVICE));
  NimBLEExtAdvertising *a = NimBLEDevice::getAdvertising();
  a->setInstanceData(0, adv);
  a->setScanResponseData(0, resp);
  a->start(0);
}

static void phoneBegin() {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  snprintf(phoneName, sizeof(phoneName), "RID-Heltec-%02X%02X", mac[4], mac[5]);
  phonePin = prefs.getUInt("pin", 0);
  if (phonePin < 100000 || phonePin > 999999) {  // pierwszy start: losowy PIN
    phonePin = 100000 + esp_random() % 900000;
    prefs.putUInt("pin", phonePin);
  }
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding, MITM (PIN), Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(phonePin);
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new PhoneServerCb());
  NimBLEService *svc = bleServer->createService(NUS_SERVICE);
  // dostęp tylko po parowaniu z PIN-em: subskrypcja i zapis wymagają szyfrowania z MITM
  bleTx = svc->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ |
                                                NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);
  bleTx->setCallbacks(new PhoneTxCb());
  NimBLECharacteristic *rx = svc->createCharacteristic(
      NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
  rx->setCallbacks(new PhoneRxCb());
  svc->start();
  // przy rozszerzonych reklamach NimBLE nie startuje serwera GATT sam - bez tego telefon
  // łączy się, ale nie widzi żadnych usług
  bleServer->start();
  // błąd NimBLE 1.4.3: konstruktor NimBLEExtAdvertising nie ustawia m_pCallbacks, a przy
  // połączeniu telefonu (koniec reklamy) biblioteka je wywołuje -> panika i restart płytki
  NimBLEDevice::getAdvertising()->setCallbacks(nullptr);
  phoneAdvertise();
}

static void phoneSend(const char *line) {
  size_t len = strlen(line), chunk = phoneMtu > 3 ? phoneMtu - 3 : 20;
  for (size_t i = 0; i < len && phoneSubscribed; i += chunk) {
    size_t n = min(chunk, len - i);
    bleTx->setValue((const uint8_t *)line + i, n);
    bleTx->notify();
    // NimBLE 1.4 nie zgłasza przepełnienia kolejki powiadomień - bez przerwy fragmenty
    // długich linii (historia) po cichu znikały; 8 ms na fragment = do ~30 kB/s
    delay(8);
  }
}

static void tagRecordPending(const char *id, uint8_t st) {
  for (int i = 0; i < tagPendingCount; i++)
    if (!strcmp(tagPending[i].id, id)) {
      tagPending[i].status = st;
      return;
    }
  if (tagPendingCount < MAX_TAG_PENDING) {
    strlcpy(tagPending[tagPendingCount].id, id, sizeof(tagPending[0].id));
    tagPending[tagPendingCount++].status = st;
  }
}

static void tagsJson(char *out, size_t size, bool pending) {
  size_t n = snprintf(out, size, pending ? "{\"tagpend\":{" : "{\"tags\":{");
  bool first = true;
  const TagEntry *list = pending ? tagPending : tags;
  int count = pending ? tagPendingCount : MAX_TAGS;
  for (int i = 0; i < count && n < size; i++) {
    if (!pending && !list[i].status) continue;
    const char *st = list[i].status == TAG_OWN ? "own" : list[i].status == TAG_FOREIGN ? "foreign" : "-";
    n += snprintf(out + n, size - n, "%s\"%s\":\"%s\"", first ? "" : ",", list[i].id, st);
    first = false;
  }
  if (n < size) snprintf(out + n, size - n, "}}\n");
}

static void phoneSendDrone(const DroneInfo &d, uint32_t now) {
  char line[300];
  size_t n = snprintf(line, sizeof(line), "{\"d\":{\"id\":\"%s\",\"mac\":\"%02x%02x%02x%02x%02x%02x\",\"rssi\":%d,\"age\":%u",
                      d.id, d.macs[0][0], d.macs[0][1], d.macs[0][2], d.macs[0][3], d.macs[0][4], d.macs[0][5],
                      d.rssi, (now - d.lastMs) / 1000);
#define PH_ADD(...) \
  if (n < sizeof(line)) n += snprintf(line + n, sizeof(line) - n, __VA_ARGS__)
  uint8_t tag = tagStatus(d.id);
  if (tag) { PH_ADD(",\"tag\":\"%s\"", tag == TAG_OWN ? "own" : "foreign"); }
  if (d.hasLoc) { PH_ADD(",\"lat\":%.7f,\"lon\":%.7f,\"st\":%u", d.lat * 1e-7, d.lon * 1e-7, d.status); }
  if (!isnan(d.height)) { PH_ADD(",\"h\":%.1f", d.height); }
  if (!isnan(d.altGeo)) { PH_ADD(",\"alt\":%.1f", d.altGeo); }
  if (!isnan(d.speed)) { PH_ADD(",\"spd\":%.1f", d.speed); }
  if (d.track >= 0) { PH_ADD(",\"trk\":%d", d.track); }
  if (d.hasOp) { PH_ADD(",\"olat\":%.7f,\"olon\":%.7f", d.opLat * 1e-7, d.opLon * 1e-7); }
  PH_ADD("}}\n");
#undef PH_ADD
  phoneSend(line);
}

// ---- wysyłka historii: kilka linii na obieg pętli, żeby nasłuch i dane na żywo działały dalej
static uint8_t histStage = 0;  // 0 brak, 1 zestawienie, 2 ślad (starszy plik), 3 ślad (bieżący)
static int histIdx = 0;
static File histFile;
static uint32_t histSent = 0;

static void histStreamStart() {
  if (histFile) histFile.close();
  histStage = 1;
  histIdx = 0;
  histSent = 0;
}

static bool histOpenTrack(const char *path) {
  if (!fsOk || !LittleFS.exists(path)) return false;
  histFile = LittleFS.open(path, FILE_READ);
  return (bool)histFile;
}

static void histStreamStep() {
  char line[760];
  for (int k = 0; k < 2 && histStage; k++) {
    if (histStage == 1) {
      while (histIdx < HIST_MAX && !hist[histIdx].used) histIdx++;
      if (histIdx >= HIST_MAX) {
        histStage = 2;
        if (!histOpenTrack(TRK_OLD)) histFile = File();
        continue;
      }
      histEntryJson(hist[histIdx++], line, sizeof(line));
      phoneSend(line);
      histSent++;
      continue;
    }
    // punkty śladu: do 10 na linię
    size_t n = snprintf(line, sizeof(line), "{\"hp\":[");
    int pts = 0;
    TrackPoint p;
    while (pts < 10 && histFile && histFile.read((uint8_t *)&p, sizeof(p)) == sizeof(p)) {
      n += snprintf(line + n, sizeof(line) - n, "%s[%u,%u,%.6f,%.6f,%d]", pts ? "," : "", p.idHash,
                    histTime(p.boot, p.up), p.lat * 1e-7, p.lon * 1e-7, p.alt);
      pts++;
    }
    if (pts) {
      snprintf(line + n, sizeof(line) - n, "]}\n");
      phoneSend(line);
      continue;
    }
    if (histFile) histFile.close();
    if (histStage == 2) {
      histStage = 3;
      if (!histOpenTrack(TRK_FILE)) histFile = File();
      continue;
    }
    histStage = 0;
    snprintf(line, sizeof(line), "{\"hend\":%u}\n", histSent);
    phoneSend(line);
  }
}

static void phoneHandleCommand(const char *cmd) {
  if (!strncasecmp(cmd, "TIME ", 5)) {  // telefon zna dokładny czas - godziny w historii także bez PC
    uint32_t t = strtoul(cmd + 5, nullptr, 10);
    if (t > 1700000000UL) {
      setBootTime(t);
      unixBase = t - millis() / 1000;
    }
    return;
  }
  if (!strcasecmp(cmd, "HIST")) {
    histStreamStart();
    return;
  }
  if (!strcasecmp(cmd, "HISTCLR")) {
    histStage = 0;
    if (histFile) histFile.close();
    histClear();
    phoneSend("{\"hclr\":1}\n");
    return;
  }
  if (!strncasecmp(cmd, "TAG ", 4)) {
    char buf[64];
    strlcpy(buf, cmd + 4, sizeof(buf));
    char *st = strchr(buf, ' ');
    if (!st) return;
    *st++ = 0;
    uint8_t s = !strcasecmp(st, "own") ? TAG_OWN : !strcasecmp(st, "foreign") ? TAG_FOREIGN : TAG_NONE;
    tagSet(buf, s);
    tagsSave();
    tagRecordPending(buf, s);
    phoneWantTags = true;
  } else if (!strncasecmp(cmd, "CH ", 3)) {
    int ch = atoi(cmd + 3);
    if (ch >= 0 && ch <= 13) fixedChannel = ch;
  }
}

static void phoneTick() {
  static uint32_t lastDrones = 0, lastStatus = 0, lastAdvCheck = 0;
  uint32_t now = millis();
  if (!bleServer) return;
  if (phoneCmdReady) {
    if (phoneSecure) phoneHandleCommand(phoneCmd);
    phoneCmdReady = false;
  }
  if (now - lastAdvCheck > 2000) {  // przy rozszerzonych reklamach NimBLE nie wznawia ich sam
    lastAdvCheck = now;
    if (!bleServer->getConnectedCount() && !NimBLEDevice::getAdvertising()->isAdvertising()) phoneAdvertise();
  }
  if (!phoneSubscribed || !phoneSecure) {
    if (histStage) {  // telefon się rozłączył w trakcie wysyłki historii
      histStage = 0;
      if (histFile) histFile.close();
    }
    return;
  }
  if (histStage) histStreamStep();
  if (phoneWantTags) {
    phoneWantTags = false;
    char buf[1200];
    tagsJson(buf, sizeof(buf), false);
    phoneSend(buf);
  }
  if (now - lastDrones > PHONE_DRONE_MS) {
    lastDrones = now;
    for (auto &d : drones)
      if (d.used && now - d.lastMs < PHONE_DRONE_MAX_AGE_MS) phoneSendDrone(d, now);
  }
  if (now - lastStatus > PHONE_STATUS_MS) {
    lastStatus = now;
    char line[200];
    snprintf(line, sizeof(line),
             "{\"s\":{\"fw\":\"1.6\",\"bat\":%d,\"bmv\":%u,\"mem\":%u,\"ch\":%u,\"fixed\":%s,\"pc\":%s,"
             "\"mesh\":%s,\"up\":%u,\"pend\":%u,\"hist\":%d}}\n",
             batPct, batMv, logCountCur + logCountOld, currentChannel, fixedChannel ? "true" : "false",
             hostConnected() ? "true" : "false", meshRadioOk ? "true" : "false", millis() / 1000, tagPendingCount,
             histCount());
    phoneSend(line);
  }
}

static int phoneBonds() { return NimBLEDevice::getNumBonds(); }

static void phoneSetPin(uint32_t pin) {
  if (pin < 100000 || pin > 999999) return;
  phonePin = pin;
  prefs.putUInt("pin", pin);
  NimBLEDevice::setSecurityPasskey(pin);
}

#else
static const bool phoneConnected = false, phoneSecure = false;
static bool phoneWantTags = false;
static void tagRecordPending(const char *, uint8_t) {}
static uint32_t phonePin = 0;
static void phoneSetPin(uint32_t) {}
static int phoneBonds() { return 0; }
static void phoneBegin() {}
static void phoneTick() {}
static void tagsJson(char *out, size_t size, bool) { snprintf(out, size, "{\"tagpend\":{}}\n"); }
static uint8_t tagPendingCount = 0;
#endif
