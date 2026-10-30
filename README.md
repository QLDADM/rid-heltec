# RID Heltec: wykrywacz dronów Remote ID na płytce Heltec V3 z podglądem na telefonie

Płytka **Heltec WiFi LoRa 32 V3** odbiera sygnał Remote ID, który nadają drony.
Telefon łączy się z nią przez Bluetooth i pokazuje drony w okolicy: odległość i kierunek od Ciebie,
radar, mapę, wysokość, prędkość, położenie operatora oraz oznaczenia swój/obcy.

## ▶ Otwórz podgląd na telefonie: **https://qldadm.github.io/rid-heltec/**

Nie trzeba niczego instalować ani zakładać konta. Strona działa w przeglądarce telefonu.

---

## Jak zacząć: 3 kroki

### 1. Wgraj firmware na Heltec (jednorazowo)

Pobierz plik `rid-heltec-v3-full.bin` z zakładki **[Releases](https://github.com/QLDADM/rid-heltec/releases)**
i wgraj go przez przeglądarkę na komputerze. Wszystko krok po kroku, także dla osób, które robią to pierwszy raz:
**[instrukcja wgrywania](firmware/README.md)**.

Po wgraniu zasilaj płytkę z powerbanku, akumulatora albo ładowarki USB. Od razu zaczyna nasłuchiwać.

### 2. Otwórz stronę na telefonie

| Telefon | Przeglądarka |
|---|---|
| **Android** | **Google Chrome** |
| **iPhone / iPad** | **Bluefy** (darmowa, z App Store). Safari i Chrome na iPhonie nie obsługują Bluetooth na stronach. |

Wejdź na **https://qldadm.github.io/rid-heltec/** i włącz w telefonie **Bluetooth** oraz **lokalizację**
(lokalizacja jest potrzebna do odległości i kierunku do drona).

Wygodnie jest dodać stronę do ekranu głównego (w menu przeglądarki: „Dodaj do ekranu głównego”).
Wtedy otwiera się jak aplikacja i działa także bez internetu.

### 3. Połącz telefon z Heltec

1. Na stronie naciśnij **Połącz**.
2. Z listy wybierz **RID-Heltec-XXXX**.
3. Telefon zapyta o **PIN**. Wpisz 6 cyfr, które w tym momencie widać na ekranie Heltec.
4. Gotowe. Następnym razem telefon połączy się bez PIN-u.

Po połączeniu strona pobiera też historię dronów, które płytka zapisała, gdy telefonu nie było w pobliżu.

---

## Co widać na stronie

- **Lista dronów** z ID, wysokością, prędkością, siłą sygnału, odległością i kierunkiem od telefonu oraz danymi operatora.
- **Radar i mapa** z położeniem dronów, operatorów i Twoim.
- Przyciski **Swój / Obcy**. Swoje drony nie wywołują alarmu.
- **Alarm** (dźwięk i wibracja) przy nowym, nieoznaczonym dronie.
- **Historia** lotów zapisana w płytce.

## Prywatność

Strona nie zawiera reklam ani śledzenia i nigdzie nie wysyła danych. Wszystko idzie tylko między telefonem a Twoją płytką.
Połączenie wymaga sparowania PIN-em i jest szyfrowane, więc obcy telefon nie podejrzy Twojej płytki.

## Najczęstsze pytania

| Pytanie | Odpowiedź |
|---|---|
| Po naciśnięciu „Połącz” płytki nie ma na liście | Sprawdź, czy Heltec jest włączony i ma wgrany firmware RID (na ekranie widać kanał i liczniki) oraz czy telefon ma włączony Bluetooth. Na Androidzie włącz też lokalizację. |
| Na iPhonie przycisk „Połącz” nie działa | Otwórz stronę w aplikacji **Bluefy**, nie w Safari. |
| Telefon nie łączy się po aktualizacji firmware | Usuń płytkę w ustawieniach Bluetooth telefonu i sparuj ją ponownie z nowym PIN-em. |
| Czy wykryje każdego drona? | Tylko drony, które nadają Remote ID. Zasięg to zwykle kilkaset metrów, zależnie od drona i terenu. |

---

## Zawartość repozytorium

- `index.html`, `sw.js`, `manifest.webmanifest` i ikony: strona podglądu, publikowana przez GitHub Pages.
- [firmware/](firmware/): kod źródłowy firmware dla Heltec V3 i instrukcja wgrywania.
- [Releases](https://github.com/QLDADM/rid-heltec/releases): gotowe pliki firmware do wgrania.

## Kilka płytek naraz

Przycisk **Dodaj płytkę** łączy kolejne płytki: Heltec i [CYD](https://github.com/QLDADM/rid-cyd). Każda ma własny
wiersz ze stanem, a drony ze wszystkich są na jednej liście i mapie, z informacją, która płytka je widzi.

## Inne wykrywacze z tym samym firmware (wersja 1.5)

- [RID CYD](https://github.com/QLDADM/rid-cyd): płytka ESP32-2432S028 z kolorowym ekranem dotykowym 2,8", radarem i zapisem na kartę SD.
- [RID M5](https://github.com/QLDADM/rid-m5): M5StickC Plus2, mały osobisty alarm z brzęczykiem i trybem szukania drona.

Oznaczenia swój/obcy są wspólne dla wszystkich urządzeń, gdy łączy je agent RID Decoder na komputerze.
