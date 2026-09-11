# AGENTS.md — ESP32 WiFi Repeater

Przewodnik dla agenta AI / developera przejmującego pracę nad tym projektem.
Zakłada zerową wiedzę o repo, ale znajomość C i ESP-IDF na poziomie podstawowym.

---

## 1. Co to jest

BezNAT-owy repeater WiFi na ESP32. Klienci podłączeni do repeatera dostają
adresy IP z DHCP routera i **siedzą w tej samej podsieci** co reszta sieci —
zero translacji adresów, przezroczysty bridge L2.

Osiągane przez trzy mechanizmy działające razem:

1. **MAC cloning** — STA repeatera przybiera MAC pierwszego klienta ("primary").
   Router myśli, że gada bezpośrednio z klientem.
2. **MAC-NAT** — kolejni klienci (2–4) mają przepisywane adresy L2 w locie,
   z tablicą `IP → prawdziwy MAC` uczoną z ruchu i z DHCP.
3. **Sniffer DHCP ACK** — repeater podsłuchuje ACK przechodzący przez bridge,
   poznaje podsieć klienta i ustawia własny AP na `<podsieć>.254`, żeby GUI
   było dostępne bez zmiany ustawień IP na telefonie/laptopie.

Główny target: **ESP32-C5** (WiFi 6, dual-band 2.4 + 5 GHz).
Wspierane też: C6, S3, C3, ESP32 classic (wszystkie 2.4 GHz).

---

## 2. Środowisko (Windows) — to jest najważniejsza sekcja

ESP-IDF **v6.1**. Instalacja przez EIM, więc `export.ps1` z katalogu IDF
**NIE zadziała** (szuka venva w `~/.espressif`, którego tu nie ma).

