// Wyjście do agenta PC. Wszystko, co firmware pisze do Serial, idzie na USB, a gdy agent połączył
// się przez Bluetooth (kanał PC w phone.h) - także do bufora BLE. Polecenia od agenta przez BLE
// trafiają do tego samego odczytu co z USB, więc agent obsługuje płytkę tak samo jak po kablu.
// Dołączany z main.cpp przed pierwszym użyciem Serial; od tego miejsca Serial = hostOut.
#pragma once
#include <Arduino.h>

#define HOST_BLE_OUT 8192
#define HOST_BLE_IN 512
static volatile bool hostBleOn = false;  // agent zasubskrybował kanał PC przez BLE
static uint8_t hostBleOut[HOST_BLE_OUT];
static volatile size_t hostBleOutHead = 0, hostBleOutTail = 0;  // pisze pętla, wysyła hostBleDrain
static char hostBleIn[HOST_BLE_IN];
static volatile size_t hostBleInHead = 0, hostBleInTail = 0;  // pisze callback BLE, czyta pętla
static void hostBleDrain();  // phone.h: wysyła kawałek bufora przez BLE

static void hostBleReset() {
  hostBleOutHead = hostBleOutTail = 0;
  hostBleInHead = hostBleInTail = 0;
}

// wywoływane z callbacku zapisu BLE (zadanie NimBLE)
static void hostBleReceive(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    size_t next = (hostBleInHead + 1) % HOST_BLE_IN;
    if (next == hostBleInTail) return;  // pełny - reszta przepada
    hostBleIn[hostBleInHead] = p[i];
    hostBleInHead = next;
  }
}

class HostOut : public Print {
 public:
  void begin(unsigned long baud) { Serial.begin(baud); }
  int available() {
    return Serial.available() + (int)((hostBleInHead + HOST_BLE_IN - hostBleInTail) % HOST_BLE_IN);
  }
  int read() {
    if (Serial.available()) return Serial.read();
    if (hostBleInTail == hostBleInHead) return -1;
    char c = hostBleIn[hostBleInTail];
    hostBleInTail = (hostBleInTail + 1) % HOST_BLE_IN;
    return c;
  }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *p, size_t n) override {
    Serial.write(p, n);
    for (size_t i = 0; i < n && hostBleOn; i++) {
      size_t next = (hostBleOutHead + 1) % HOST_BLE_OUT;
      while (next == hostBleOutTail && hostBleOn) hostBleDrain();  // pełny bufor (np. DUMP): czekamy na wysyłkę
      if (!hostBleOn) break;
      hostBleOut[hostBleOutHead] = p[i];
      hostBleOutHead = next;
    }
    return n;
  }
};
static HostOut hostOut;
#define Serial hostOut
