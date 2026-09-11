# ESP32 WiFi Repeater (bez NAT)

> [🇬🇧 English README](README.md)

Repeater WiFi z **wieloklientowym MAC-NAT**, **web GUI** do konfiguracji w locie i przezroczystym L2 bridgingiem.

Główny target: **ESP32-C5** — WiFi 6 (802.11ax), dual-band 2.4 GHz + 5 GHz.
Wspierane też: **ESP32-C6** (WiFi 6, 2.4 GHz), **ESP32-S3**, **ESP32-C3**, **ESP32** (WiFi 4, 2.4 GHz).

> **Wymaga ESP-IDF v6.1** | [🤖 AGENTS.md](AGENTS.md)

## Jak działa

```
[Internet] ─── [Router/AP] ════WiFi════ [ESP32 Repeater] ════WiFi════ [Klienci]
                upstream AP          STA ◄──► AP              (ta sama podsieć!)
```

### Bez NAT — ta sama podsieć

Repeater **nie zmienia podsieci**. Wszyscy klienci (do 4) podłączeni do repeatera dostają IP z DHCP routera upstream, jakby byli podłączeni bezpośrednio.

**Mechanizm: MAC cloning + MAC-NAT + L2 bridging**

- **Primary client**: Repeater klonuje MAC pierwszego klienta na STA. Upstream AP myśli, że komunikuje się bezpośrednio z klientem. Przezroczysty bridge.
- **Dodatkowi klienci (MAC-NAT)**: Repeater przepisuje `src MAC` na sklonowany MAC upstream, a odpowiedzi kieruje po tablicy `IP→MAC` do właściwego klienta. DHCP broadcast flag jest automatycznie ustawiany dla non-primary klientów, żeby serwer DHCP odpowiadał broadcastem (unicast do chaddr byłby odrzucony przez WiFi HW filter).

### Obsługiwane SoC

| Cecha | ESP32-C5 | ESP32-C6 | ESP32-S3 | ESP32-C3 | ESP32 |
|---|---|---|---|---|---|
| WiFi | WiFi 6 (802.11ax) | WiFi 6 (802.11ax) | WiFi 4 (802.11n) | WiFi 4 (802.11n) | WiFi 4 (802.11b/g/n) |
| Pasma | **2.4 + 5 GHz** | 2.4 GHz | 2.4 GHz | 2.4 GHz | 2.4 GHz |
| CPU | RISC-V 240 MHz single-core + LP | RISC-V 160 MHz single-core | Xtensa LX7 240 MHz dual-core | RISC-V 160 MHz single-core | Xtensa LX6 240 MHz dual-core |
| Domyślne bandwidth | HE20 @ 2.4 GHz, HT40 @ 5 GHz | HE20 | HT40 | HT40 | HT40 |
| PSRAM | Brak | Brak | Opcjonalny (nieużywany) | Brak | Opcjonalny (nieużywany) |

Kompatybilność wsteczna: klienci WiFi 4/5/6 łączą się bez problemu ze wszystkimi wariantami.

## 5 GHz i WiFi 6 (ESP32-C5)

C5 ma **jedno radio**. `WIFI_BAND_MODE_AUTO` **nie znaczy** jednoczesnego
dual-bandu — znaczy „wybierz pasmo automatycznie". W trybie APSTA kanał STA ma
wyższy priorytet, więc SoftAP jest przenoszony na kanał (i pasmo) upstreamu
po połączeniu STA.

Do ustawienia w `menuconfig` albo w web GUI:

| Ustawienie | Domyślnie | Uwagi |
|---|---|---|
| Band mode | 2.4 GHz + 5 GHz (auto) | albo tylko 2.4 GHz / tylko 5 GHz |
| Bandwidth 2.4 GHz | 20 MHz | zachowuje HE (11ax); 2.4 GHz jest zwykle zatłoczone |
| Bandwidth 5 GHz | 40 MHz | zmierzone 46% szybciej niż HE20, kosztem 11ax |
| Margines preferencji 5 GHz | 10 dB | AP 5 GHz o tym samym SSID wygrywa, dopóki jego RSSI nie jest gorszy o więcej niż tyle dB |

### 40 MHz albo 11ax — trzeba wybrać

40 MHz i HE/VHT **wykluczają się** w sterowniku ESP: `WIFI_BW40` jest przyjmowane
tylko wtedy, gdy w masce protokołów danego pasma nie ma ani `11AX`, ani `11AC`.
Sterownik mówi to sam przy starcie SoftAP:

```
W wifi:11ax/11ac mode can not work under phy bw 40M, the softap 5G bandwidth changed to 20M
```