Aktywacja środowiska — dokładnie ta linia, w każdej nowej sesji shella:

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
```

Ustawia m.in.:

| Zmienna | Wartość |
|---|---|
| `IDF_PATH` | `C:\esp\v6.1\esp-idf` |
| `IDF_TOOLS_PATH` | `C:\Espressif\tools` |
| `IDF_PYTHON_ENV_PATH` | `C:\Espressif\tools\python\v6.1\venv` |

Dodatkowo warto ustawić `$env:PYTHONUTF8="1"` — bez tego `idf.py` sypie
ostrzeżeniem o Unicode na polskim locale (nieszkodliwe, ale zaśmieca output).

Jednolinijkowiec do skryptów/agentów:

```powershell
$env:PYTHONUTF8="1"; . C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1 *> $null; idf.py build
```

**Na innym komputerze / Linuksie**: wystarczy ESP-IDF v6.1 i normalne
`. $IDF_PATH/export.sh`. Projekt nie ma żadnych zależności poza IDF
(`main/idf_component.yml` wymaga tylko `idf >= 6.1.0`).

---

## 3. Build / flash / monitor

```powershell
idf.py set-target esp32c5      # tylko przy zmianie targetu (kasuje sdkconfig)
idf.py build
idf.py -p COM3 flash
idf.py -p COM3 monitor         # wyjście: Ctrl+]
```

Płytka jest na **COM3** (USB-Serial/JTAG, 4 MB flash, C5 rev v1.0).

Inne targety:

```powershell
idf.py set-target esp32c6   # / esp32s3 / esp32c3 / esp32
```

`sdkconfig` jest generowany i **gitignorowany** — źródłem prawdy są
`sdkconfig.defaults` + `sdkconfig.defaults.<target>`. Po edycji któregoś z nich
skasuj `sdkconfig` i przebuduj, inaczej zmiany nie wejdą:

```powershell
Remove-Item sdkconfig; idf.py build
```

### Nieinteraktywne czytanie UART

`idf.py monitor` jest interaktywny i blokuje agenta. Do zrzutu logu na
określony czas jest helper:

```powershell
python tools\serial_capture.py COM3 25    # port, sekundy
```

---

## 4. USB-Serial/JTAG na ESP32-C5 — przeczytaj przed debugowaniem

Ta sekcja to zapis kilku godzin błądzenia. Oszczędzi ci je.

### 4.0 Najpierw: nie wieszaj płytki na zewnętrznym hubie USB

**To była przyczyna większości problemów w tej sesji.** Na zewnętrznym hubie
USB objawy były takie:

- `esptool` łączył się dokładnie **raz** po BOOT+RESET, a każda kolejna
  operacja kończyła się `Write timeout`,
- `read-flash` nie przechodził ani razu,
- port COM potrafił zniknąć z systemu w trakcie pracy,
- konsola aplikacji nie wypisywała nic, co wyglądało jak zawieszony firmware.

Po przełożeniu na hub wbudowany w obudowę komputera **wszystko zaczęło działać
od pierwszego strzału**: pełny log bootu, stabilny `read-flash`, powtarzalny
flash. Firmware nie był winny.

Wniosek: jeśli widzisz cokolwiek z powyższej listy, **najpierw zmień punkt
podłączenia USB**, a dopiero potem szukaj błędu w kodzie. C5 przy WiFi TX ma
skokowy pobór prądu, a USB-Serial/JTAG robi reenumerację przy każdym resecie —
słaby hub tego nie wytrzymuje.

### 4.1 Konsola aplikacji musi być przestawiona na USB

Domyślnie IDF ustawia `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`, a USB-Serial/JTAG
tylko jako *secondary*. Efekt: przez USB widzisz komunikaty ROM-u i bootloadera,
ale **żadnego `ESP_LOGx` z aplikacji** — bo te idą na fizyczne piny UART0,
których DevKit nie wyprowadza. Wygląda dokładnie jak zawieszona płytka.

`sdkconfig.defaults.esp32c5` ustawia więc:

```
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
```

Sprawdzenie, że weszło:

```powershell
Select-String -Path sdkconfig -Pattern "ESP_CONSOLE"
# ma być: CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
#         CONFIG_ESP_CONSOLE_UART_NUM=-1
```

### 4.2 `Write timeout` NIE znaczy zawieszonego CPU

```
A serial exception error occurred: Write timeout
```

To jest **normalny stan działającej aplikacji**. Konsola USB-Serial/JTAG tylko
pisze; nikt nie czyta stdin, więc dane host→urządzenie zostają w sprzętowym
FIFO, endpoint OUT zaczyna NAK-ować i zapis z hosta się nie kończy.

Potwierdzone eksperymentalnie: firmware wypisywał w tym samym czasie normalne
logi na monitorze, a `esptool --before no-reset` dostawał `Write timeout`.

Praktycznie: **nie diagnozuj po `Write timeout`.** Żeby pogadać z chipem, użyj
`--before default-reset` (domyślne) — wtedy esptool sam wprowadzi go w download
mode. Dopiero gdy i to zawodzi, podejrzewaj zawieszenie albo złe USB (§4.0).

### 4.3 Reset i boot mode

Przy zdrowym połączeniu USB reset po liniach CDC działa poprawnie i uruchamia
aplikację — w logu widać `rst:0x15 (USB_UART_HPSYS),boot:0x58
(SPI_FAST_FLASH_BOOT)`. Zarówno `idf.py flash` (kończy się resetem), jak i
`idf.py monitor` wstają normalnie.

Jeśli **każdy** reset daje `boot:0x8` albo `boot:0x48` czyli
`DOWNLOAD(UART0/USB)`, to znaczy że GPIO9 jest trzymany nisko — sprawdź, czy
przycisk BOOT nie jest wciśnięty albo zacięty.

Czego **nie** używać:

| Nie rób | Dlaczego |
|---|---|
| `esptool --after watchdog-reset` | resetuje rdzeń, ale nie peryferial USB-Serial/JTAG — stan endpointu rozjeżdża się z hostem |
| `esptool --before no-reset` przy działającej aplikacji | zawsze `Write timeout` (§4.2) |
| grzebania w DTR/RTS z pySerial | Windows propaguje `SET_CONTROL_LINE_STATE` tylko parami, pojedyncza zmiana nie przechodzi; łatwo wpaść w download mode |

`tools/serial_capture.py` celowo **nie rusza** linii kontrolnych, dopóki nie
podasz `--reset`. Nie „naprawiaj" tego.

### 4.4 Odzyskiwanie płytki

Gdy nic nie pomaga (po sprawdzeniu §4.0):

1. Przytrzymaj **BOOT**
2. Wciśnij i puść **RESET**
3. Puść **BOOT**

Chip wchodzi w ROM download mode niezależnie od stanu aplikacji.

Lista portów bez esptoola (`Get-PnpDevice` / `Get-CimInstance` w tej sesji
zwracały puste wyniki — nie polegaj na nich):

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

### 4.5 Diagnostyka bez UART — znaczniki etapów bootu

Gdy logu nie da się złapać, firmware zapisuje numer etapu startu do NVS, a
`tools/read_boot_stage.py` odczytuje go z hosta (sam wchodzi w download mode):

```powershell
python tools\read_boot_stage.py COM3
# boot stage : 9 -- boot complete, all tasks running
# phy guard  : clear -- radio config completed successfully
```

Etapy są opisane w `repeater_config.h`. Skrypt parsuje strony NVS razem z
bitmapą stanów wpisów i numerami sekwencyjnymi stron — **nie** wystarczy
poszukać klucza w binarce, bo NVS trzyma też stare, skasowane wpisy i po
kompaktowaniu strony zwróciłoby to nieaktualną wartość.

### 4.6 PHY guard — dlaczego już nie musisz się bać

Żeby zła konfiguracja radia nie wymagała za każdym razem BOOT+RESET,
firmware zapisuje w NVS flagę `phy_try` **przed** dotknięciem PHY i czyści ją
po sukcesie (`repeater_phy_guard_arm/disarm/tripped` w `repeater_config.c`).

Jeśli przy starcie flaga wciąż jest ustawiona, znaczy że poprzedni boot nie
dojechał do końca — firmware loguje `PHY SAFE MODE` i wstaje na 2.4 GHz /
20 MHz, bez zmiany band mode. Wtedy wystarczy **cykl zasilania** (nie
BOOT+RESET), żeby dostać działające GUI i poprawić ustawienia.

---

## 5. Układ repo

```
main/
  wifi_repeater_main.c   ~1400 linii — cała logika: bridge, MAC clone,
                         MAC-NAT, sniffer DHCP, roaming, konfiguracja PHY
  repeater_config.[ch]   warstwa NVS: defaults z Kconfig → nadpisanie z NVS
                         + PHY guard
  repeater_httpd.[ch]    serwer HTTP + GUI (jeden string HTML z %s)
  Kconfig.projbuild      menuconfig → "WiFi Repeater Configuration"
