# RID Heltec - podgląd na telefonie

Strona łączy się przez Bluetooth z czujnikiem Remote ID na płytce Heltec WiFi LoRa 32 V3
(firmware RID 1.4+) i pokazuje drony w zasięgu: odległość i kierunek od telefonu, radar,
wysokość, prędkość, operatora oraz oznaczenia swój/obcy.

- Android: Chrome. iPhone: przeglądarka Bluefy (Safari nie obsługuje Bluetooth na stronach).
- Przy pierwszym połączeniu telefon pyta o PIN, który widać na ekranie Heltec.
- Strona nie zawiera ani nie wysyła żadnych danych. Wszystko idzie tylko między telefonem a płytką.

Źródło: projekt rid-decoder (`server/app/static/phone/`). Tę kopię aktualizuje się przez skopiowanie plików i push.
