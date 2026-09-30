# ChromaVertex Arena Beacon — Firmware

Firmware for the ChromaVertex arena corner beacons.  
Each beacon is a 267mm WS2812B LED tower (16 LEDs, 8 active) mounted on an  
ESP32-C3 SuperMini, used to provide visual position references for the VertexBot swarm.
![Beacon](/documentation/beacon.png)
---

## Two Implementations

Two complete, functionally identical implementations are provided.

### ✅ ESPHome / YAML (Recommended for development)

Located in: `esphome/beacon1.yaml`

The ESPHome version is **strongly recommended** for members working with  
Home Assistant, or for anyone who wants to get a beacon running quickly.  
It is significantly simpler to configure, maintain and extend — the entire  
application is expressed in around 100 lines of readable YAML.

**Requirements:**
- Home Assistant with the ESPHome Device Builder add-on, or the ESPHome CLI
- Secrets file (`secrets.yaml`) with your WiFi credentials

**Flash:**
```bash
esphome run esphome/beacon1.yaml
```

---

### ⚙️ ESP-IDF / C++ (This folder)

Located in: `beacon1/` (this directory)

The ESP-IDF version is provided for members who:
- prefer a pure C++ environment
- are not running Home Assistant
- want to understand what ESPHome generates under the hood
- are working on the robot camera firmware (which must use ESP-IDF)
- want to loan beacons to other members without reflashing

The code is structured to be as readable as possible, with each source file  
corresponding directly to a section of the YAML.

---

## Features

- **38 LED sequences** — 32 unique de Bruijn identity patterns + 6 solid colours
- **WiFi provisioning** — self-configuring via browser, no reflashing needed
- **NVS credential storage** — remembers WiFi across power cycles
- **mDNS** — accessible as `http://beacon1.local` without knowing the IP
- **OTA firmware updates** — upload new firmware via the web UI
- **Web control UI** — sequence selection, firmware upload, WiFi management
- **Multi-network fallback** — tries saved credentials, then hardcoded, then provisioning AP

---

## WiFi Setup for Team Members

Beacons are pre-flashed and self-configuring. No reflashing required.

**First power-on (or after "Forget WiFi"):**
1. The beacon starts a hotspot: **`ChromaVertex-Setup`** (password: `chromavertex`)
2. Connect your phone or laptop to that hotspot
3. **The setup page opens automatically** — iOS, Android and Windows all detect
   the captive portal and pop up a "Sign in to network" dialog
4. If it doesn't open automatically, navigate to **`http://192.168.4.1`** manually
5. Select your WiFi network from the dropdown and enter your password
6. Tap **Connect & Save** — the beacon tests the connection
7. On success it saves the credentials and reboots automatically
8. From now on it connects to your network on every power-on

![Captive Portal](/documentation/captive.png)

**Captive portal OS compatibility:**

| Platform | Trigger | Notes |
|---|---|---|
| iOS | `/hotspot-detect.html` | Opens Safari automatically |
| Android | `/generate_204` | Opens browser automatically |
| Windows | `/ncsi.txt`, `/connecttest.txt` | May show notification |
| macOS | `/hotspot-detect.html` | Opens browser automatically |

**To change the WiFi network:**
- Open the control page at `http://beacon1.local`
- Scroll to the WiFi section
- Tap **Forget WiFi & Re-provision** — returns to step 1 above

---

## Normal Operation

Once connected, open **`http://beacon1.local`** in any browser on the same network.

The control page provides:
- **Sequence selector** — choose from 1–38 sequences
- **Firmware update** — select a `.bin` file and upload OTA
- **Forget WiFi** — clear saved credentials and re-provision

JSON status is available at `http://beacon1.local/status`
![Web Server](/documentation/WebServer.png)
---

## YAML → C++ Component Map