sdkconfig.defaults               wspólne dla wszystkich targetów
sdkconfig.defaults.esp32c5       C5: 240 MHz, konsola USB, bufory/AMPDU, 4 MB
sdkconfig.defaults.esp32c6/s3/c3/esp32
tools/serial_capture.py          nieinteraktywny zrzut UART
```

---

## 6. Architektura — co gdzie siedzi

### Ścieżka pakietu (hot path)

`esp_wifi_internal_reg_rxcb()` **zastępuje** domyślny handler RX. Po
rejestracji pakiety **nie idą** automatycznie do lwIP — trzeba je ręcznie
przekazać przez `esp_netif_receive()` albo zwolnić
`esp_wifi_internal_free_rx_buffer()`. Zgubienie bufora = wyciek.

- `on_sta_rx()` — z routera: sniff DHCP ACK → MAC-NAT downstream →
  `esp_wifi_internal_tx(WIFI_IF_AP)` → warunkowo lwIP
- `on_ap_rx()` — od klienta: MAC-NAT upstream →
  `esp_wifi_internal_tx(WIFI_IF_STA)` → warunkowo lwIP

Te dwie funkcje wołają się dla **każdej ramki L2**, więc:

- `CONFIG_REPEATER_BROADCAST_FILTER` (domyślnie ON) przepuszcza do lwIP tylko
  ARP request o nasze IP; reszta broadcastu (mDNS, SSDP, NetBIOS, IGMP, IPv6)
  jest forwardowana na L2, ale omija stos → ~10–20k cykli/pakiet oszczędności
- sniffer DHCP ma inline pre-check `EtherType==0x0800 && UDP && port 67→68`,
  więc wywołanie funkcji następuje dla ~0.1% ruchu
- MAC-NAT jest całkowicie pomijany przy `s_client_count <= 1`
- `macnat_learn()` nie woła `esp_timer_get_time()` gdy IP+MAC się nie zmieniły

**Nie dodawaj tu logowania ani alokacji.** Każdy cykl kosztuje throughput.

### Maszyna stanów

`STATE_IDLE → STATE_MAC_CHANGING → STATE_BRIDGING → STATE_MAC_RESTORING → IDLE`

Zmiany MAC dzieją się w osobnym tasku (`mac_change_task`), **nigdy** w
handlerze eventów — bo wymagają `esp_wifi_disconnect()`/`connect()`, co
generuje nowe eventy i zablokowałoby pętlę. Task jest serializowany przez
`s_mac_task_mutex`.

`s_suppress_auto_reconnect` blokuje auto-reconnect z
`WIFI_EVENT_STA_DISCONNECTED`, gdy połączeniem zarządza task zmiany MAC albo
kod startowy. Ustaw go zawsze, kiedy sam wołasz disconnect/connect.

### Liczenie klientów

`s_client_count` jest liczony z `esp_wifi_ap_get_sta_list()`, **nie** przez
`++`/`--` na eventach. Powód: timeout SA Query generuje zduplikowane eventy
"leave", które rozjeżdżały licznik.

---

## 7. Pasma i PHY (ESP32-C5)

### Reguła nr 1: konfiguruj PHY, dopóki SoftAP jeszcze nie działa

Start jest dwufazowy i taka kolejność jest sprawdzona na sprzęcie:

```
init_wifi()                  # netify, esp_wifi_init, tryb STA-only, config STA
esp_wifi_start()             # startuje tylko STA
radio_apply_phy_config()     # band mode + protokoły + bandwidth, TYLKO dla STA
start_softap()               # set_mode(APSTA) → set_config(AP)
esp_wifi_connect()           # dopiero teraz łączymy się z upstreamem
```

Powody:

- `esp_wifi_set_band_mode()` wymaga wystartowanego WiFi
  (`ESP_ERR_WIFI_NOT_STARTED`), więc nie da się tego zrobić w `init_wifi()`.
  Tak samo robi `examples/wifi/power_save`.
- Parametrów PHY dla `WIFI_IF_AP` **nie ustawiamy wcale**. Nie trzeba: w APSTA
  kanał i pasmo STA mają wyższy priorytet, więc AP jest dociągany do upstreamu
  po połączeniu, a sterownik sam sprowadza SoftAP do 20 MHz (patrz reguła 3).

W `start_softap()` kolejność jest odwrotna niż przy PHY: **najpierw**
`esp_wifi_set_mode(WIFI_MODE_APSTA)`, **potem** `esp_wifi_set_config(WIFI_IF_AP)`.
Konfigurowanie wyłączonego interfejsu zwraca `ESP_ERR_WIFI_IF`, a pod
`ESP_ERROR_CHECK` dawało to `abort()` → panic → reboot → pętla restartów.
Dlatego te dwa wywołania mają logowane błędy zamiast `ESP_ERROR_CHECK`.

`app_main()` ustawia `s_suppress_auto_reconnect = true` przed
`esp_wifi_start()`, żeby handler `WIFI_EVENT_STA_START` nie zaczął się łączyć,
zanim PHY i AP będą gotowe.

### Reguła nr 2: kolejność w środku `radio_apply_phy_config()`

1. `esp_wifi_set_band_mode()` — musi być pierwszy; kolejne API ignorują pasmo
   wyłączone przez band mode.
2. `esp_wifi_set_protocols()` — **przed** bandwidth.
3. `esp_wifi_set_bandwidths()`.

### Reguła nr 3: 40 MHz wyklucza się z 11ax/11ac

Sterownik przyjmuje `WIFI_BW40` tylko wtedy, gdy w masce protokołów tego pasma
**nie ma** ani `WIFI_PROTOCOL_11AX`, ani `WIFI_PROTOCOL_11AC`. Zgadza się to
z docstringiem `esp_wifi_set_bandwidths()` („When the interface supports
11AX/11AC, it only supports setting WIFI_BW20") oraz z
`$IDF_PATH/examples/wifi/ftm/main/ftm_main.c`, gdzie BW40 zawsze idzie w parze
z maską `11A|11N`, a `11AC|11AX` tylko z BW20.

Sterownik mówi to też sam w logu bootu, gdy startuje SoftAP:

```
W wifi:11ax/11ac mode can not work under phy bw 40M, the softap 2G bandwidth changed to 20M
W wifi:11ax/11ac mode can not work under phy bw 40M, the softap 5G bandwidth changed to 20M
```

Kod utrzymuje tę zależność automatycznie dla STA: wybór 40 MHz dla danego pasma
zdejmuje z jego maski 11ax/11ac i loguje ostrzeżenie.

### Reguła nr 4: w 5 GHz SoftAP MUSI mieć 20 MHz

Zweryfikowane na sprzęcie (C5 + Pixel 7). Przy SoftAP w 5 GHz i 40 MHz klient
kojarzy się poprawnie, a po ~4 s jest wyrzucany:

```
I wifi:station: 32:f1:8c:11:0a:da join, AID=1, an, 40D
I wifi:station: 32:f1:8c:11:0a:da leave, AID = 1, reason = 15
```

`reason 15` to `4WAY_HANDSHAKE_TIMEOUT` — kojarzenie przechodzi, ale uzgadnianie
kluczy WPA2 nie. Powtarzalne w 100% prób. Po zmianie tylko szerokości kanału na
20 MHz ten sam telefon łączy się od pierwszego razu:

```
I wifi:station: 32:f1:8c:11:0a:da join, AID=1, an, 20
I wifi6_rep: -> Client joined: ... === MAC CLONE ... === BRIDGE ACTIVE ===
```

Dlatego domyślne 5 GHz to 20 MHz, a wybór 40 MHz loguje wyraźne ostrzeżenie.
W 2.4 GHz 40 MHz działa poprawnie.

### Obserwacja: SoftAP wydaje się nie oferować HE

Mimo maski protokołów z `WIFI_PROTOCOL_11AX`, klienci kojarzą się z naszym AP
jako `an` (802.11a/n), a Pixel raportuje 65 Mb/s przy 20 MHz — czyli 11n HT20
MCS7. Przy HE20 powinno być ~86 Mb/s lub więcej. To sugeruje, że HE działa tylko
po stronie STA (upstream), a SoftAP kończy na 802.11n.

Dokumentacja IDF tego nie stwierdza wprost, więc traktuj to jako obserwację.
Konsekwencja praktyczna jest jednak konkretna: **przepustowość do klienta
zależy tylko od szerokości kanału**, bo tryb i tak jest 11n:

| Konfiguracja AP | Link klienta | Uwaga |
|---|---|---|
| 2.4 GHz, 40 MHz | 150 Mb/s | najszybsze działające |
| 5 GHz, 20 MHz | 65 Mb/s | 5 GHz nie może mieć 40 MHz (reguła 4) |
| 2.4 GHz, 20 MHz | 72 Mb/s | |

### Dlaczego domyślnie 20 MHz w obu pasmach

| Konfiguracja | PHY rate @ 1SS |
|---|---|
| HE20 (20 MHz + 11ax) | 143 Mbps |
| HT40 (40 MHz + 11n)  | 150 Mbps |

Espressif podaje surowy sufit C5 na ~130 Mbps (raw 802.11 RX/TX), więc HT40
nic nie zyskuje, a traci OFDMA/MU-MIMO i odporność HE. **20 MHz + 11ax jest
optymalne** i to jest domyślna konfiguracja.

### Jedno radio

`WIFI_BAND_MODE_AUTO` **nie znaczy** jednoczesny dual-band — znaczy
"wybierz pasmo automatycznie". C5 ma jedno radio. W APSTA kanał STA ma
priorytet nad kanałem AP, więc SoftAP jest przenoszony na kanał (i pasmo)
upstreamu po połączeniu.

Kanał startowy AP musi leżeć w wybranym pasmie: `1` dla 2.4G/auto, `36` dla
5G-only (najniższy UNII-1, bez DFS).

### Preferencja 5 GHz

`wifi_sta_config_t.threshold.rssi_5g_adjustment` (domyślnie 10 dB) — AP 5 GHz
o tym samym SSID jest wybierany, dopóki jego RSSI nie jest gorszy od 2.4 GHz
o więcej niż tyle dB. Ta sama logika jest powtórzona ręcznie w `roaming_task()`
przy ocenie kandydatów.

---

## 8. Konfiguracja — trzy warstwy

```
Kconfig (menuconfig)  →  NVS  →  web GUI
   wartości startowe     zapis    edycja
