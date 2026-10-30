# Firmware RID dla Heltec WiFi LoRa 32 V3 (wersja 1.5)

Zamienia płytkę Heltec V3 (ESP32-S3 + SX1262) w wykrywacz dronów Remote ID (ASTM F3411 / Open Drone ID).

- Odbiera Wi-Fi Beacon, Wi-Fi NAN, BLE 4 i BLE 5 Long Range (2,4 GHz).
- Pokazuje drony na OLED: ekran główny, lista, szczegóły i stan płytki (przycisk PRG).
- Bez komputera zapisuje wykryte drony w pamięci flash (około 4,9 MB) i prowadzi historię do 32 dronów ze śladem lotu.
- Łączy się przez Bluetooth z [podglądem na telefonie](https://qldadm.github.io/rid-heltec/) (parowanie z 6-cyfrowym PIN-em, do 8 telefonów).
- Oznaczenia swój/obcy są wspólne dla płytki, telefonu i serwera RID Decoder.
- Wykrywa też drony Wi-Fi bez Remote ID (DJI Tello, Parrot, zabawkowe drony z kamerą Wi-Fi) po nazwie sieci albo producencie.
- Może wysyłać alarmy o obcych dronach przez LoRa na kanał Meshtastic (domyślnie wyłączone).
- Komputer z agentem RID Decoder może się z nią łączyć przez Bluetooth zamiast kabla, także gdy telefon jest połączony w tym samym czasie.
- Pokazuje stan baterii i rozpoznaje ładowanie („LAD” z błyskawicą zamiast procentów, „pelna” po naładowaniu).

---

## Wgranie krok po kroku (bez instalowania programów)

### Co jest potrzebne

- Płytka **Heltec WiFi LoRa 32 V3**, najlepiej z podłączoną anteną LoRa.
- **Kabel USB-C do transmisji danych.** Część kabli służy tylko do ładowania i z nimi komputer nie widzi płytki.
  Jeśli nic się nie dzieje, najpierw zmień kabel.
- Komputer z przeglądarką **Google Chrome** albo **Microsoft Edge**. Firefox i Safari nie obsługują tej metody.

### Krok 1. Pobierz plik firmware

1. Wejdź na https://github.com/QLDADM/rid-heltec/releases.
2. Przy najnowszej wersji (np. **v1.5**) w sekcji **Assets** kliknij **`rid-heltec-v3-full.bin`**.
3. Plik trafi do folderu **Pobrane**. Nie otwieraj go, nie rozpakowuj i nie zmieniaj mu nazwy.

### Krok 2. Podłącz płytkę

1. Podłącz Heltec kablem USB-C do komputera. Ekran płytki może się zaświecić albo nie, oba przypadki są w porządku.
2. Zamknij programy, które mogą używać płytki: agenta RID Decoder (`start_rid.py`), aplikację Meshtastic na
   komputerze, Arduino, monitor portu. Port może naraz używać tylko jeden program.
3. **Windows:** płytka ma układ USB **CP2102**. Windows 10 i 11 zwykle instaluje sterownik sam w ciągu kilkudziesięciu
   sekund. Jeśli w kroku 3 przeglądarka nie pokazuje żadnego portu, zainstaluj sterownik
   „CP210x Universal Windows Driver” ze strony Silicon Labs:
   https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers. Potem odłącz i podłącz płytkę ponownie.

### Krok 3. Otwórz programator w przeglądarce

1. W Chrome albo Edge wejdź na **https://espressif.github.io/esptool-js/**.
2. Zostaw **puste** pole wyboru **WebUSB (CH340)** (Heltec ma inny układ USB) oraz **Show Debug log**.
3. W sekcji **Program** zostaw **Baudrate: 921600**.

### Krok 4. Połącz się z płytką

1. Kliknij **Connect**.
2. Przeglądarka pokaże okienko z listą portów. Wybierz pozycję podobną do
   **„CP2102 USB to UART Bridge Controller (COM9)”**. Numer COM może być inny. Kliknij **Połącz**.
3. Poczekaj kilka sekund. W czarnej konsoli na dole pojawi się tekst z **„ESP32-S3”**, a w sekcji Program
   **pojawi się tabela z polem „Flash Address”** i przyciskiem wyboru pliku. Przed połączeniem tej tabeli nie ma.

**Jeśli połączenie się nie udaje** (błąd albo brak ESP32-S3 w konsoli), wprowadź płytkę w tryb wgrywania:
1. przytrzymaj przycisk **PRG** na płytce (drugi to **RST**),
2. trzymając PRG, krótko naciśnij **RST**,
3. puść **PRG**,
4. odśwież stronę (F5) i kliknij **Connect** jeszcze raz.

### Krok 5. Wgraj firmware

1. W polu **Flash Address** wpisz **`0x0`** (zero, iks, zero).
2. Kliknij **Wybierz plik** w tym samym wierszu i wskaż pobrany **`rid-heltec-v3-full.bin`**.
3. Kliknij **Program**.
4. Pasek postępu dojdzie do 100% w około 30–60 s. **Nie odłączaj kabla i nie zamykaj strony w trakcie.**
5. Gdy konsola pokaże zakończenie, naciśnij na płytce przycisk **RST** albo odłącz i podłącz kabel.

### Krok 6. Sprawdź, czy działa

Na ekranie Heltec powinien pojawić się ekran wykrywacza z numerem kanału Wi-Fi i licznikami ramek.
Krótkie naciśnięcie **PRG** przełącza ekrany. Jeśli ekran jest ciemny, kliknij PRG dwa razy szybko (dwuklik włącza i wyłącza ekran).

Gotowe. Płytka od razu nasłuchuje i zapisuje drony, także zasilana z powerbanku.

---

## Co warto wiedzieć przed wgraniem

- **Meshtastic zostanie nadpisany.** Jeśli płytka miała Meshtastic, zrób najpierw w aplikacji Meshtastic kopię ustawień
  (kanały, klucze). Meshtastic można później przywrócić przez https://flasher.meshtastic.org.
- **Ustawienia płytki są kasowane** także przy aktualizacji z wcześniejszej wersji RID:
  - płytka wylosuje **nowy PIN**,
  - telefony trzeba **sparować od nowa**: najpierw usuń płytkę w ustawieniach Bluetooth telefonu,
  - ustawienia Meshtastic trzeba wysłać ponownie (ze strony RID Decoder),
  - oznaczenia swój/obcy wrócą same po podłączeniu do komputera z agentem.

  Zapisane drony i historia zostają.
- Antena LoRa: płytka domyślnie **nie nadaje** przez LoRa. Gdy włączysz alarmy Meshtastic, antena LoRa musi być podłączona,
  bo nadawanie bez anteny może uszkodzić radio.

## Pierwsze połączenie z telefonem

1. Otwórz stronę podglądu https://qldadm.github.io/rid-heltec/: na Androidzie w **Chrome**, na iPhonie w przeglądarce **Bluefy**
   (Safari nie obsługuje Bluetooth na stronach).
2. Kliknij **Połącz** i wybierz płytkę **RID-Heltec-XXXX**.
3. Telefon zapyta o PIN. Wpisz **6 cyfr widocznych na ekranie Heltec**.
4. Kolejne połączenia z tym telefonem odbywają się już bez PIN-u.

## Połączenie z komputerem przez Bluetooth

Agent RID Decoder (`start_rid.py`) obsługuje płytkę kablem USB albo przez Bluetooth komputera:

1. Podłącz płytkę **raz** kablem USB, gdy działa agent. Agent zapamięta jej PIN.
2. Odłącz kabel. Po kilkunastu sekundach agent połączy się z płytką przez Bluetooth i paruje ją sam.
   Na stronie RID Decoder Heltec ma wtedy port **BLE**.
3. Po podłączeniu kabla agent sam wraca na USB.

Przez Bluetooth działa wszystko to co po kablu: drony, pobieranie pamięci, oznaczenia, Meshtastic.
Telefon może być połączony z płytką w tym samym czasie. Zasięg zwykle wynosi 10–30 m.
Na czas połączenia agent wyłącza własny skaner Bluetooth, bo drony BLE wykrywa wtedy płytka.

## Przycisk PRG

| Akcja | Działanie |
|---|---|
| krótkie naciśnięcie | następny ekran: główny → lista dronów → szczegóły → stan płytki |
| przytrzymanie 2 s i puszczenie, na szczegółach | oznaczenie drona: brak → SWÓJ → OBCY → brak |
| przytrzymanie 2 s i puszczenie, na stanie | kanał Wi-Fi: skakanie ↔ stały kanał 6 |
| dwuklik | ekran wł./wył. |
| **przytrzymanie 6 s** | **wyłączenie** (po 4 s pojawia się „Trzymaj: wylaczenie”) |
| **przytrzymanie 1 s przy wyłączonej płytce** | **włączenie** |

Wyłączona płytka śpi z bardzo małym poborem prądu. Krótkie dotknięcie PRG jej nie włącza, przycisk RST zawsze ją uruchamia.

Ekran gaśnie sam po 2 minutach, a nasłuch działa dalej. Budzi go przycisk, nowy dron albo parowanie telefonu.

## Najczęstsze problemy

| Problem | Rozwiązanie |
|---|---|
| Po kliknięciu Connect lista portów jest pusta | Inny kabel (do transmisji danych), inny port USB, sterownik CP210x (krok 2). |
| „Failed to connect” / „Timed out” | Tryb wgrywania: przytrzymaj PRG, naciśnij RST, puść PRG i połącz ponownie. Zamknij inne programy używające portu. |
| Port jest zajęty | Zamknij agenta RID Decoder, aplikację Meshtastic na komputerze albo drugą kartę z esptool-js. |
| Po wgraniu ekran jest czarny | Naciśnij RST. Jeśli dalej nic, kliknij dwa razy PRG (dwuklik włącza ekran). |
| Podczas ładowania nie widać procentów, tylko „LAD” | Tak ma być. Ładowarka zawyża napięcie baterii, więc procent byłby fałszywy (prawie zawsze 100%). Płytka rozpoznaje ładowanie po skoku napięcia. Prawdziwy procent pojawia się 15–30 s po odłączeniu kabla, a na ekranie STAN widać procent sprzed podłączenia. |
| Telefon nie łączy się po aktualizacji | Usuń płytkę w ustawieniach Bluetooth telefonu i sparuj ponownie z nowym PIN-em. |

---

## Dla zaawansowanych

### Wgranie z wiersza poleceń

```powershell
pip install esptool
esptool --chip esp32s3 --baud 921600 write_flash 0x0 rid-heltec-v3-full.bin
```

### Budowanie ze źródeł

```powershell
pip install platformio
cd firmware
pio run -e heltec_v3 -t upload        # pełna wersja
pio run -e heltec_v3_wifi -t upload   # tylko Wi-Fi, bez Bluetooth
```

Wgranie przez PlatformIO **nie kasuje** ustawień (PIN, telefony, Meshtastic), bo zapisuje tylko program.

### Polecenia przez USB (921600 bod)

| Polecenie | Działanie |
|---|---|
| `ID` | wersja i stan płytki |
| `CH 6`, `CH 0` | stały kanał 6, powrót do skakania po kanałach |
| `TIME <unix>` | ustawienie czasu |
| `DUMP`, `CLEAR` | wypisanie zapisu z pamięci, wyczyszczenie zapisu |
| `PIN 123456` | zmiana PIN-u Bluetooth |
| `BONDCLR` | usunięcie sparowanych telefonów |