Wybór 40 MHz w menuconfig albo w GUI sprawia więc, że firmware automatycznie
zdejmuje 11ax/11ac z maski tego pasma i loguje ostrzeżenie.

Które jest szybsze, to kwestia pomiaru — i na C5 wygrywa szerszy kanał:

| Konfiguracja | PHY rate @ 1 strumień | Przez bridge |
|---|---|---|
| HE20 (20 MHz + 11ax) | 143 Mb/s | 28 Mb/s |
| HT40 (40 MHz + 11n) | 150 Mb/s | **41 Mb/s** |

PHY rate jest praktycznie identyczny, więc te +46% pochodzą z czasu antenowego,
a nie z nominalnej szybkości. Dlatego 5 GHz ma tu domyślnie 40 MHz. 2.4 GHz
zostaje na 20 MHz, bo to pasmo jest zwykle zatłoczone i kanał 40 MHz traci tam
na zakłóceniach więcej, niż zyskuje na szerokości — przestaw w GUI, jeśli masz
u siebie ciche 2.4 GHz.

## Web GUI

Repeater posiada wbudowaną stronę konfiguracyjną — zmiana ustawień bez rekompilacji.

### Jak się dostać

| Stan | Adres GUI | Jak |
|---|---|---|
| **Przed połączeniem z routerem** | `http://192.168.4.1` | Połącz się z AP repeatera, dostajesz IP z jego DHCP (192.168.4.x) |
| **Po połączeniu (bridge aktywny)** | `http://<podsieć>.254` | ESP sniffuje DHCP ACK i ustawia AP na najwyższy wolny IP w podsieci klienta (np. `http://192.168.8.254`) |

> **Zero konfiguracji ręcznej** — nie trzeba zmieniać ustawień IP na telefonie/laptopie.

### Co można ustawić w GUI

- SSID i hasło upstream AP (do którego się łączymy)
- SSID i hasło repeatera (naszego AP)
- Moc nadawania (TX power)
- Maksymalna liczba klientów
- Tryb uwierzytelniania AP (WPA / WPA2 / WPA/WPA2 / WPA2/WPA3 / WPA3)
- **Band mode** (2.4 + 5 GHz auto / tylko 2.4 GHz / tylko 5 GHz — ESP32-C5)
- **Bandwidth per pasmo** (20 / 40 MHz) i **margines preferencji 5 GHz** (ESP32-C5)
- Klonowanie SSID upstream (AP repeater przejmuje nazwę sieci routera)
- Pseudo-mesh roaming (próg RSSI + histereza)
- Reset do ustawień domyślnych

Karta statusu pokazuje też stan łącza na żywo: pasmo, wynegocjowane PHY
(11ax / 11ac / 11n) i szerokość kanału.

Ustawienia zapisywane w **NVS** (pamięć nieulotna) — przetrwają restart i reflash.

GUI włączane/wyłączane w `menuconfig` → `REPEATER_HTTPD_ENABLE`.

## Konfiguracja (menuconfig)

```bash
idf.py menuconfig
```

W menu **"WiFi Repeater Configuration"**:

| Ustawienie | Opis | Domyślna wartość |
|---|---|---|
| Upstream AP SSID | SSID sieci do której się łączymy | MyUpstreamAP |
| Upstream AP Password | Hasło upstream AP | password123 |
| Repeater AP SSID | SSID naszego repeatera | MyRepeater1 |
| Repeater AP Password | Hasło repeatera | repeater123 |
| Max connected clients | Max klientów | 4 |
| AP Authentication Mode | Tryb uwierzytelniania AP | WPA2-PSK |
| Clone upstream SSID | AP przejmuje SSID routera | Nie |
| TX Power (dBm) | Moc nadawania | 20 |
| WiFi band mode | Wybór pasma 2.4 / 5 GHz (SoC z 5 GHz) | 2.4 + 5 GHz (auto) |
| 2.4 GHz bandwidth | 20 MHz zachowuje 11ax; 40 MHz go wyłącza | 20 MHz |
| 5 GHz bandwidth | 20 MHz zachowuje 11ax/11ac; 40 MHz je wyłącza, ale jest szybsze | 40 MHz |
| Prefer 5 GHz AP margin | O ile dB RSSI AP 5 GHz może być gorszy i nadal wygrać | 10 dB |
| Enable pseudo-mesh roaming | Roaming do lepszego AP z tym samym SSID | Nie |
| Roaming RSSI threshold | Próg RSSI do rozpoczęcia skanowania | -70 dBm |
| Roaming hysteresis | Nowy AP musi być lepszy o tyle dB | 8 dB |
| Enable HTTP config GUI | Web GUI do konfiguracji | Tak |
| HTTP server port | Port serwera HTTP | 80 |
| Filter broadcast/multicast | Pomijaj lwIP dla nie-ARP broadcastów (szybciej) | Tak |