```

`repeater_config_load()` **najpierw** wypełnia strukturę defaultami z Kconfig
(`repeater_config_defaults()`), **potem** nakłada klucze obecne w NVS. Dzięki
temu dodanie nowego pola nie wymaga duplikowania kodu, a stare NVS bez nowego
klucza dostaje sensowną wartość. Na końcu są clampy zakresów — NVS z innej
wersji firmware'u nie może wstawić wartości nieobsługiwanej przez sprzęt.

**Dodając nowe pole konfiguracji trzeba ruszyć 5 miejsc:**

1. `repeater_config_t` w `repeater_config.h`
2. `repeater_config_defaults()` + `load_u8`/`load_str` + clamp + `..._save()`
   w `repeater_config.c`
3. `Kconfig.projbuild` (dla SoC-zależnych opcji pamiętaj o fallbacku
   `#ifndef CONFIG_...` w `repeater_config.c` — Kconfig nie definiuje symbolu,
   gdy `depends on` nie jest spełnione)
4. `repeater_httpd.c`: HTML (`BAND_CARD` albo `HTML_PAGE`) + parsowanie
   w `save_post_handler()`
5. użycie w `wifi_repeater_main.c`

### Uwaga o `HTML_PAGE`

To jeden wielki string formatujący. Liczba i kolejność `%s`/`%d` **musi**
zgadzać się z argumentami `snprintf()`. Literalny znak `%` w CSS trzeba pisać
jako `%%`. Karta zależna od SoC (`BAND_CARD`) jest renderowana do osobnego
bufora i wstrzykiwana jednym `%s` — powtórz ten wzorzec dla kolejnych
warunkowych sekcji, nie mnóż `#if` w środku wielkiego stringa.

