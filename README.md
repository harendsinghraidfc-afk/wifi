# ESP-Router

ESP32 ko WiFi repeater + router-style admin panel bana dene wala firmware.

Device apna access point (AP) banata hai, upstream WiFi se connect hota hai, aur `192.168.4.1` par ek local admin panel serve karta hai. Internet upstream se AP clients tak NAPT ke through share hota hai — yaani ek chhota WiFi router.

> **UI:** vanilla HTML/CSS/JS firmware mein bundled hai. OTA update mein panel bhi update hota hai.
>
> **Firmware:** Arduino framework + ESPAsyncWebServer + ArduinoJson, PlatformIO (pioarduino) se build.

---

## Features

- **AP + STA repeater** — upstream WiFi connect karke NAT se internet share.
- **Standalone AP mode** — NAT off karke sirf local network.
- **Router-style panel** — dashboard, wireless, clients, DHCP & NAT, system, live logs.
- **Direct AP** — first boot par `Lesh` WiFi aur panel login ready hain.
- **mDNS** — `http://esp-router.local` (hostname configurable).
- **Login security** — PBKDF2-HMAC-SHA256 password hash, HttpOnly session cookie, CSRF token, 5-failure lockout.
- **Live updates** — WebSocket se stats aur logs (2s / 1s push).
- **Client management** — online devices, RSSI, rename, block/unblock (block turant deauth karta hai).
- **DHCP control** — pool start/end, lease time, upstream DNS advertise.
- **System tools** — OTA firmware upload, config backup/restore (secrets optional), NTP + timezone, daily scheduled reboot, factory reset.
- **Dual board support** — classic ESP32 DevKit (4MB, 2.4GHz) aur ESP32-C5 DevKit N8 (8MB, dual-band).
- **5 GHz AP** — sirf ESP32-C5 par, band selector panel mein.

---

## Supported boards

| Board | Flash | WiFi | Env |
|---|---|---|---|
| ESP32 DevKit / WROOM-32 | 4 MB | 2.4 GHz | `esp32dev` |
| ESP32-C5 DevKit N8 | 8 MB | 2.4 + 5 GHz | `esp32c5` |

> C5 ka `esp32-c5-devkitc-1` board definition 4MB default deta hai; `platformio.ini` mein flash size 8MB override kiya gaya hai.

---

## Project structure

```
firmware/
├── platformio.ini
├── partitions/
│   ├── esp32_4mb_ota.csv
│   └── esp32c5_8mb_ota.csv
├── data/                     # Embedded admin and guest pages (bundled in OTA)
│   ├── index.html
│   └── assets/
│       ├── app.css
│       └── app.js
└── src/
    ├── main.cpp
    ├── version.h
    ├── core/
    │   ├── AuthManager.*     # PBKDF2, sessions, CSRF, lockout
    │   ├── ClientTracker.*   # AP clients, DHCP/ARP IP mapping, block
    │   ├── ConfigStore.*     # LittleFS config + NVS secrets
    │   ├── DhcpManager.*     # dhcpd pool / lease / DNS
    │   ├── NatManager.*      # esp_netif NAPT
    │   └── WifiManager.*     # AP/STA, scan, band mode, events
    ├── modules/
    │   ├── LogBuffer.*       # in-RAM log ring + esp_log hook
    │   └── SystemTools.*     # NTP, scheduled reboot, ping
    └── web/
        └── WebApp.*          # REST API, WebSocket, OTA, static files
```

---

## Build & flash

### Requirements

- Python 3.10+ (tested with 3.13)
- PlatformIO Core 6.2+ (`python -m pip install platformio`)
- **Git** — pioarduino platform import par `git` PATH mein hona chahiye.
  - Is workspace mein portable MinGit `tools/git/` mein rakha gaya hai. Build se pehle:
    ```powershell
    $env:PATH = "$PWD\tools\git\cmd;" + $env:PATH
    ```
  - Ya system-wide Git install karo: <https://git-scm.com/download/win>

### Build

```powershell
cd firmware
python -m platformio run -e esp32dev
```

C5 ke liye:

```powershell
python -m platformio run -e esp32c5
```

### Flash

```powershell
python -m platformio device list          # COM port dekho
python -m platformio run -e esp32dev -t upload --upload-port COM5
```

