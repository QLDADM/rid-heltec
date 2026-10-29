// Przycisk PRG (GPIO0) i ekrany OLED - obsługa płytki bez telefonu i komputera.
//   krótko          - następny ekran: główny -> lista dronów -> szczegóły (kolejne drony) -> stan
//                     -> ustawienia ekranu (tu krótko = następna opcja, na "Dalej" -> główny)
//   przytrzymaj 2 s - szczegóły: oznaczenie drona (brak -> SWOJ -> OBCY -> brak)
//                     stan: kanał Wi-Fi (skakanie <-> stały 6)
//                     ustawienia: zmiana zaznaczonej opcji (jasność, odwrócenie, obrót; w NVS)
//   dwuklik         - ekran wł./wył.; gaśnie sam po SCREEN_OFF_MS, budzi go przycisk,
//                     nowy dron albo parowanie telefonu (nasłuch i zapis działają cały czas)
// Dołączany z main.cpp po phone.h, przed drawDisplay().
#pragma once

#define PIN_BTN 0
#define BTN_DEBOUNCE_MS 30
#define BTN_LONG_MS 2000
#define BTN_DOUBLE_MS 350
#define SCREEN_OFF_MS 120000UL
#define UI_DRONE_MAX_AGE_MS 60000

enum Screen : uint8_t { SCR_MAIN, SCR_LIST, SCR_DETAIL, SCR_STATUS, SCR_SETTINGS };
static Screen screen = SCR_MAIN;

// ustawienia wyświetlacza (OLED świeci jednym kolorem - zmienić da się jasność, odwrócenie, obrót)
static const uint8_t BRIGHTNESS[] = {255, 90, 8};
static const char *BRIGHTNESS_NAME[] = {"dzien", "wieczor", "noc"};
static uint8_t dispBright = 0, setCursor = 0;
static bool dispInvert = false, dispRotate = false;

static void uiApplyDisplay() {
  display.setBrightness(BRIGHTNESS[dispBright]);
  if (dispInvert) display.invertDisplay();
  else display.normalDisplay();
  if (dispRotate) display.resetOrientation();  // domyślnie ekran jest odwrócony (flipScreenVertically)
  else display.flipScreenVertically();
}

static void uiSaveDisplay() {
  prefs.putUChar("dbr", dispBright);
  prefs.putBool("dinv", dispInvert);
  prefs.putBool("drot", dispRotate);
}
static uint8_t detailIdx = 0;
static bool displayOn = true;
static uint32_t lastActivityMs = 0;
static char toast[32];
static uint32_t toastUntil = 0;

// drony widziane w ostatniej minucie, najświeższe pierwsze
static int activeDrones(DroneInfo **out) {
  uint32_t now = millis();
  int n = 0;
  for (auto &d : drones)
    if (d.used && now - d.lastMs < UI_DRONE_MAX_AGE_MS) out[n++] = &d;
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && out[j]->lastMs > out[j - 1]->lastMs; j--) {
      DroneInfo *t = out[j];
      out[j] = out[j - 1];
      out[j - 1] = t;
    }
  return n;
}

static void uiWake() {
  lastActivityMs = millis();
  if (!displayOn) {
    display.displayOn();
    displayOn = true;
  }
}

static void uiToast(const char *t) {
  strlcpy(toast, t, sizeof(toast));
  toastUntil = millis() + 1500;
}

static void uiShortPress() {
  if (!displayOn) {
    uiWake();
    return;
  }
  DroneInfo *list[MAX_DRONES];
  int n = activeDrones(list);
  switch (screen) {
    case SCR_MAIN: screen = SCR_LIST; break;
    case SCR_LIST:
      detailIdx = 0;
      screen = n ? SCR_DETAIL : SCR_STATUS;
      break;
    case SCR_DETAIL:
      if (++detailIdx >= n) screen = SCR_STATUS;
      break;
    case SCR_STATUS:
      screen = SCR_SETTINGS;
      setCursor = 0;
      break;
    case SCR_SETTINGS:  // krótko: następna opcja; z "Dalej" wracamy na ekran główny
      if (++setCursor > 3) screen = SCR_MAIN;
      break;
  }
}

