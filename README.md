# ESP32-C6 WiFi 6 Repeater (bez NAT)

Repeater WiFi oparty na ESP32-C6 z obsługą **WiFi 6 (802.11ax)** i kompatybilnością wsteczną z WiFi 4/5.

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

## Konfiguracja

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

## Budowanie i flashowanie

```bash
idf.py set-target esp32c6
idf.py build
idf.py -p COMx flash monitor
```

## Ograniczenia

- ESP32-C6 ma **jedno radio** - STA i AP muszą pracować na tym samym kanale (automatycznie dopasowywany do upstream AP)
- Maksymalny throughput jest dzielony między upstream i downstream (half-duplex repeating)
- Dla najlepszej wydajności z jednym klientem, MAC spoofing zapewnia przezroczysty bridge
- Z wieloma klientami, wydajność może być nieco niższa przez przepisywanie nagłówków
- `esp_wifi_internal_reg_rxcb` to wewnętrzne API ESP-IDF - może się zmienić w przyszłych wersjach

## Architektura

```
┌─────────────────────────────────────────┐
│           ESP32-C6 Repeater             │
│                                         │
│  ┌─────────┐  L2 Bridge  ┌──────────┐  │
│  │  STA    │◄────────────►│   AP     │  │
│  │ (WiFi6) │  MAC spoof   │ (WiFi6)  │  │
│  └────┬────┘              └────┬─────┘  │
│       │                       │         │
│  esp_wifi_internal_reg_rxcb() │         │
│  Przechwytywanie pakietów L2  │         │
│  przed stosem TCP/IP          │         │
└───────┼───────────────────────┼─────────┘
        │                       │
   Upstream AP             Klienci WiFi
   (router)               (phone, laptop...)
```