Panel ke liye `uploadfs` zaroori nahi hai. LittleFS mein router ki settings rehti hain.

### Full erase (agar device bricked/loop ho)

```powershell
python -m platformio run -e esp32dev -t erase --upload-port COM5
python -m platformio run -e esp32dev -t upload -t uploadfs --upload-port COM5
```

### Monitor

```powershell
python -m platformio device monitor -b 115200
```

---

## First boot

1. Firmware flash karo.
2. **`Lesh`** WiFi join karo (default: open, WiFi password nahi hai).
3. `http://192.168.4.1/` kholo. Username **`Lesh`**, panel password **`admin`**.
4. Internet share karna hai to **Wireless → Upstream (WAN)** mein upstream WiFi select karo. AP ka password Wireless mein aur panel password System mein badal sakte ho.
5. **OTA:** System → Firmware update mein apne board ki `.pio/build/<env>/firmware.bin` upload karo. Board khud reboot karega; panel saath update hoga aur settings preserve rahengi.

Is workspace mein COM3 par esptool ne **ESP32-D0WD-V3 rev 3.1, 4 MB** detect kiya. Is board ka environment `esp32dev` hai. USB auto-reset detection ke waqt kaam kiya; first install ke liye upar wale upload commands mein `COM3` use karo.

---

## Panel tour

| Page | Kya milta hai |
|---|---|
| **Dashboard** | WAN/AP/NAT status, uptime, heap, live client count, quick actions |
| **Wireless** | AP SSID/password/band/channel/tx power/IP/subnet; WAN scan + static IP |
| **Clients** | MAC, IP, signal, connected time, rename, block/unblock |
| **DHCP & NAT** | Pool start/end, lease minutes, DNS, NAPT toggle + status |
| **System** | Device info, OTA, NTP/timezone/scheduled reboot, backup/restore, admin password, reboot/factory reset |
| **Logs** | Live esp_log stream, pause/clear/autoscroll/download |

Panel hash-routing use karta hai (`#/dashboard`, `#/wireless`, …), isliye reload par page bana rehta hai.

---

## NAPT / repeater mode

- NAPT AP interface (`WIFI_AP_DEF`) par enable hota hai, STA par nahi.
- `NatManager` har second check karta hai: NAT tabhi active hota hai jab config enabled ho, upstream connected ho, aur device setup complete ho.
- Arduino core 3.3.13 ke prebuilt libs mein `CONFIG_LWIP_IP_FORWARD=y` aur `CONFIG_LWIP_IPV4_NAPT=y` dono boards (esp32 + esp32c5) ke liye enabled hain — alag build flag ki zaroorat nahi.
- Agar kabhi aisa build ho jismein NAPT compile nahi hua, `esp_netif_napt_enable()` `ESP_ERR_NOT_SUPPORTED` return karta hai; panel System page par **NAPT: Unsupported** dikhega. Us case mein `platformio.ini` ke `[env]` section mein:
  ```ini
  build_flags =
      -DCONFIG_LWIP_IP_FORWARD=1
      -DCONFIG_LWIP_IPV4_NAPT=1
  ```
  (Ye sirf custom framework builds ke liye hai; prebuilt Arduino libs already NAPT ke saath aate hain.)

### Band mode (C5)

`esp_wifi_set_band_mode()` poore WiFi driver par lagta hai — AP aur STA dono.

- `2.4` — sirf 2.4 GHz (default).
- `5` — sirf 5 GHz; upstream bhi 5 GHz hona chahiye.
- `auto` — dono bands; dual-band repeater ke liye best.

Classic ESP32 par band selector disabled hai.

---

## Partition layout

### 4 MB (`esp32_4mb_ota.csv`)

| Partition | Size | Note |
|---|---|---|
| nvs | 20 KB | secrets (WiFi passwords, admin hash) |
| otadata | 8 KB | OTA slot state |
| app0 | 1.5 MB | firmware |
| app1 | 1.5 MB | OTA slot |
| spiffs (LittleFS) | 896 KB | settings + optional guest HTML |
| coredump | 64 KB | crash dumps |

### 8 MB (`esp32c5_8mb_ota.csv`)

| Partition | Size |
|---|---|
| nvs | 20 KB |
| otadata | 8 KB |
| app0 | 2 MB |
| app1 | 2 MB |
| spiffs (LittleFS) | 3.875 MB |
| coredump | 64 KB |