Endpointy: `GET /`, `POST /save`, `POST /reset`, `GET /status` (JSON).
Po zwiększeniu liczby handlerów podnieś `config.max_uri_handlers`.

---

## 9. Migracja z ESP-IDF v5.x na v6.1 — co się zmieniło

| v5.x | v6.1 |
|---|---|
| `WIFI_BW_HT20` / `WIFI_BW_HT40` | `WIFI_BW20` / `WIFI_BW40` (stare usunięte) |
| `esp_wifi_set_bandwidth()` | nadal jest, ale dla dual-band: `esp_wifi_set_bandwidths()` |
| `esp_wifi_set_protocol()` | zalecane `esp_wifi_set_protocols()` (per pasmo) |
| brak | `esp_wifi_set_band_mode()`, `esp_wifi_set_band()` |
| brak | `threshold.rssi_5g_adjustment` w `wifi_sta_config_t` |
| `ESP_IF_WIFI_STA` | usunięte → `WIFI_IF_STA` |
| `esp_netif_next()` | usunięte → `esp_netif_next_unsafe()` / `esp_netif_find_if()` |

API `esp_wifi_internal_reg_rxcb()`, `esp_wifi_internal_tx()`,
`esp_wifi_internal_free_rx_buffer()` z `esp_private/wifi.h` **nadal istnieją**
w v6.1 z niezmienionymi sygnaturami. To wciąż API prywatne — przy skoku na
v6.2+ sprawdź je pierwsze.

