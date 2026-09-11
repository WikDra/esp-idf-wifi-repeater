# ESP32 WiFi Repeater (no NAT)

WiFi repeater with **multi-client MAC-NAT**, a **web GUI** for on-the-fly configuration and transparent L2 bridging.

Primary target: **ESP32-C5** — WiFi 6 (802.11ax), dual-band 2.4 GHz + 5 GHz.
Also supported: **ESP32-C6** (WiFi 6, 2.4 GHz), **ESP32-S3**, **ESP32-C3**, **ESP32** (WiFi 4, 2.4 GHz).

> **Requires ESP-IDF v6.1** | [🇵🇱 Polish README](README.pl.md) | [🤖 AGENTS.md](AGENTS.md)

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

| Feature | ESP32-C5 | ESP32-C6 | ESP32-S3 | ESP32-C3 | ESP32 |
|---|---|---|---|---|---|
| WiFi | WiFi 6 (802.11ax) | WiFi 6 (802.11ax) | WiFi 4 (802.11n) | WiFi 4 (802.11n) | WiFi 4 (802.11b/g/n) |
| Bands | **2.4 + 5 GHz** | 2.4 GHz | 2.4 GHz | 2.4 GHz | 2.4 GHz |
| CPU | RISC-V 240 MHz single-core + LP | RISC-V 160 MHz single-core | Xtensa LX7 240 MHz dual-core | RISC-V 160 MHz single-core | Xtensa LX6 240 MHz dual-core |
| Default bandwidth | HE20 @ 2.4 GHz, HT40 @ 5 GHz | HE20 | HT40 | HT40 | HT40 |
| PSRAM | No | No | Optional (unused) | No | Optional (unused) |

Backwards compatible: WiFi 4/5/6 clients connect without issues to all variants.

## 5 GHz and WiFi 6 (ESP32-C5)

The C5 has **one radio**. `WIFI_BAND_MODE_AUTO` does **not** mean simultaneous
dual-band — it means "pick a band automatically". In APSTA mode the STA channel
takes priority, so the SoftAP is moved to the upstream's channel (and band)
once the STA connects.

Configurable via `menuconfig` or the web GUI:

| Setting | Default | Notes |
|---|---|---|
| Band mode | 2.4 GHz + 5 GHz (auto) | or 2.4 GHz only / 5 GHz only |
| 2.4 GHz bandwidth | 20 MHz | 40 MHz works here and is faster — see below |
| 5 GHz bandwidth | 20 MHz | **do not use 40 MHz**, it breaks client association |
| Prefer 5 GHz margin | 10 dB | a 5 GHz AP with the same SSID wins unless its RSSI is more than this many dB worse |

### 5 GHz SoftAP must stay at 20 MHz

Verified on a C5 with a Pixel 7: with the SoftAP on 5 GHz at 40 MHz the client
associates and is then dropped after about four seconds, every single time:

```
I wifi:station: ... join, AID=1, an, 40D
I wifi:station: ... leave, AID = 1, reason = 15
```

`reason 15` is `4WAY_HANDSHAKE_TIMEOUT` — association succeeds but the WPA2 key
exchange never completes. Changing only the channel width to 20 MHz makes the
same phone connect on the first attempt. 40 MHz at 2.4 GHz is fine.

### 40 MHz or 11ax — and why the AP only ever does 11n

40 MHz and HE/VHT are **mutually exclusive** in the ESP WiFi driver: it accepts
`WIFI_BW40` only when neither `11AX` nor `11AC` is in that band's protocol mask.
The driver says so itself while starting the SoftAP:

```
W wifi:11ax/11ac mode can not work under phy bw 40M, the softap 5G bandwidth changed to 20M
```

Selecting 40 MHz therefore makes the firmware drop 11ax/11ac from that band's
mask automatically and log a warning.

In practice the choice matters less than it looks, because **the SoftAP appears
not to offer HE at all**: clients associate as `an` (802.11a/n) and a Pixel 7
reports a 65 Mbps link at 20 MHz, which is 11n HT20 MCS7 — HE20 would be 86 Mbps
or more. ESP-IDF does not document this either way, so treat it as an
observation. The consequence is that client-side throughput tracks channel width
only:

