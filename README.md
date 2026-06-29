# Smart Home Control System

> An ESP32 + FreeRTOS smart-home controller that detects room occupancy with directional sensor fusion, automatically drives lighting / air / audio, and exposes real-time monitoring and remote control through a cloud-synced web dashboard.

![Platform](https://img.shields.io/badge/Platform-ESP32-E7352C?logo=espressif&logoColor=white)
![Framework](https://img.shields.io/badge/Framework-ESP--IDF-000000?logo=espressif&logoColor=white)
![RTOS](https://img.shields.io/badge/RTOS-FreeRTOS-00979D)
![Language](https://img.shields.io/badge/Firmware-C-A8B9CC?logo=c&logoColor=white)
![Cloud](https://img.shields.io/badge/Cloud-Firebase%20RTDB-FFCA28?logo=firebase&logoColor=black)
![Dashboard](https://img.shields.io/badge/Dashboard-HTML%2FJS-F7DF1E?logo=javascript&logoColor=black)
![License](https://img.shields.io/badge/License-Apache%202.0-blue)

A real-time embedded IoT system built on the ESP32. Two paired infrared break-beams plus a PIR sensor track who enters and leaves a room; the firmware counts occupants and automatically switches a lamp, an ultrasonic atomizer, and a welcome-audio player. Every state change is mirrored to a Firebase Realtime Database and rendered live on a responsive web dashboard, which can also push commands back to the device.

Developed for **CSCE 4301 – Embedded Systems** as a 3-person team project (see [Team](#team)).

---

## Highlights

- **Concurrent FreeRTOS design** — a high-rate sensor task and a lower-priority network task run independently and communicate through a FreeRTOS queue, so Wi-Fi/cloud latency never stalls time-critical presence detection.
- **Directional occupancy detection** — a debounced state machine reads the order in which two IR beams break (outer→inner = entry, inner→outer = exit) and confirms entries with a PIR sensor, maintaining an accurate live occupant count.
- **Bidirectional cloud control** — the device publishes state to Firebase over HTTPS REST and polls a command node, letting the dashboard toggle lights/atomizer/music from anywhere.
- **Real-time web dashboard** — a single-page app shows occupancy, sensor readings, and device status and refreshes without a page reload.
- **Iterated architecture** — the system was first built on a hand-rolled **MQTT** client and later migrated to **Firebase** for cloud access without a self-hosted broker (the original MQTT build is preserved in [`legacy/`](legacy/)).

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                        ESP32  ·  ESP-IDF + FreeRTOS                    │
│                                                                        │
│   Sensor task (10 ms loop)                  Network task (priority 5)  │
│   ┌─────────────────────────┐   detection   ┌───────────────────────┐ │
│   │ 2× IR beam-break + PIR   │──►  queue  ──►│ consume events        │ │
│   │ debounce + directional   │  (QueueHandle)│ drive actuators       │ │
│   │ entry/exit state machine │               │ publish state to cloud│ │
│   └─────────────────────────┘               │ poll command node     │ │
│            │ actuators                        └──────────┬────────────┘ │
│            ▼                                             │ HTTPS REST   │
│   relay · atomizer · DFPlayer Mini (UART)               ▼              │
└─────────────────────────────────────────────┬──────────────────────────┘
                                               │
                                  Firebase Realtime Database
                                               │
                                  Web dashboard (desktop / mobile)
```

**Why two tasks?** Presence detection samples every 10 ms and must never block. By posting confirmed entry/exit events to a queue, the sensor loop hands off to the network task, which owns all the (potentially slow) Wi-Fi and HTTP work. The two are decoupled and scheduled independently by FreeRTOS.

---

## Features

| Feature | Details |
|---|---|
| Occupancy detection | Directional IR break-beam + PIR sensor fusion, debounced, with a 3 s sequence window and 2 s cooldown |
| Live occupant count | `count > 0` ⇒ room **OCCUPIED**, `count == 0` ⇒ **EMPTY** |
| Automatic actuation | On first entry: lamp ON, atomizer ON, welcome audio plays. On last exit: everything OFF |
| Lighting | Active-LOW relay (GPIO 23) |
| Air freshening | Ultrasonic atomizer via transistor-driven control pin (GPIO 21) |
| Audio | DFPlayer Mini over UART2 (GPIO 17 TX) |
| Remote control | `LIGHTS_ON/OFF`, `ATOMIZER_ON/OFF`, `MUSIC_ON/OFF`, `STATUS` commands from the dashboard |
| Cloud sync | Firebase Realtime Database via REST, anonymous auth with token refresh |
| Dashboard | Responsive HTML/JS, auto-refresh, control buttons |
| AC control *(stretch)* | IR transmit path implemented; reliable per-state code capture for the AC remote left unfinished (see [`docs/`](docs/) and [`legacy/main_ac.c`](legacy/main_ac.c)) |

---

## Tech stack

- **MCU / framework:** ESP32, ESP-IDF, FreeRTOS
- **Firmware:** C — GPIO, UART, HTTP client, Wi-Fi station, NVS, event groups, queues
- **Sensors:** 2× IR break-beam (GPIO), HC-SR501 PIR (GPIO), DHT22 (single-wire, bit-banged in the legacy build)
- **Actuators:** relay, ultrasonic atomizer, DFPlayer Mini (UART), IR LED (RMT, AC stretch goal)
- **Cloud / UI:** Firebase Realtime Database + Hosting, HTML/CSS/JavaScript
- **Earlier iteration:** hand-rolled MQTT 3.1.1 client over TCP sockets (`legacy/main_old.c`)

---

## Repository layout

```
.
├── main/
│   ├── main.c               # production firmware (the built target)
│   ├── secrets.h.example    # template for Wi-Fi + Firebase credentials
│   └── CMakeLists.txt
├── dashboard.html           # single-page web dashboard
├── legacy/                  # earlier prototypes (MQTT build, AC/IR build, PIR bench test)
├── docs/                    # project report + IR AC control guide
├── CMakeLists.txt           # ESP-IDF project root
└── LICENSE
```

---

## Getting started

### Prerequisites
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) (v5.x)
- An ESP32 dev board and the hardware in [Hardware](#hardware)
- A Firebase project with the Realtime Database enabled

### 1. Configure credentials
```bash
cp main/secrets.h.example main/secrets.h
# then edit main/secrets.h with your Wi-Fi + Firebase values
```
`secrets.h` is git-ignored, so your credentials stay out of version control.

### 2. Build, flash, and monitor
```bash
idf.py build
idf.py -p <PORT> flash monitor      # e.g. -p COM5 (Windows) or -p /dev/ttyUSB0 (Linux)
```
Press **Ctrl+]** to exit the monitor.

### 3. Open the dashboard
Open `dashboard.html` in a browser (or host it on Firebase Hosting) and enter your
Firebase Realtime Database URL and Web API key in the connection fields.

```bash
# optional: deploy the dashboard to Firebase Hosting
npm install -g firebase-tools
firebase login
firebase deploy --only hosting
```

---

## Hardware

| Component | Purpose | Pin |
|---|---|---|
| ESP32 | Main microcontroller / Wi-Fi | — |
| IR break-beam (outer) | Entry/exit detection | GPIO 13 |
| IR break-beam (inner) | Entry/exit detection | GPIO 14 |
| PIR sensor (HC-SR501) | Motion confirmation | GPIO 25 |
| DHT22 / AM2302 | Temperature & humidity | GPIO 26 |
| Relay module | Lamp control (active LOW) | GPIO 23 |
| Ultrasonic atomizer | Mist / air freshening | GPIO 21 |
| DFPlayer Mini | Audio playback | UART2 (GPIO 17 TX) |
| IR LED (transmitter) | AC remote control *(stretch)* | GPIO 18/19 |

> Wiring diagrams for each component are in the firmware comments and the project report under [`docs/`](docs/).

---

## Firebase data model

```
smarthome/room001/
├── room        → "OCCUPIED" | "EMPTY"
├── count       → number of people
├── light       → "ON" | "OFF"
├── atomizer    → "ON" | "OFF"
├── music       → "PLAYING" | "STOPPED"
├── temperature → °C
├── humidity    → %
└── command     → "LIGHTS_ON" | "LIGHTS_OFF" | "ATOMIZER_ON" | "ATOMIZER_OFF"
                   | "MUSIC_ON" | "MUSIC_OFF" | "STATUS"
```

The firmware reads `command`, executes it, then deletes the node so each command runs once.

---

## Engineering notes

- **Non-blocking IPC:** entry/exit events flow through a `QueueHandle_t`; the sensor loop calls `xQueueSend` without ever waiting on the network.
- **Wi-Fi lifecycle:** connection state is tracked with a FreeRTOS **event group** and bounded auto-retry.
- **Debouncing:** each beam requires a run of consistent samples before a state flip, rejecting electrical and optical noise.
- **DFPlayer protocol:** audio commands are built as raw 10-byte UART frames with a computed checksum.
- **From MQTT to Firebase:** the first build used a self-written MQTT client and a local Mosquitto broker (mutex-guarded TX, bit-banged DHT22 driver — see `legacy/main_old.c`). It was migrated to Firebase REST to remove the always-on local broker and enable access from anywhere.

---

## Team

A 3-person project for CSCE 4301 (Embedded Systems):

- **Omar Saqr** — [@omarsaqr12](https://github.com/omarsaqr12)
- **Mostafa Gaafar** — [@mostafa21314](https://github.com/mostafa21314)
- **Farida Bey**

This repository is a hosted copy of the team's work. The full commit history (preserved here) reflects each contributor's authorship.

---

## License

Released under the [Apache License 2.0](LICENSE).