Pełne listy: `$IDF_PATH/docs/en/migration-guides/release-6.x/6.0/wifi.rst`
i `networking.rst`.

---

## 10. Weryfikacja zmian

Minimum przed uznaniem zadania za zrobione:

```powershell
$env:PYTHONUTF8="1"; . C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1 *> $null
idf.py build                      # musi być 0 errorów i 0 warningów
idf.py -p COM3 flash
idf.py -p COM3 monitor            # albo: python tools\serial_capture.py COM3 25
```

Tak wygląda potwierdzony poprawny boot na ESP32-C5 (wycinek):

```
rst:0x15 (USB_UART_HPSYS),boot:0x58 (SPI_FAST_FLASH_BOOT)
I boot.esp32c5: SPI Flash Size : 4MB
I cpu_start: cpu freq: 240000000 Hz
I app_init: ESP-IDF: v6.1
I wifi:mac_version:HAL_MAC_ESP32AX_752MP_ECO2, band mode:0x3
I wifi_init: rx ba win: 22
I wifi6_rep: PHY: setting band mode 2.4 GHz + 5 GHz (auto)...
I wifi6_rep: PHY: protocols 2.4G=0x47 5G=0x74
I wifi6_rep: PHY: bandwidth in use 2.4 GHz=20 MHz, 5 GHz=20 MHz
I wifi6_rep: PHY: config applied
I wifi6_rep: SoftAP: authmode 7, start channel 1
I wifi:mode : sta (...) + softAP (...)
I esp_netif_lwip: DHCP server started on interface WIFI_AP_DEF with IP: 192.168.4.1
I rep_httpd: HTTP server started on port 80
I wifi6_rep: Waiting for connections...
```

Co sprawdzać w tych liniach:

- `boot:0x58 (SPI_FAST_FLASH_BOOT)` — aplikacja faktycznie wstała
  (`DOWNLOAD` = nie wstała, patrz §4.3)