> LittleFS partition ka subtype `spiffs` hai — Arduino `LittleFS` isi ko mount karta hai.

---

## Storage & configuration

- **`/config.json`** (LittleFS) - AP, WAN (bina WiFi password), DHCP, NAT, clients, system aur portal settings. Portal code, grant key aur webhook secret device par isi file mein rehte hain.
- **NVS namespace `router`** — `apPass`, `wanPass`, `adminHash`, `adminSalt`.
- Config save atomic hai: `config.tmp` likh kar rename.
- Backup JSON flat config hai; `?secrets=1` par `secrets` object bhi include hota hai.
- Restore usi format ko accept karta hai; secrets optional hain.

---

## Security model

- Admin password: PBKDF2-HMAC-SHA256, 12,000 iterations, 16-byte random salt, 32-byte hash.
- Session: 32-byte random token, HttpOnly `SID` cookie, `SameSite=Strict`, 30 min idle timeout, max 4 sessions (LRU evict).
- CSRF: har mutating request par `X-CSRF-Token` header; token login/auth-status response se milta hai.
- Login lockout: 5 failures ke baad 60s block, per client IP, 5-minute window.
- WebSocket handshake bhi `SID` cookie validate karta hai.
- Panel HTTP par hai (HTTPS v1 mein nahi). Isliye admin password ko public WiFi par reuse mat karo.
- OTA upload bhi session + CSRF ke bina reject hota hai.

---

## REST API (summary)

Auth: `SID` cookie. Mutating endpoints ko `X-CSRF-Token` chahiye.

| Method | Path | Auth | Description |
|---|---|---|---|
| GET | `/api/auth/status` | — | setup/authenticated state, CSRF |
| POST | `/api/auth/login` | — | `{username:"Lesh", password}` |
| POST | `/api/auth/logout` | ✓ CSRF | session close |
| POST | `/api/setup` | — | first-boot wizard |
| POST | `/api/auth/password` | ✓ CSRF | change admin password |
| GET | `/api/system/info` | ✓ | device/firmware stats |
| POST | `/api/system/reboot` | ✓ CSRF | reboot |
| POST | `/api/system/factory-reset` | ✓ CSRF | `{confirm:"RESET"}` |
| GET | `/api/system/config` | ✓ | backup (`?secrets=1`) |
| POST | `/api/system/config` | ✓ CSRF | restore |
| GET | `/api/system/logs` | ✓ | `?since=<seq>` |
| GET/POST | `/api/system/time` | ✓ | NTP/schedule settings |
| POST | `/api/system/ota` | ✓ CSRF | multipart `.bin` upload |
| GET | `/api/wifi/scan` | ✓ | nearby networks |
| GET | `/api/wifi/status` | ✓ | AP + STA state |
| POST | `/api/wifi/ap` | ✓ CSRF | AP settings |
| POST | `/api/wifi/wan` | ✓ CSRF | upstream settings |
| POST | `/api/wifi/wan/disconnect` | ✓ CSRF | drop upstream |
| GET | `/api/wifi/clients` | ✓ | client list |
| POST | `/api/wifi/clients/block` | ✓ CSRF | `{mac, blocked}` |
| POST | `/api/wifi/clients/rename` | ✓ CSRF | `{mac, name}` |
| GET/POST | `/api/dhcp` | ✓ | DHCP pool |
| GET/POST | `/api/router/nat` | ✓ | NAT toggle |
| POST | `/api/tools/ping` | ✓ CSRF | `{host, count}` |
| WS | `/ws` | cookie | `stats` + `logs` push |

---

## Troubleshooting

**Panel par "UI not uploaded"**
: `python -m platformio run -e esp32dev -t uploadfs` chalao.

**Build error: "Git not found in PATH"**
: Portable Git ka `cmd` folder PATH mein add karo (upar command), ya Git install karo.

**Build error: `WindowsLongPathError` / path exceeds 260 characters**
: ESP32 Arduino libs archive mein bahut lambi paths hain. `platformio.ini` mein `cache_dir = C:/pio-cache` set hai; agar `C:` par likhne ki permission na ho to ise kisi short path par badal do (jaise `D:/pc`). Ye sirf download/extract cache hai — project ya packages move nahi hote.

