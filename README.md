# ESP32-C6 WiFi 6 Repeater (bez NAT)

Repeater WiFi oparty na ESP32-C6 z obsługą **WiFi 6 (802.11ax)**, kompatybilnością wsteczną z WiFi 4/5, i **web GUI** do konfiguracji w locie.

## Jak działa

```
[Internet] ─── [Router/AP] ════WiFi════ [ESP32-C6 Repeater] ════WiFi════ [Klienci]
                upstream AP          STA ◄──► AP               (ta sama podsieć!)
```

### Bez NAT - ta sama podsieć

Repeater **nie zmienia podsieci**. Klienci podłączeni do repeatera dostają IP z DHCP routera upstream, jakby byli podłączeni bezpośrednio.

**Trick: MAC spoofing + L2 bridging**

- **1 klient**: Repeater klonuje MAC klienta na interfejsie STA. Upstream AP myśli, że komunikuje się bezpośrednio z klientem. Przezroczysty bridge.
- **Wielu klientów**: Repeater przepisuje nagłówki MAC w pakietach, przekazując ruch między interfejsami STA i AP na warstwie L2 (przed stosem TCP/IP).

### WiFi 6 (802.11ax)

ESP32-C6 natywnie wspiera WiFi 6. Włączone funkcje:
- **HE (High Efficiency)** - lepsza wydajność w zatłoczonych sieciach
- **OFDMA** - równoczesna komunikacja z wieloma urządzeniami
- **BSS Coloring** - redukcja interferencji z sąsiednimi AP
- **MCS 0-9** - wyższy throughput
- **WPA3-SAE** - nowoczesne szyfrowanie
- **HT40** - 40 MHz bandwidth

Kompatybilność wsteczna: klienci WiFi 4 (802.11n) i WiFi 5 (802.11ac) mogą się łączyć bez problemu.

## Web GUI

Repeater posiada wbudowaną stronę konfiguracyjną — zmiana ustawień bez rekompilacji.

### Jak się dostać

| Stan | Adres GUI | Jak |
|---|---|---|
| **Przed połączeniem z routerem** | `http://192.168.4.1` | Połącz się z AP repeatera, dostajesz IP z jego DHCP (192.168.4.x) |
| **Po połączeniu (bridge aktywny)** | `http://<podsieć>.254` | ESP sniffuje DHCP ACK i ustawia AP na najwyższy wolny IP w podsieci klienta (np. `http://192.168.1.254`) |

> **Zero konfiguracji ręcznej** — nie trzeba zmieniać ustawień IP na telefonie/laptopie.

### Co można ustawić w GUI

- SSID i hasło upstream AP (do którego się łączymy)
- SSID i hasło repeatera (naszego AP)
- Moc nadawania (TX power)
- Maksymalna liczba klientów
- Reset do ustawień domyślnych

Ustawienia zapisywane w **NVS** (pamięć nieulotna) — przetrwają restart i reflash.

GUI włączane/wyłączane w `menuconfig` → `REPEATER_HTTPD_ENABLE`.

## Konfiguracja (menuconfig)

```bash
idf.py menuconfig
```

W menu **"WiFi 6 Repeater Configuration"**:

| Ustawienie | Opis | Domyślna wartość |
|---|---|---|
| Upstream AP SSID | SSID sieci do której się łączymy | MyUpstreamAP |
| Upstream AP Password | Hasło upstream AP | password123 |
| Repeater AP SSID | SSID naszego repeatera | MyRepeater |
| Repeater AP Password | Hasło repeatera | repeater123 |
| Max connected clients | Max klientów | 4 |
| TX Power (dBm) | Moc nadawania | 20 |
| Enable HTTP config GUI | Web GUI do konfiguracji | Tak |
| HTTP server port | Port serwera HTTP | 80 |

> Wartości z menuconfig są **domyślne** — nadpisywane przez NVS / web GUI po pierwszym zapisie.

## Budowanie i flashowanie

```bash
idf.py set-target esp32c6
idf.py build
idf.py -p COMx flash monitor
```

## Szybki start

1. **Flash** firmware na ESP32-C6
2. **Połącz się** z siecią WiFi `MyRepeater` (hasło: `repeater123`)
3. **Otwórz** `http://192.168.4.1` w przeglądarce
4. **Wpisz** SSID i hasło swojego routera → **Save & Reboot**
5. Repeater łączy się z routerem. Po podłączeniu klienta, GUI dostępne pod `http://<podsieć>.254` (np. `192.168.1.254`)

## Ograniczenia

- ESP32-C6 ma **jedno radio** — STA i AP muszą pracować na tym samym kanale (automatycznie dopasowywany do upstream AP)
- Maksymalny throughput jest dzielony między upstream i downstream (half-duplex repeating)
- Dla najlepszej wydajności z jednym klientem, MAC spoofing zapewnia przezroczysty bridge
- Z wieloma klientami, wydajność może być nieco niższa przez przepisywanie nagłówków
- `esp_wifi_internal_reg_rxcb` to wewnętrzne API ESP-IDF — może się zmienić w przyszłych wersjach

## Architektura

```
┌─────────────────────────────────────────────────┐
│              ESP32-C6 Repeater                  │
│                                                 │
│  ┌─────────┐  L2 Bridge  ┌──────────┐          │
│  │  STA    │◄────────────►│   AP     │          │
│  │ (WiFi6) │  MAC spoof   │ (WiFi6)  │          │
│  └────┬────┘              └────┬─────┘          │
│       │                       │                 │
│  esp_wifi_internal_reg_rxcb() │   ┌──────────┐  │
│  Przechwytywanie pakietów L2  │   │ HTTP GUI │  │
│  + DHCP ACK sniffer           │   │ NVS conf │  │
│  przed stosem TCP/IP          │   └──────────┘  │
└───────┼───────────────────────┼─────────────────┘
        │                       │
   Upstream AP             Klienci WiFi
   (router)               (phone, laptop...)
```

### DHCP ACK Sniffer

Podczas bridgingu STA DHCP jest wyłączony (MAC sklonowany = kolizja DHCP).
Pakiety DHCP routera przechodzą przezroczyście przez bridge do klienta.
Repeater **sniffuje DHCP ACK** w `on_sta_rx()`:

1. Parsuje: `EtherType=IPv4 → UDP:67→68 → BOOTREPLY → cookie → option 53=ACK`
2. Wyciąga: **yiaddr** (IP klienta), **subnet mask**, **gateway**
3. Ustawia AP na `<podsieć>.254` (najwyższy wolny adres, omija IP klienta i gateway)

Dzięki temu klient na `192.168.1.50` wchodzi na `http://192.168.1.254` — zero konfiguracji.