- `band mode:0x3` — sterownik pracuje w trybie 2.4 + 5 GHz
- `protocols 2.4G=0x47` = 11b|11g|11n|**11ax**; `5G=0x74` = 11n|11a|11ac|**11ax**
- `bandwidth in use ... 20 MHz` w obu pasmach — czyli HE jest aktywne
- `rx ba win: 22` — weszły ustawienia z `sdkconfig.defaults.esp32c5`

Po 8 s (potem co 30 s) leci raport statusu z realnym stanem radia czytanym ze
sterownika. Bez routera zobaczysz w nim `Up: not connected` i
`<< Disconnected (reason 201)` — to normalne, `reason 201` = NO_AP_FOUND dla
domyślnego SSID `MyUpstreamAP`.

Kontrola bez UART:

```powershell
python tools\read_boot_stage.py COM3
# boot stage : 9 -- boot complete, all tasks running
# phy guard  : clear -- radio config completed successfully
```

Test funkcjonalny (wymaga prawdziwego routera — wpisz jego SSID/hasło w GUI
pod `http://192.168.4.1` z telefonu):

1. `>> Connected to: <SSID> (ch 36 / 5 GHz, BSSID ...)` i w statusie
   `Link PHY: WiFi6 (11ax) @ 20 MHz`
2. Klient łączy się z AP repeatera → w logu `MAC CLONE` → `BRIDGE ACTIVE`
3. Klient dostaje IP z routera (nie 192.168.4.x)
4. `DHCP ACK sniffed` + `AP IP set to <podsieć>.254`
5. GUI odpowiada pod `http://<podsieć>.254`
6. Drugi klient → `MAC-NAT learned: <IP> -> <MAC>`, oba mają internet
7. Wyjście ostatniego klienta → `MAC RESTORE` → `IDLE MODE`

Nie ma testów jednostkowych — logika jest nierozerwalnie spleciona ze
sterownikiem WiFi. Weryfikacja jest z konieczności na sprzęcie.

---

## 11. Pułapki i ograniczenia

- **Jedno radio.** STA i AP dzielą kanał (i pasmo). Throughput jest
  half-duplex: to samo radio miele ruch w górę i w dół.
- **MAC clone obsługuje jednego klienta.** Reszta przez MAC-NAT, max
  **8 wpisów** w tablicy (eviction LRU).
- **Checksum UDP zerowany** po ustawieniu flagi broadcast w DHCP. Legalne dla
  UDP/IPv4 (RFC 768: `checksum=0` = "nie policzono"). Nie da się przenieść na
  IPv6, gdzie zero jest zabronione.
- **DHCP klienta a STA.** W trybie bridge DHCP client na STA jest wyłączony
  (ten sam MAC = kolizja) i podstawiany jest dummy `169.254.1.1`.
  `ap_mirror_sta_ip()` celowo ignoruje link-local, czekając na sniff.
- **Brak uwierzytelniania w GUI.** Serwer HTTP jest otwarty dla każdego, kto
  jest w sieci — również dla klientów zbridgowanych do podsieci routera.
  Świadomy kompromis (zero-config), ale przy wystawianiu tego poza LAN
  trzeba dodać auth.
- **WPA2/WPA3 (tryb przejściowy) psuje kojarzenie na części telefonów.**
  Zaobserwowane na sprzęcie: przy `authmode 7` + `sae_pwe_h2e = BOTH` telefon
  próbuje SAE i kończy na
  `wifi:removing station <MAC> after unsuccessful auth/assoc`.
  Po zejściu na czyste WPA2-PSK (`authmode 3`) skojarzenie udaje się od razu —
  dlatego to jest domyślna wartość. WPA3 można włączyć w GUI, jeśli wszystkie
  klienty go poprawnie obsługują. Zmieniając tryb w GUI **zapomnij sieć
  w telefonie**, bo zapamiętany profil trzyma stare ustawienia bezpieczeństwa.
- **`esp_wifi_internal_*` to API prywatne.** Może zniknąć bez ostrzeżenia.
- **Log level = INFO.** Żaden `ESP_LOGI` nie leży w ścieżce per-pakiet
  (`macnat_learn` loguje tylko przy nowym wpisie), więc koszt jest pomijalny.
  Ustaw WARN w `sdkconfig.defaults`, jeśli chcesz cichy UART.

---

## 12. Wydajność — punkty odniesienia i sufit