> Wartości z menuconfig są **domyślne** — nadpisywane przez NVS / web GUI po pierwszym zapisie.

## Budowanie i flashowanie

```bash
# ESP32-C5 (WiFi 6, dual-band 2.4 + 5 GHz) — główny target
idf.py set-target esp32c5
idf.py build
idf.py -p COMx flash monitor

# ESP32-C6 (WiFi 6, 2.4 GHz)
idf.py set-target esp32c6
idf.py build
idf.py -p COMx flash monitor

# ESP32-S3 / ESP32-C3 / ESP32 classic (WiFi 4)
idf.py set-target esp32s3      # albo esp32c3 / esp32
idf.py build
idf.py -p COMx flash monitor
```

`sdkconfig` jest generowany i gitignorowany — źródłem prawdy są
`sdkconfig.defaults` + `sdkconfig.defaults.<target>`. Po edycji któregoś z nich
skasuj `sdkconfig` i przebuduj.

Osobliwości USB-Serial/JTAG na ESP32-C5 (routing konsoli, zachowanie resetu,
odzyskiwanie po zawieszeniu) są opisane w [AGENTS.md](AGENTS.md) — oszczędzą
kilka godzin.

## Szybki start

1. **Flash** firmware na ESP32-C5, C6, S3, C3 lub ESP32
2. **Połącz się** z siecią WiFi `MyRepeater1` (hasło: `repeater123`)
3. **Otwórz** `http://192.168.4.1` w przeglądarce
4. **Wpisz** SSID i hasło swojego routera → **Save & Reboot**
5. Repeater łączy się z routerem. Po podłączeniu klienta, GUI dostępne pod `http://<podsieć>.254` (np. `192.168.8.254`)
6. **Podłącz kolejne urządzenia** — do 4 klientów jednocześnie, wszystkie dostają IP z routera

## Multi-client (MAC-NAT)

Repeater obsługuje **do 4 klientów jednocześnie** mimo ograniczenia jednego MAC na STA:

```
┌──────────────┐          ┌─────────────────────────┐          ┌────────────┐
│   Router     │          │      ESP32 Repeater      │          │  Klient 1  │
│              │◄────────►│  STA (MAC=klient1)       │◄────────►│  (primary) │
│  Widzi       │          │                          │          └────────────┘
│  jeden MAC   │          │  Tablica MAC-NAT:        │          ┌────────────┐
│              │          │  IP_2 → MAC_klient2      │◄────────►│  Klient 2  │
│              │          │  IP_3 → MAC_klient3      │          └────────────┘
│              │          │  IP_4 → MAC_klient4      │          ┌────────────┐
└──────────────┘          └─────────────────────────┘◄────────►│  Klient 3  │
                                                               └────────────┘
```

**Upstream (klient → router):**
- Src MAC klientów 2-4 przepisywany na sklonowany MAC (primary)
- Router widzi jeden MAC, niezależnie od liczby klientów
- Tablica IP→MAC uczona z pakietów klientów (IPv4 src, ARP sender, DHCP chaddr)

**Downstream (router → klient):**
- Dst MAC przepisywany z sklonowanego na prawdziwy MAC klienta (lookup po dst IP)
- ARP target hardware address również przepisywany

**DHCP broadcast flag:**
- Non-primary klienci mają ustawiony broadcast flag w DHCP Discover/Request
- Router odpowiada broadcastem zamiast unicastem do chaddr
- Zapobiega odrzuceniu przez WiFi HW filter (STA MAC ≠ chaddr)
- UDP checksum zerowany po modyfikacji (RFC 768: checksum=0 = "not computed")

### Niezawodność

- **Licznik klientów** oparty na `esp_wifi_ap_get_sta_list()` zamiast manualnych ++/-- (odporny na duplikaty event leave z SA Query timeout)
- **Auto-clone po restore**: jeśli klient dołączy podczas przywracania MAC (3s okno), repeater automatycznie klonuje MAC po zakończeniu restore
- **Re-clone przy odejściu primary**: jeśli primary client odchodzi a inni zostają, MAC jest re-klonowany pod pierwszego dostępnego klienta

## AP Clone SSID

Gdy włączone (`Clone upstream SSID` w GUI lub `REPEATER_AP_CLONE_SSID` w menuconfig), repeater **automatycznie kopiuje SSID upstream AP** na swój AP po połączeniu STA. Klienci widzą tę samą nazwę sieci co router — repeater działa przezroczyście.

