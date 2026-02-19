# ESP32 WiFi Repeater (no NAT)

WiFi repeater based on **ESP32-C6** (WiFi 6 / 802.11ax), **ESP32-S3** (WiFi 4 / 802.11n), **ESP32-C3** (WiFi 4 / 802.11n) or **ESP32** (WiFi 4 / 802.11b/g/n), with **multi-client MAC-NAT**, **web GUI** for on-the-fly configuration and transparent L2 bridging.

> **Tested on ESP-IDF v5.5.1** | [🇵🇱 Polish README](README.pl.md)

## How it works

```
[Internet] ─── [Router/AP] ════WiFi════ [ESP32 Repeater] ════WiFi════ [Clients]
                upstream AP          STA ◄──► AP              (same subnet!)
```

### No NAT — same subnet

The repeater **does not change the subnet**. All clients (up to 4) connected to the repeater get IPs from the upstream router's DHCP, as if connected directly.

**Mechanism: MAC cloning + MAC-NAT + L2 bridging**

- **Primary client**: The repeater clones the first client's MAC to the STA. The upstream AP thinks it's communicating directly with the client. Transparent bridge.
- **Additional clients (MAC-NAT)**: The repeater rewrites `src MAC` to the cloned upstream MAC, and routes responses via an `IP→MAC` table to the correct client. DHCP broadcast flag is automatically set for non-primary clients so the DHCP server replies with broadcast (unicast to chaddr would be rejected by WiFi HW filter).

### Supported SoCs

| Feature | ESP32-C6 | ESP32-S3 | ESP32-C3 | ESP32 |
|---|---|---|---|---|
| WiFi | WiFi 6 (802.11ax) | WiFi 4 (802.11n) | WiFi 4 (802.11n) | WiFi 4 (802.11b/g/n) |
| CPU | RISC-V 160 MHz single-core | Xtensa LX7 240 MHz dual-core | RISC-V 160 MHz single-core | Xtensa LX6 240 MHz dual-core |
| Bandwidth | HT20 (required for HE) | HT40 | HT40 | HT40 |
| PSRAM | No | Optional (unused) | No | Optional (unused) |

Backwards compatible: WiFi 4/5/6 clients connect without issues to all variants.

## Web GUI

The repeater has a built-in configuration page — change settings without recompiling.

### How to access

| State | GUI address | How |
|---|---|---|
| **Before connecting to router** | `http://192.168.4.1` | Connect to the repeater's AP, get IP from its DHCP (192.168.4.x) |
| **After connecting (bridge active)** | `http://<subnet>.254` | ESP sniffs DHCP ACK and sets AP to the highest free IP in the client's subnet (e.g. `http://192.168.8.254`) |

> **Zero manual configuration** — no need to change IP settings on phone/laptop.

### Configurable settings