| ESPHome YAML | ESP-IDF file | Notes |
|---|---|---|
| `esphome: on_boot:` | `main.cpp` — `app_main()` | Boot sequence |
| `wifi: networks:` | `wifi_manager.cpp` | Multi-network with NVS + provisioning |
| `wifi: ap:` | `wifi_manager.cpp` — `start_provisioning_ap()` | Self-provisioning AP |
| `web_server: port: 80` | `web_server.cpp` | HTTP control + provisioning UI |
| `web_server: auth:` | `web_server.cpp` — `check_auth()` | Basic authentication |
| `globals: sequence_id` | `led_manager.cpp` — `s_sequence_id` | Global state |
| `number: platform: template` | `web_server.cpp` — `handle_root()` | Sequence selector UI |
| `light: esp32_rmt_led_strip` | `led_manager.cpp` — `led_manager_init()` | LED strip setup |
| `addressable_lambda` | `led_manager.cpp` — `render_sequence()` | Pattern rendering |
| `update_interval: 500ms` | `led_manager.cpp` — `esp_timer_start_periodic()` | 500ms timer |
| `ota: platform: esphome` | `web_server.cpp` — `handle_ota()` | OTA via web UI |
| _(no equivalent)_ | `wifi_manager.cpp` — NVS functions | Credential persistence |
| _(no equivalent)_ | `main.cpp` — `mdns_start()` | `beacon1.local` hostname |

---

## Hardware

| Item | Detail |
|---|---|
| MCU | ESP32-C3 SuperMini |
| LED strip | WS2812B (GRB order), 16 LEDs |
| Active LEDs | 8 (even indices 0,2,4...14) |
| Separator LEDs | 8 (odd indices 1,3,5...15 — always off) |
| Data pin | GPIO4 |
| Beacon height | 267mm |
| Diffuser segments | 15mm per active LED |

---

## Sequences

| Range | Type | Description |
|---|---|---|
| 1 – 32 | de Bruijn | Unique 8-position patterns over 4 colours |
| 33 | Solid | All LEDs CYAN |
| 34 | Solid | All LEDs MAGENTA |
| 35 | Solid | All LEDs YELLOW |
| 36 | Solid | All LEDs GREEN |
| 37 | Solid | All LEDs RED |
| 38 | Solid | All LEDs BLUE |

The 4 arena corner beacons are assigned sequences 1–4 (or any 4 de Bruijn  
sequences chosen to be maximally visually distinct from each other).

---

## Colour Palette

| Index | Name | RGB |
|---|---|---|
| 0 | CYAN | (0, 255, 255) |
| 1 | MAGENTA | (255, 0, 255) |
| 2 | YELLOW | (255, 255, 0) |
| 3 | GREEN | (0, 255, 0) |
| 4 | RED | (255, 0, 0) |
| 5 | BLUE | (0, 0, 255) |

---

## ESP-IDF Build Instructions

### Prerequisites

- ESP-IDF v5.5.1 installed (via EIM or manual)
- VS Code with ESP-IDF extension, or `idf.py` on PATH
- **Important:** Do not store the project in a cloud-synced folder (OneDrive, Dropbox etc.)  
  The toolchain contains thousands of binary files that cloud sync corrupts

### ⚠️ Common setup mistakes

**`idf_component.yml` must be in `main/` not the project root.**  
If you run `idf.py add-dependency`, it creates the file in the project root — move it:
```powershell
Move-Item "beacon1\idf_component.yml" "beacon1\main\idf_component.yml"
Remove-Item -Recurse -Force "beacon1\build"
idf.py reconfigure
```

**Always use IDF v5.5.1.** The `.vscode/settings.json` in this repo locks VS Code to v5.5.1.  
For the IDF terminal, load the environment first:
```powershell
& 'C:\Espressif\tools\Microsoft.v5.5.1.PowerShell_profile.ps1'
$env:IDF_TARGET = ""   # clear any stale target
```

**Never store the project in OneDrive or cloud-synced folders** — binary toolchain  
files get corrupted during sync.

### Configure credentials

Copy the template and fill in your WiFi credentials:

```powershell
Copy-Item "main\secrets.h.template" "main\secrets.h"
# then edit main\secrets.h
```

`secrets.h` is in `.gitignore` and is never committed.  
For beacons deployed to team members, leave all values as empty strings `""` —  
the beacon will start in provisioning mode automatically.