- Pole "Repeater AP SSID" jest wtedy ignorowane
- SSID aktualizowane dynamicznie po każdym połączeniu STA
- Przydatne do rozszerzania zasięgu istniejącej sieci bez zmiany nazwy

## Pseudo-mesh roaming

Gdy włączone (`REPEATER_PSEUDO_MESH` w menuconfig lub checkbox w GUI), repeater monitoruje jakość sygnału upstream AP i automatycznie przełącza się na lepszy AP z tym samym SSID:

1. **Monitoring** — co 10 sekund sprawdza RSSI upstream AP
2. **Skanowanie** — jeśli RSSI < próg (domyślnie -70 dBm), skanuje w poszukiwaniu AP z tym samym SSID
3. **Filtrowanie BSSID** — pomija własny AP (`s_ap_mac`) żeby nie połączyć się sam do siebie
4. **Histereza** — nowy AP musi mieć RSSI lepszy o co najmniej `hysteresis` dB (domyślnie 8) od obecnego
5. **Roaming** — rozłącza STA i łączy z nowym BSSID, po roamingu 30s cooldown

Idealny dla scenariuszy z wieloma routerami/AP z tym samym SSID (mesh, roaming między piętrami itp.).

> **Uwaga**: Na ESP32-C6 z wbudowaną anteną PCB zasięg jest ograniczony — próg RSSI warto dostosować do warunków.

## Ograniczenia

- ESP32 ma **jedno radio** — STA i AP muszą pracować na tym samym kanale i w tym samym pasmie (automatycznie dopasowywane do upstreamu)
- STA MAC sklonowany pod jednego klienta (primary) — dodatkowi klienci obsługiwani przez MAC-NAT
- Maksymalnie **8 wpisów** w tablicy MAC-NAT (LRU eviction)
- Web GUI **nie ma uwierzytelniania** — dostanie się do niego każdy, kto jest w sieci. Świadomy kompromis (zero-config); przed wystawieniem poza zaufany LAN dodaj auth.
- `esp_wifi_internal_reg_rxcb` to prywatne API ESP-IDF — może się zmienić w przyszłych wersjach

### Zmierzony throughput

Ruch przechodzi przez to samo radio dwa razy (w górę i w dół), więc bridge
realnie oddaje mniej więcej połowę tego, co radio potrafi przepchnąć.

| Platforma | Warunki | Przez bridge |
|---|---|---|
| ESP32-S3 | WiFi 4, HT40, filtr broadcast WŁ | ~40 Mb/s |
| ESP32-C6 | WiFi 6, HE20, filtr broadcast WŁ | ~15 Mb/s |
| **ESP32-C5** | 2.4 GHz, **HT40** (40 MHz + 11n), link 150 Mb/s | **41 Mb/s** |
| **ESP32-C5** | 5 GHz, **HE20** (20 MHz + 11ax), link 72 Mb/s | **27,6 / 24,1 Mb/s** (down/up) |
| ESP32-C5 | 2.4 GHz, HT20, link 72 Mb/s | 28 Mb/s |

Zmierzone przez speedtest.net; szybkości linku odczytane z Pixela 7.

Implementacja jest blisko sufitu: 27,6 Mb/s przy linku 72 Mb/s to 77% z 36 Mb/s,
na które pozwala half-duplex, bo każdy pakiet przechodzi przez to samo radio dwa
razy. Prawie symetryczny upload to potwierdza — w obu kierunkach radio wykonuje
tę samą pracę. Zostaje narzut 802.11, na który kod aplikacji nie ma wpływu.

Czyli **przepustowość zależy od szybkości linku klienckiego**, a ta od szerokości
kanału (SoftAP i tak jest 11n, patrz wyżej).

To ograniczenie **czasem antenowym**, nie CPU: podwojenie szerokości kanału dało
+46%, co nie mogłoby się zdarzyć gdyby ograniczał procesor — pakiet kosztuje
tyle samo cykli niezależnie od tego, jak szybko leci w powietrzu. 150 Mb/s PHY
to realnie ~90–100 Mb/s TCP w jedną stronę, podzielone przez dwukrotne przejście
radiem daje sufit ~45–50 Mb/s, a 41 Mb/s to ~85% tego.

Włącz `REPEATER_CPU_STATS` w menuconfig, żeby raport statusu pokazywał udział
CPU i potwierdzić to na własnym sprzęcie (domyślnie wyłączone — run-time stats
FreeRTOS-a dokładają narzut przy każdym przełączeniu kontekstu).

### Dlaczego C5 nie przebija S3 na surowej szybkości