- Upstream AP SSID and password
- Repeater AP SSID and password
- TX power
- Maximum number of clients
- AP authentication mode (WPA / WPA2 / WPA/WPA2 / WPA2/WPA3 / WPA3)
- Upstream SSID cloning (repeater AP takes over the router's network name)
- Pseudo-mesh roaming (RSSI threshold + hysteresis)
- Reset to defaults

Settings saved in **NVS** (non-volatile storage) — survive restart and reflash.

GUI can be enabled/disabled in `menuconfig` → `REPEATER_HTTPD_ENABLE`.

## Configuration (menuconfig)

```bash
idf.py menuconfig
```

In the **"WiFi Repeater Configuration"** menu:

| Setting | Description | Default value |
|---|---|---|
| Upstream AP SSID | SSID of the network to connect to | MyUpstreamAP |
| Upstream AP Password | Upstream AP password | password123 |
| Repeater AP SSID | Our repeater's SSID | MyRepeater |
| Repeater AP Password | Repeater password | repeater123 |
| Max connected clients | Max clients | 4 |
| AP Authentication Mode | AP auth mode | WPA2/WPA3-PSK |
| Clone upstream SSID | AP takes over router's SSID | No |
| TX Power (dBm) | TX power | 20 |
| Enable pseudo-mesh roaming | Roam to better AP with same SSID | No |
| Roaming RSSI threshold | RSSI threshold to start scanning | -70 dBm |
| Roaming hysteresis | New AP must be better by this many dB | 8 dB |
| Enable HTTP config GUI | Web GUI for configuration | Yes |
| HTTP server port | HTTP server port | 80 |
| Filter broadcast/multicast | Skip lwIP for non-ARP broadcast (faster) | Yes |

> Values from menuconfig are **defaults** — overridden by NVS / web GUI after first save.

## Build and flash

```bash
# ESP32-C6 (WiFi 6)
idf.py set-target esp32c6
idf.py build
idf.py -p COMx flash monitor

# ESP32-S3 (WiFi 4)
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor

# ESP32-C3 (WiFi 4)
idf.py set-target esp32c3
idf.py build
idf.py -p COMx flash monitor

# ESP32 classic (WiFi 4)
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

## Quick start

1. **Flash** firmware to ESP32-C6, ESP32-S3, ESP32-C3 or ESP32
2. **Connect** to WiFi network `MyRepeater` (password: `repeater123`)
3. **Open** `http://192.168.4.1` in a browser
4. **Enter** your router's SSID and password → **Save & Reboot**
5. The repeater connects to the router. After a client connects, GUI is available at `http://<subnet>.254` (e.g. `192.168.8.254`)
6. **Connect more devices** — up to 4 clients simultaneously, all getting IPs from the router

## Multi-client (MAC-NAT)

The repeater supports **up to 4 clients simultaneously** despite the single MAC limitation on STA:

```
┌──────────────┐          ┌─────────────────────────┐          ┌────────────┐
│   Router     │          │      ESP32 Repeater      │          │  Client 1  │
│              │◄────────►│  STA (MAC=client1)       │◄────────►│  (primary) │
│  Sees        │          │                          │          └────────────┘
│  one MAC     │          │  MAC-NAT table:          │          ┌────────────┐
│              │          │  IP_2 → MAC_client2      │◄────────►│  Client 2  │
│              │          │  IP_3 → MAC_client3      │          └────────────┘
│              │          │  IP_4 → MAC_client4      │          ┌────────────┐
└──────────────┘          └─────────────────────────┘◄────────►│  Client 3  │
                                                               └────────────┘
```

**Upstream (client → router):**
- Src MAC of clients 2-4 rewritten to cloned MAC (primary)
- Router sees one MAC regardless of client count
- IP→MAC table learned from client packets (IPv4 src, ARP sender, DHCP chaddr)

**Downstream (router → client):**
- Dst MAC rewritten from cloned to real client MAC (lookup by dst IP)
- ARP target hardware address also rewritten

**DHCP broadcast flag:**
- Non-primary clients have broadcast flag set in DHCP Discover/Request
- Router responds with broadcast instead of unicast to chaddr
- Prevents rejection by WiFi HW filter (STA MAC ≠ chaddr)
- UDP checksum zeroed after modification (RFC 768: checksum=0 = "not computed")

### Reliability

- **Client counter** based on `esp_wifi_ap_get_sta_list()` instead of manual ++/-- (resistant to duplicate leave events from SA Query timeout)
- **Auto-clone after restore**: if a client joins during MAC restore (3s window), the repeater automatically clones MAC after restore completes
- **Re-clone on primary leave**: if primary client leaves while others remain, MAC is re-cloned to the first available client

## AP Clone SSID

When enabled (`Clone upstream SSID` in GUI or `REPEATER_AP_CLONE_SSID` in menuconfig), the repeater **automatically copies the upstream AP's SSID** to its own AP after STA connects. Clients see the same network name as the router — the repeater acts transparently.

- The "Repeater AP SSID" field is then ignored
- SSID updated dynamically after each STA connection
- Useful for extending an existing network without changing its name

## Pseudo-mesh roaming

When enabled (`REPEATER_PSEUDO_MESH` in menuconfig or checkbox in GUI), the repeater monitors upstream AP signal quality and automatically switches to a better AP with the same SSID:

1. **Monitoring** — checks upstream AP RSSI every 10 seconds
2. **Scanning** — if RSSI < threshold (default -70 dBm), scans for APs with the same SSID
3. **BSSID filtering** — skips own AP (`s_ap_mac`) to avoid connecting to itself
4. **Hysteresis** — new AP must have RSSI at least `hysteresis` dB better than current (default 8)
5. **Roaming** — disconnects STA and connects to new BSSID, 30s cooldown after roaming

Ideal for scenarios with multiple routers/APs sharing the same SSID (mesh, floor-to-floor roaming, etc.).

> **Note**: On ESP32-C6 with built-in PCB antenna range is limited — RSSI threshold should be tuned to conditions.

## Limitations

- ESP32 has **one radio** — STA and AP must operate on the same channel (automatically matched)
- Throughput shared between upstream and downstream (half-duplex) — realistically **~15 Mbps** (ESP32-C6, `-O2`, WiFi 6 HT20, broadcast filter ON). ESP32-C3/S3/ESP32 achieve similar results (WiFi 4 HT40)
- STA MAC cloned for one client (primary) — additional clients handled via MAC-NAT
- Maximum **8 entries** in MAC-NAT table (LRU eviction)
- `esp_wifi_internal_reg_rxcb` is an internal ESP-IDF API — may change in future versions

## Architecture

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
│  - DHCP ACK sniffer (inline)        - MAC-NAT        │
│  - MAC-NAT downstream                 upstream       │
│  - forward → AP                     - forward → STA  │
│  - broadcast/unicast → lwIP         - bcast → lwIP   │
│       │                                  │           │
│       │          ┌──────────┐            │           │
│       └──────────┤ HTTP GUI ├────────────┘           │
│                  │ NVS conf │                        │
│                  └──────────┘                        │
└───────┼──────────────────────────────┼───────────────┘
        │                              │
   Upstream AP                      WiFi clients
   (router)                      (phone, laptop, ...)
```

### DHCP ACK Sniffer

During bridging, STA DHCP is disabled (MAC cloned = DHCP collision).
Router DHCP packets pass transparently through the bridge to the client.
The repeater **sniffs DHCP ACK** in `on_sta_rx()`:

1. **Inline pre-check**: `EtherType=0x0800, UDP, port 67→68` (skips 99.9% of packets without function call)
2. Parses: `BOOTREPLY → magic cookie → option 53=ACK`
3. Extracts: **yiaddr** (client IP), **subnet mask**, **gateway**
4. Sets AP to `<subnet>.254` (highest free address, skips client IP and gateway)
5. Learns MAC-NAT table from `chaddr` (client MAC in DHCP payload)
6. After first ACK sets `s_ap_ip_from_sniff` flag — subsequent ACKs update MAC-NAT but skip IP recalculation

This way a client at `192.168.8.110` opens `http://192.168.8.254` — zero configuration.

### Hot-path optimizations

Forwarding callbacks (`on_sta_rx`, `on_ap_rx`) are called for **every L2 packet**:

- **Broadcast filter** (`CONFIG_REPEATER_BROADCAST_FILTER`, default ON): only ARP requests for our IP enter lwIP; all other broadcast/multicast (mDNS, SSDP, NetBIOS, IGMP, IPv6) forwarded at L2 but skipped by lwIP — saves ~10-20k cycles/packet, measured ~13→15 Mb/s
- DHCP sniffer: inline EtherType+port check, function call only for DHCP (0.1%)
- MAC-NAT: skip when `s_client_count <= 1` (single client = zero overhead)
- `macnat_learn()`: skip `esp_timer_get_time()` when IP+MAC unchanged (hot path)
- No `IRAM_ATTR` or `volatile` on counters (single-core C6 — avoids cache thrashing)