**C5 upload/flash issues**
: Board `esp32-c5-devkitc-1` 4MB default hai; env 8MB override karta hai. Agar aapke board par 4MB flash hai to `platformio.ini` se `board_build.flash_size` / `board_upload.*` hatao. USB-JTAG port use karo.

**AP dikh nahi raha**
: Channel/band mismatch ho sakta hai. C5 par `5 GHz` select karke channel 36+ rakho; classic par 2.4 GHz. Ya serial monitor par `softAP failed` dekho.

**NAT active nahi ho raha**
: Dashboard/NAT page par upstream "Connected" hona chahiye. `NAPT: Unsupported` dikhe to build flag se NAPT enable karo.

**Upstream connect nahi ho raha (C5, 5 GHz mode)**
: Band mode global hai — 5 GHz AP ke saath 2.4 GHz upstream nahi chalega. `Auto` ya `2.4` choose karo.

**Password bhool gaye**
: Panel access nahi hai to full erase + reflash karo (upar commands). Isse config aur secrets dono delete honge.

**mDNS kaam nahi kar raha**
: Windows par Bonjour install hona chahiye; ya seedha IP use karo. Hostname change ke baad reboot zaroori hai.

---

## Limitations (v1)

- Static DHCP leases, port forwarding, QoS, multiple SSIDs/VLANs nahi hain.
- Admin panel HTTP par hai. Guest portal HTTP aur HTTPS dono par milta hai: `https://192.168.4.1/portal` (AP IP badle to naya address use karo). HTTPS default/custom page aur guest authorization API dono par kaam karta hai.
- **Captive portal → Download router certificate** se public self-signed certificate milta hai. ESP32 apna ECDSA key device par generate karta hai; identity reboot aur OTA ke baad same rehti hai. Factory reset ya AP IP badalne par naya certificate aata hai. Browser/system ko is specific certificate par trust na ho to warning aayegi. Doosri websites ke HTTPS certificates isse validate nahi hote; OS captive probes HTTP par hi rehte hain.
- **Captive portal** default off hai. Sidebar se enable karo. Guest page `/portal` hai; admin panel hamesha `/admin` par milta hai. Toggle ke baad purana DHCP lease use kar rahe clients WiFi reconnect karein. HTTP probes portal par redirect hote hain; HTTPS ko transparent redirect nahi kar sakte. Auto-popup OS par depend karta hai.
- Guest access methods: click, shared code, ya local HTTP webhook. Webhook ko router `POST {"ip":"<client IP>"}` bhejta hai (optional bearer secret); HTTP 200 aur `{"allow":true}` par hi access milta hai. Session default 60 minutes hai. Access us connected device ke IP aur MAC dono se bind hota hai.
- Custom page: admin panel se single HTML (8 KiB tak, inline assets) upload/preview/reset karo. `GET /api/portal/info` returns title, message, mode, enabled, authorized. `POST /api/portal/authorize` accepts JSON `{}` or `{"code":"..."}`; `authorized:true` means the session was granted. `GET /api/portal/status` checks it; `POST /api/portal/logout` ends it. WAN/NAT availability is independent.
- `POST /api/portal/grant` with `{"ip":"192.168.4.10"}` accepts an admin session + CSRF or the configured `X-Portal-Key`. Never put this key in guest HTML. Admins can list `/api/portal/authorized` and POST the same IP to `/api/portal/revoke`. Portal secrets are omitted from ordinary JSON backups.
- 5 GHz AP channel set manual hai (36–165), auto channel selection nahi.
- Clients page DHCP hostname dikhata hai. Jo device naam share nahi karta uske liye MAC suffix dikhta hai; Rename se apna naam rakh sakte ho.
- Ping API synchronous hai aur web task ko kuch seconds block kar sakta hai.
- OTA sirf panel se; Arduino OTA (network port) nahi.
- WebSocket live push single broadcast — per-client subscriptions nahi.

---

## License

Internal project. Third-party libraries apni respective licenses follow karte hain:

- ArduinoJson (MIT)
- ESPAsyncWebServer / AsyncTCP (MIT/Apache-2.0)
- Arduino-ESP32 (LGPL-2.1)
- ESP-IDF (Apache-2.0)