| AP configuration | Client link | Through the bridge |
|---|---|---|
| 2.4 GHz, 40 MHz | 150 Mbps | **41 Mbps** — fastest working setup |
| 2.4 GHz, 20 MHz | 72 Mbps | 28 Mbps |
| 5 GHz, 20 MHz | 65 Mbps | 5 GHz cannot use 40 MHz |

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
- **Band mode** (2.4 + 5 GHz auto / 2.4 GHz only / 5 GHz only — ESP32-C5)
- **Per-band bandwidth** (20 / 40 MHz) and **prefer-5 GHz margin** (ESP32-C5)
- Upstream SSID cloning (repeater AP takes over the router's network name)
- Pseudo-mesh roaming (RSSI threshold + hysteresis)
- Reset to defaults

The status card also reports the live link: band, negotiated PHY
(11ax / 11ac / 11n) and channel width.

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
| Repeater AP SSID | Our repeater's SSID | MyRepeater1 |
| Repeater AP Password | Repeater password | repeater123 |
| Max connected clients | Max clients | 4 |
| AP Authentication Mode | AP auth mode | WPA2-PSK |
| Clone upstream SSID | AP takes over router's SSID | No |
| TX Power (dBm) | TX power | 20 |
| WiFi band mode | 2.4 / 5 GHz selection (5 GHz-capable SoCs) | 2.4 + 5 GHz (auto) |
| 2.4 GHz bandwidth | 20 MHz keeps 11ax; 40 MHz drops it | 20 MHz |
| 5 GHz bandwidth | 20 MHz keeps 11ax/11ac; 40 MHz drops them but is faster | 40 MHz |
| Prefer 5 GHz AP margin | dB of RSSI a 5 GHz AP may be worse and still win | 10 dB |
| Enable pseudo-mesh roaming | Roam to better AP with same SSID | No |
| Roaming RSSI threshold | RSSI threshold to start scanning | -70 dBm |
| Roaming hysteresis | New AP must be better by this many dB | 8 dB |
| Enable HTTP config GUI | Web GUI for configuration | Yes |
| HTTP server port | HTTP server port | 80 |
| Filter broadcast/multicast | Skip lwIP for non-ARP broadcast (faster) | Yes |

> Values from menuconfig are **defaults** — overridden by NVS / web GUI after first save.

## Build and flash

```bash
# ESP32-C5 (WiFi 6, dual-band 2.4 + 5 GHz) — primary target
idf.py set-target esp32c5
idf.py build
idf.py -p COMx flash monitor

# ESP32-C6 (WiFi 6, 2.4 GHz)
idf.py set-target esp32c6
idf.py build
idf.py -p COMx flash monitor

# ESP32-S3 / ESP32-C3 / ESP32 classic (WiFi 4)
idf.py set-target esp32s3      # or esp32c3 / esp32
idf.py build
idf.py -p COMx flash monitor
```

`sdkconfig` is generated and gitignored — the source of truth is
`sdkconfig.defaults` plus `sdkconfig.defaults.<target>`. After editing either,
delete `sdkconfig` and rebuild.

See [AGENTS.md](AGENTS.md) for the ESP32-C5 USB-Serial/JTAG quirks (console
routing, reset behaviour, recovery) — they will save you a few hours.

## Quick start

1. **Flash** firmware to ESP32-C5, C6, S3, C3 or ESP32
2. **Connect** to WiFi network `MyRepeater1` (password: `repeater123`)
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

- ESP32 has **one radio** — STA and AP must operate on the same channel and band (automatically matched to the upstream)
- STA MAC cloned for one client (primary) — additional clients handled via MAC-NAT
- Maximum **8 entries** in MAC-NAT table (LRU eviction)
- The web GUI has **no authentication** — anyone on the network can reach it. Deliberate zero-config tradeoff; add auth before exposing it beyond a trusted LAN.
- `esp_wifi_internal_reg_rxcb` is a private ESP-IDF API — may change in future versions

### Measured throughput

Traffic crosses the same radio twice (in and out), so the bridge realistically
delivers about half of what the radio can push.

| Platform | Conditions | Through the bridge |
|---|---|---|
| ESP32-S3 | WiFi 4, HT40, broadcast filter ON | ~40 Mbps |
| ESP32-C6 | WiFi 6, HE20, broadcast filter ON | ~15 Mbps |
| **ESP32-C5** | 2.4 GHz, **HT40** (40 MHz + 11n), 150 Mbps link | **41 Mbps** |
| **ESP32-C5** | 5 GHz, **HE20** (20 MHz + 11ax), 72 Mbps link | **27.6 / 24.1 Mbps** (down/up) |
| ESP32-C5 | 2.4 GHz, HT20, 72 Mbps link | 28 Mbps |

Measured with speedtest.net; link rates as reported by a Pixel 7.

The implementation is close to the ceiling: 27.6 Mbps on a 72 Mbps link is 77% of
the 36 Mbps that half-duplex allows, since every packet crosses the same radio
twice. The nearly symmetric upload confirms it — the radio does the same work in
both directions. What is left is 802.11 overhead the application cannot touch.

So **throughput tracks the client link rate**, which in turn tracks channel width
(the SoftAP is 11n either way, see above).

This is an **airtime** limit, not a CPU limit: doubling the channel width gave
+46%, which cannot happen if the CPU is the bottleneck — a packet costs the same
cycles no matter how fast it flies. 150 Mbps PHY yields roughly 90-100 Mbps of
one-way TCP, halved by the double radio traversal, so ~45-50 Mbps is the ceiling
and 41 Mbps is about 85% of it.

Enable `REPEATER_CPU_STATS` in menuconfig to have the status log report CPU
idle time and confirm this on your own hardware (off by default — FreeRTOS
run-time stats add per-context-switch overhead).

### Why the C5 does not beat the S3 on raw speed

The C5 caps at **HT40 with one spatial stream**, same as the S3 — ESP-IDF lists
it among the chips that "support Wi-Fi bandwidth HT20 or HT40", so there is no
80 MHz mode and no second stream. 40 MHz at 5 GHz is exactly as fast as 40 MHz
at 2.4 GHz.

The 5 GHz advantage is **spectrum cleanliness** — fewer retransmissions, less
wasted airtime. In a congested 2.4 GHz environment that can be worth 20-30%; in
a quiet one, almost nothing.

Espressif's `iperf` figures for the C5 in a shield-box (UDP RX 71 / TX 64 Mbps,
raw 802.11 ceiling 130 Mbps) put the bridge ceiling around 60-65 Mbps, so there
is headroom left — but it has to come from efficiency (non-DFS channel, less
interference), not from a wider channel.

Breaking past that needs **two radios**: a C5 as a 5 GHz station plus a second
chip (e.g. a C6) as a 2.4 GHz AP, wired together over SDIO/SPI
(ESP-Hosted / `esp_wifi_remote`) and bridged at L2 across that link. Then the
half-duplex penalty disappears because each radio handles one direction.

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
