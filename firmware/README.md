# Firmware RID dla Heltec WiFi LoRa 32 V3 (wersja 1.6)

Zamienia płytkę Heltec V3 (ESP32-S3 + SX1262) w wykrywacz dronów Remote ID (ASTM F3411 / Open Drone ID).

- Odbiera Wi-Fi Beacon, Wi-Fi NAN, BLE 4 i BLE 5 Long Range (2,4 GHz).
- Pokazuje drony na OLED: ekran główny, lista, szczegóły, stan płytki (przycisk PRG).
- Bez komputera zapisuje wykryte drony w pamięci flash (około 4,9 MB) i prowadzi historię do 32 dronów ze śladem lotu.
- Łączy się przez Bluetooth z [podglądem na telefonie](../) (parowanie z 6-cyfrowym PIN-em, do 8 telefonów).
- Oznaczenia swój/obcy są wspólne z telefonem i z serwerem RID Decoder.
- Może wysyłać alarmy o obcych dronach przez LoRa na kanał Meshtastic (domyślnie wyłączone).
- Pokazuje stan baterii.

Wgranie nadpisuje Meshtastic. Zrób najpierw kopię ustawień, bo Meshtastic można potem przywrócić przez
https://flasher.meshtastic.org.

## Wgranie gotowego pliku

Plik [release/rid-heltec-v3-full.bin](release/rid-heltec-v3-full.bin) zawiera cały obraz pamięci i wgrywa się od adresu 0x0.

- Przeglądarka (Chrome lub Edge): https://espressif.github.io/esptool-js/. Pole „WebUSB” zostaw puste.
  Kliknij Connect i wybierz port płytki. Dopiero po połączeniu pojawia się tabela z polem Flash Address:
  wpisz `0x0`, wybierz ten plik i kliknij Program.
- Albo z wiersza poleceń:

```powershell
pip install esptool
esptool --chip esp32s3 --baud 921600 write_flash 0x0 rid-heltec-v3-full.bin
```

Jeśli wgrywanie nie startuje, przytrzymaj PRG, naciśnij RST i puść PRG.

## Budowanie ze źródeł

```powershell
pip install platformio
cd firmware
pio run -e heltec_v3 -t upload        # pełna wersja
pio run -e heltec_v3_wifi -t upload   # tylko Wi-Fi, bez Bluetooth
```

## Przycisk PRG

| Akcja | Działanie |
|---|---|
| krótkie naciśnięcie | następny ekran: główny → lista dronów → szczegóły → stan płytki |
| przytrzymanie 2 s na szczegółach | oznaczenie drona: brak → SWÓJ → OBCY → brak |
| przytrzymanie 2 s na stanie | kanał Wi-Fi: skakanie ↔ stały kanał 6 |
| dwuklik | ekran wł./wył. |

## Pierwsze połączenie z telefonem

1. Otwórz stronę podglądu: na Androidzie w Chrome, na iPhonie w przeglądarce Bluefy.
2. Wybierz „Połącz” i płytkę **RID-Heltec-XXXX**.
3. Wpisz PIN widoczny na OLED. Płytka losuje go przy pierwszym starcie.

## Polecenia przez USB (921600 bod)

| Polecenie | Działanie |
|---|---|
| `ID` | wersja i stan płytki |
| `CH 6`, `CH 0` | stały kanał 6, powrót do skakania po kanałach |
| `TIME <unix>` | ustawienie czasu |
| `DUMP`, `CLEAR` | wypisanie zapisu z pamięci, wyczyszczenie zapisu |
| `PIN 123456` | zmiana PIN-u Bluetooth |
| `BONDCLR` | usunięcie sparowanych telefonów |