static void uiLongPress() {
  if (!displayOn) {
    uiWake();
    return;
  }
  if (screen == SCR_DETAIL) {
    DroneInfo *list[MAX_DRONES];
    int n = activeDrones(list);
    if (detailIdx >= n || !list[detailIdx]->id[0]) {
      uiToast("Dron bez ID - bez oznaczen");
      return;
    }
    const char *id = list[detailIdx]->id;
    uint8_t st = tagStatus(id);
    st = st == TAG_NONE ? TAG_OWN : st == TAG_OWN ? TAG_FOREIGN : TAG_NONE;
    tagSet(id, st);
    tagsSave();
    tagRecordPending(id, st);  // trafi na stronę RID Decoder przy podłączeniu do komputera
    phoneWantTags = true;      // i od razu do połączonego telefonu
    uiToast(st == TAG_OWN ? "Oznaczono: SWOJ" : st == TAG_FOREIGN ? "Oznaczono: OBCY" : "Usunieto oznaczenie");
  } else if (screen == SCR_SETTINGS) {
    if (setCursor == 0) {
      dispBright = (dispBright + 1) % 3;
      uiToast((String("Jasnosc: ") + BRIGHTNESS_NAME[dispBright]).c_str());
    } else if (setCursor == 1) {
      dispInvert = !dispInvert;
      uiToast(dispInvert ? "Kolory odwrocone" : "Kolory normalne");
    } else if (setCursor == 2) {
      dispRotate = !dispRotate;
      uiToast(dispRotate ? "Ekran obrocony" : "Ekran normalnie");
    } else {
      screen = SCR_MAIN;
      return;
    }
    uiApplyDisplay();
    uiSaveDisplay();
  } else if (screen == SCR_STATUS) {
    fixedChannel = fixedChannel ? 0 : 6;
    uiToast(fixedChannel ? "Staly kanal 6" : "Skakanie po kanalach");
  }
}

static void uiDoubleClick() {
  if (displayOn) {
    display.displayOff();
    displayOn = false;
  } else {
    uiWake();
  }
}

static void uiBegin() {
  pinMode(PIN_BTN, INPUT_PULLUP);
  lastActivityMs = millis();
  dispBright = min<uint8_t>(prefs.getUChar("dbr", 0), 2);
  dispInvert = prefs.getBool("dinv", false);
  dispRotate = prefs.getBool("drot", false);
  uiApplyDisplay();
}

static void uiButtonTick() {
  static bool down = false, longFired = false, pendingShort = false;
  static uint32_t downMs = 0, upMs = 0;
  bool pressed = digitalRead(PIN_BTN) == LOW;
  uint32_t now = millis();
  if (pressed && !down) {
    down = true;
    downMs = now;
    longFired = false;
  }
  if (pressed && down && !longFired && now - downMs >= BTN_LONG_MS) {
    longFired = true;
    pendingShort = false;
    lastActivityMs = now;
    uiLongPress();
  }
  if (!pressed && down) {
    down = false;
    if (!longFired && now - downMs > BTN_DEBOUNCE_MS) {
      if (pendingShort && now - upMs < BTN_DOUBLE_MS) {
        pendingShort = false;
        lastActivityMs = now;
        uiDoubleClick();
      } else {
        pendingShort = true;  // czekamy chwilę, czy to nie dwuklik
        upMs = now;
      }
    }
  }
  if (pendingShort && !down && now - upMs >= BTN_DOUBLE_MS) {
    pendingShort = false;
    lastActivityMs = now;
    uiShortPress();
  }
  if (phoneConnected && !phoneSecure) uiWake();  // PIN do parowania musi być widoczny
  if (displayOn && now - lastActivityMs > SCREEN_OFF_MS) {
    display.displayOff();
    displayOn = false;
  }
}

// ---------------------------------------------------------------- ekrany

static String shortId(const DroneInfo &d, int maxLen) {
  if (!d.id[0]) return "bez ID";
  int len = strlen(d.id);
  return String(d.id + max(0, len - maxLen));
}