| Platforma | Warunki | Przez bridge |
|---|---|---|
| ESP32-S3 | WiFi 4, HT40, filtr broadcast ON | **~40 Mb/s** |
| ESP32-C6 | WiFi 6, HE20, filtr broadcast ON | ~15 Mb/s |
| **ESP32-C5** | WiFi 6, **HE20** (20 MHz + 11ax) | **28 Mb/s** |
| **ESP32-C5** | WiFi 4, **HT40** (40 MHz + 11n) | **41 Mb/s** |

Zmierzone speedtestem, link telefon↔repeater raportowany jako 150 Mb/s
(HT40, 1 strumień, short GI).

### Dlaczego 40 MHz wygrywa, choć wyłącza 11ax

Pierwotnie założyłem, że HT40 nic nie zyska, bo surowy sufit C5 to ~130 Mb/s,
a HE20 daje 143 Mb/s PHY. **Pomiar to obalił**: 28 → 41 Mb/s, czyli +46%.
Sufit raw nie był tu ograniczeniem. Dlatego domyślne 5 GHz to teraz 40 MHz,
a 2.4 GHz zostaje na 20 MHz (HE + mniejsza wrażliwość na zatłoczone pasmo).

### To jest ograniczenie czasem antenowym, nie CPU

Sam fakt, że podwojenie szerokości kanału dało +46%, dowodzi że ogranicza
radio — pakiet kosztuje tyle samo cykli CPU niezależnie od tego, jak szybko
leci w powietrzu. Gdyby ograniczał CPU, szerszy kanał nie zmieniłby nic.

Rachunek: 150 Mb/s PHY → realnie ~90–100 Mb/s TCP jednokierunkowo. Bridge
przepuszcza każdy pakiet przez **to samo radio dwa razy**, więc sufit to
~45–50 Mb/s. 41 Mb/s to ~85% tego.

Do potwierdzenia na sprzęcie jest `CONFIG_REPEATER_CPU_STATS` (domyślnie OFF,
bo run-time stats dokładają narzut przy każdym przełączeniu kontekstu).
Włączone dopisuje do raportu statusu:

```
CPU: 34% busy, 66% idle  (idle near 0 = CPU bound, otherwise airtime bound)
```

### Czego NIE da się przeskoczyć na jednym C5

- **80 MHz nie istnieje.** Dokumentacja IDF wymienia C5 w grupie „supports
  Wi-Fi bandwidth HT20 or HT40". Maksimum to 40 MHz, 1 strumień → 150 Mb/s PHY,
  identycznie jak S3. Na samym PHY rate C5 **nie przebije S3**.
- **Jednoczesny dual-band nie istnieje.** Jedno radio = jeden kanał. W APSTA
  kanał STA ma priorytet i SoftAP jest przeciągany na kanał upstreamu.
- Przewaga 5 GHz jest wyłącznie w **czystości pasma** (mniej retransmisji),
  nie w przepustowości. W zatłoczonym 2.4 GHz może to dać +20–30%, w cichym
  otoczeniu prawie nic.

Espressif dla C5 (`iperf`, shield-box): UDP RX 71 / TX 64 Mb/s, raw 802.11
130 Mb/s → po podzieleniu przez half-duplex sufit bridge'a ~60–65 Mb/s.
Czyli zapas z 41 Mb/s jeszcze jest, ale do wyciągnięcia efektywnością
(kanał bez DFS, mniej zakłóceń), nie szerszym kanałem.

### Jedyna droga do realnego przeskoku: drugie radio

Upstream 5 GHz i AP 2.4 GHz jednocześnie wymaga dwóch radiów: C5 jako STA
w 5 GHz + drugi układ (np. C6) jako AP w 2.4 GHz, połączone przewodowo przez
SDIO/SPI (ESP-Hosted / `esp_wifi_remote`), z bridgem L2 przez ten link.
Wtedy half-duplex znika, bo każde radio obsługuje jeden kierunek, i throughput
powinien się mniej więcej podwoić. To osobny projekt: dwa firmware'y, transport
SDIO i przeniesienie MAC-NAT na drugą stronę linku.

Co już jest zoptymalizowane: `CONFIG_COMPILER_OPTIMIZATION_PERF`,
IRAM dla WiFi/lwIP, `LWIP_TCPIP_CORE_LOCKING`, bufory i okna AMPDU
skopiowane z `examples/wifi/iperf/sdkconfig.defaults.esp32c5`,
`MINIMAL_BUILD ON` w głównym `CMakeLists.txt`.