C5 kończy na **HT40 z jednym strumieniem**, tak samo jak S3 — ESP-IDF wymienia
go w grupie układów, które „support Wi-Fi bandwidth HT20 or HT40", więc nie ma
ani trybu 80 MHz, ani drugiego strumienia. 40 MHz w 5 GHz jest dokładnie tak
szybkie jak 40 MHz w 2.4 GHz.

Przewaga 5 GHz to **czystość pasma** — mniej retransmisji, mniej straconego
czasu antenowego. W zatłoczonym 2.4 GHz może to być warte 20–30%, w cichym
otoczeniu prawie nic.

Liczby Espressifu dla C5 w shield-boxie (`iperf`: UDP RX 71 / TX 64 Mb/s, surowy
sufit 802.11 130 Mb/s) dają sufit bridge'a ~60–65 Mb/s, więc zapas jeszcze jest
— ale musi pochodzić z efektywności (kanał bez DFS, mniej zakłóceń), nie
z szerszego kanału.

Przełamanie tego wymaga **dwóch radiów**: C5 jako stacja w 5 GHz plus drugi
układ (np. C6) jako AP w 2.4 GHz, połączone przewodowo przez SDIO/SPI
(ESP-Hosted / `esp_wifi_remote`) i zbridgowane na L2 przez ten link. Wtedy kara
half-duplex znika, bo każde radio obsługuje jeden kierunek.

## Architektura

```
┌──────────────────────────────────────────────────────┐
│                 ESP32 Repeater                       │
│                                                      │
│  ┌─────────┐  L2 Bridge + MAC-NAT  ┌──────────┐     │
│  │  STA    │◄──────────────────────►│   AP     │     │
│  │(WiFi6/4)│  MAC clone + rewrite   │(WiFi6/4) │     │
│  └────┬────┘                        └────┬─────┘     │
│       │                                  │           │
│  on_sta_rx():                       on_ap_rx():      │
│  - DHCP ACK sniffer (inline)        - MAC-NAT       │
│  - MAC-NAT downstream                 upstream      │
│  - forward → AP                     - forward → STA │
│  - broadcast/unicast → lwIP         - bcast → lwIP  │
│       │                                  │           │
│       │          ┌──────────┐            │           │
│       └──────────┤ HTTP GUI ├────────────┘           │
│                  │ NVS conf │                        │
│                  └──────────┘                        │
└───────┼──────────────────────────────┼───────────────┘
        │                              │
   Upstream AP                    Klienci WiFi
   (router)                      (phone, laptop, ...)
```

### DHCP ACK Sniffer

Podczas bridgingu STA DHCP jest wyłączony (MAC sklonowany = kolizja DHCP).
Pakiety DHCP routera przechodzą przezroczyście przez bridge do klienta.
Repeater **sniffuje DHCP ACK** w `on_sta_rx()`:

1. **Inline pre-check**: `EtherType=0x0800, UDP, port 67→68` (skip 99.9% pakietów bez function call)
2. Parsuje: `BOOTREPLY → magic cookie → option 53=ACK`
3. Wyciąga: **yiaddr** (IP klienta), **subnet mask**, **gateway**
4. Ustawia AP na `<podsieć>.254` (najwyższy wolny adres, omija IP klienta i gateway)
5. Uczy tablicę MAC-NAT z `chaddr` (MAC klienta w DHCP payload)
6. Po pierwszym ACK ustawia flagę `s_ap_ip_from_sniff` — kolejne ACK uczą MAC-NAT ale pomijają przeliczanie IP

Dzięki temu klient na `192.168.8.110` wchodzi na `http://192.168.8.254` — zero konfiguracji.

### Optymalizacje hot-path

Forwarding callbacks (`on_sta_rx`, `on_ap_rx`) są wywoływane dla **każdego pakietu L2**:

- **Filtr broadcast** (`CONFIG_REPEATER_BROADCAST_FILTER`, domyślnie WŁ): tylko ARP requesty do naszego IP trafiają do lwIP; reszta broadcast/multicast (mDNS, SSDP, NetBIOS, IGMP, IPv6) forwardowana na L2 ale pomijana przez lwIP — oszczędność ~10-20k cykli/pakiet, zmierzono ~13→15 Mb/s
- DHCP sniffer: inline EtherType+port check, function call tylko dla DHCP (0.1%)
- MAC-NAT: skip gdy `s_client_count <= 1` (single client = zero overhead)
- `macnat_learn()`: skip `esp_timer_get_time()` gdy IP+MAC bez zmian (hot path)
- Brak `IRAM_ATTR` ani `volatile` na counterach (single-core C6 — cache thrashing)