```cpp
// main/secrets.h — developer example
#pragma once
#define WIFI_SSID_HOME    "YourSSID"
#define WIFI_PASS_HOME    "YourPassword"
#define WIFI_SSID_MOBILE  ""
#define WIFI_PASS_MOBILE  ""
#define WIFI_SSID_IPHONE  ""
#define WIFI_PASS_IPHONE  ""
```

### First flash (sets up OTA partition table)

```bash
cd beacon1
idf.py set-target esp32c3
idf.py build
idf.py -p COM3 erase-flash     # required first time — sets up OTA partitions
idf.py -p COM3 flash monitor
```

### Subsequent updates

After the first flash, firmware can be updated OTA via the web UI:
1. `idf.py build` — produces `build/beacon1.bin`
2. Open `http://beacon1.local`
3. Select `build/beacon1.bin` and click **Upload Firmware**

### Configuring for each beacon

In `main/main.cpp`, change per beacon:
```cpp
#define BEACON_HOSTNAME  "beacon2"   // beacon1, beacon2, beacon3, beacon4
#define BEACON_BOOT_SEQ  2           // starting sequence
```

---

## IDF 5.5.1 Build Notes

The following issues were encountered building against IDF 5.5.1 on Windows.  
Documented here to save the next person the same debugging time.

| Issue | Cause | Fix |
|---|---|---|
| `esp_log` component not found | In 5.5.1 it is internal — do not declare in `REQUIRES` | Remove from `REQUIRES` |
| `driver/gpio.h` not found | GPIO headers moved in IDF 5.x | Use `hal/gpio_types.h` instead |
| `LED_STRIP_COLOR_COMPONENT_FMT_GRB` not declared | Field not in `led_strip` 2.5.5 | Remove from `led_strip_config_t` |
| `esp_timer_create_args_t` designator order error | C++ requires designated initialisers in declaration order | Reorder: `callback`, `arg`, `dispatch_method`, `name` |
| `ESP_WIFI_TX_BUFFER_TYPE_DYNAMIC` unknown symbol | Removed in IDF 5.5.1 | Remove from `sdkconfig.defaults` |
| `idf_component.yml` not found | Must be in `main/` not project root | Move to `main/idf_component.yml` |
| `cc1.exe` not found / `libexec` is a file | Toolchain corrupted by cloud sync | Install to a local non-synced directory |
| `0x200` WiFi auth fail with correct credentials | WiFi driver reading stale NVS cache | Add `esp_wifi_set_storage(WIFI_STORAGE_RAM)` before connecting |
| `esp_wifi_set_max_tx_power` crashes | Must be called after `esp_wifi_start()` not `esp_wifi_init()` | Move into `try_connect()` after `esp_wifi_start()` |
| httpd stack overflow on first web request | `snprintf` of large HTML buffer overflows 4096 default stack | Declare buffers as `static`, set `config.stack_size = 8192` |
| `0x200` auth fail on first cold boot | Omada router rejects first auth after radio cold start | Retry NVS credentials up to 3 times with 2s delay |
| `0x200` auth fail during provisioning | WiFi driver retains AP state affecting STA auth | Call `esp_wifi_deinit()` + `esp_wifi_init()` before STA attempt |
| `mdns` component not found | Moved to component manager in 5.5.1 | Add `espressif/mdns: ">=1.0.0"` to `main/idf_component.yml` |
| Wrong chip target after project move | VS Code retains `IDF_TARGET` env var from previous session | Clear with `$env:IDF_TARGET = ""` then `idf.py set-target esp32c3` |
| `sockaddr_in` designated initialiser error | C++ requires all or no designated initialisers; `.sin_addr.s_addr` uses nested designator | Use `memset` + individual field assignment instead |
| Captive portal URIs returning 404 | Captive portal handlers registered in wrong `if/else` block | Register under `wifi_manager_is_provisioning()` block, not the normal mode block |

---

## Project

**ChromaVertex** — a swarm of collaborative autonomous robots  
Melbourne TechGuilds — Deep Dive Guild  
GitHub: https://github.com/asekerci/chromavertex  
License: GPL-3.0