static void drawList() {
  DroneInfo *list[MAX_DRONES];
  int n = activeDrones(list);
  display.drawString(0, 0, "DRONY: " + String(n));
  drawBattery();
  if (!n) {
    display.drawString(0, 28, "brak dronow w zasiegu");
    return;
  }
  for (int i = 0; i < n && i < 4; i++) {
    const DroneInfo &d = *list[i];
    uint8_t t = tagStatus(d.id);
    int y = 13 + i * 12;
    display.drawString(0, y, String(t == TAG_OWN ? "S " : t == TAG_FOREIGN ? "O " : "? ") + shortId(d, 9));
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(127, y, (d.wifiKind ? String("WiFi") : isnan(d.height) ? String("-") : String((int)d.height) + "m") +
                                   " " + String(d.rssi));
    display.setTextAlignment(TEXT_ALIGN_LEFT);
  }
}

static void drawDetail() {
  DroneInfo *list[MAX_DRONES];
  int n = activeDrones(list);
  if (detailIdx >= n) {  // dron zniknął z zasięgu w trakcie oglądania
    screen = n ? SCR_DETAIL : SCR_LIST;
    detailIdx = 0;
    if (!n) {
      drawList();
      return;
    }
  }
  const DroneInfo &d = *list[detailIdx];
  uint8_t t = tagStatus(d.id);
  display.drawString(0, 0, "DRON " + String(detailIdx + 1) + "/" + String(n));
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(127, 0, t ? tagText(t) : "nieoznaczony");
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 13, shortId(d, 20));
  if (d.wifiKind) {  // dron Wi-Fi bez Remote ID
    display.drawString(0, 25, String("Wi-Fi: ") + d.wifiKind);
    display.drawString(0, 37, String(d.rssi) + "dBm  kanal " + String(d.wifiCh) + "  " +
                                  String((millis() - d.lastMs) / 1000) + "s");
    display.drawString(0, 49, "brak Remote ID");
    return;
  }
  String l2 = "wys " + (isnan(d.height) ? String("-") : String((int)d.height) + "m");
  if (!isnan(d.speed)) l2 += "  " + String(d.speed, 0) + "m/s";
  if (d.track >= 0) l2 += "  " + String(d.track) + "st";
  display.drawString(0, 25, l2);
  uint32_t age = (millis() - d.lastMs) / 1000;
  String l3 = (d.hasOp && d.hasLoc) ? "op " + String((int)distM(d.lat, d.lon, d.opLat, d.opLon)) + "m  " : String("");
  display.drawString(0, 37, l3 + String(d.rssi) + "dBm  " + String(age) + "s");
  display.drawString(0, 49, d.hasLoc ? String(d.lat * 1e-7, 5) + " " + String(d.lon * 1e-7, 5) : String("brak pozycji"));
}

static void drawStatus() {
  display.drawString(0, 0, String("STAN  Mesh: ") + (meshRadioOk ? "nadaje" : "wyl"));
  drawBattery();
  display.drawString(0, 13, batPct >= 0 ? "Bateria " + String(batPct) + "%  " + String(batMv / 1000.0, 2) + "V"
                                         : String("Bateria: brak (USB)"));
  display.drawString(0, 25, "Zapis " + String(logCountCur + logCountOld) + "  Historia " + String(histCount()));
  display.drawString(0, 37, "PIN " + String(phonePin) + "  Tel: " +
                                (phoneConnected ? (phoneSecure ? "polaczony" : "parowanie") : "-"));
  // stan kanału wprost słowami - sama gwiazdka przy numerze była niezauważalna
  display.drawString(0, 49, fixedChannel ? "Wi-Fi: staly kanal " + String(fixedChannel) : String("Wi-Fi: skakanie"));
}

static void drawSettings() {
  display.drawString(0, 0, "USTAWIENIA EKRANU");
  const String items[] = {
      String("Jasnosc: ") + BRIGHTNESS_NAME[dispBright],
      String("Odwrocenie: ") + (dispInvert ? "tak" : "nie"),
      String("Obrot: ") + (dispRotate ? "180 st." : "normalny"),
      "Dalej ->",
  };
  for (int i = 0; i < 4; i++) display.drawString(0, 13 + i * 12, String(i == setCursor ? "> " : "  ") + items[i]);
}

static void drawToast() {
  if ((int32_t)(millis() - toastUntil) >= 0) return;
  display.setColor(BLACK);
  display.fillRect(0, 50, 128, 14);
  display.setColor(WHITE);
  display.drawRect(0, 50, 128, 14);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 51, toast);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
}
