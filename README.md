# Smart Home Control System

**An ESP32 / FreeRTOS room-occupancy and actuator-control prototype with a Firebase-backed web dashboard.**

Built as a three-person **CSCE 4301 — Embedded Systems** project at the American University in Cairo. The repository contains current ESP-IDF firmware, a single-file dashboard, earlier experiments and a project report. The most useful engineering material is the beam-sequence occupancy state machine and its separation from potentially slow network requests. This is a **hardware-dependent course prototype**, not a security-hardened home-automation product.

> **Important security and completeness notice:** The current firmware and dashboard sign in to Firebase but send Realtime Database requests with `?key=<web-api-key>` instead of the Firebase ID token. An API key is **not** a Realtime Database authentication credential. Do not expose this prototype through permissive public database rules or connect it to unattended mains-powered equipment. Authentication, HTTPS certificate validation, command acknowledgments and live hardware testing need to be completed before any real deployment. See [engineering review](docs/ENGINEERING_REVIEW.md).

## What was built

- A 10 ms polling loop for two doorway IR break-beams, using a debounced order-of-interruption state machine. Entry requires PIR confirmation; exits decrement a nonnegative software count.
- A FreeRTOS queue that passes detection events to a separate network/actuation task. Slow HTTP requests are not made directly in the sensor polling loop.
- An active-low lamp relay, toggle-pulsed atomizer and UART DFPlayer audio commands.
- An ESP-IDF Wi-Fi station, an attempted Firebase anonymous sign-in and REST read/write routines, plus a browser dashboard with status display and remote command buttons.
- Preserved [`legacy/`](legacy/) implementations, including a hand-written MQTT iteration and unfinished air-conditioner IR experiments. These are **not** the compiled current application.

The architecture and interfaces are visible in [`main/main.c`](main/main.c) and [`dashboard.html`](dashboard.html). The project report is preserved in [`docs/Project-Report.docx`](docs/Project-Report.docx); this review did not independently validate its binary contents, figures or hardware measurements.

## How the current code is arranged

```text
ESP32: main/main.c
  GPIO 13/14 IR beam input + GPIO 25 PIR input
         │
         ▼
  debouncing → directional state machine → detection_queue
                                               │
                                               ▼
                                         network_task
                                         ├─ relay / atomizer / DFPlayer
                                         └─ Firebase REST read/write
                                               │
                                               ▼
                                      dashboard.html (browser)
```

| Path | Purpose |
| --- | --- |
| [`main/main.c`](main/main.c) | Current compiled firmware; GPIO, FreeRTOS tasks, Firebase requests, state machine, actuation |
| [`main/secrets.h.example`](main/secrets.h.example) | Placeholder Wi-Fi and Firebase configuration; never commit a populated `secrets.h` |
| [`dashboard.html`](dashboard.html) | Browser UI and Firebase REST client; configuration form is currently hidden |
| [`legacy/`](legacy/) | Historical MQTT, AC and sensor experiments; retained for provenance |
| [`docs/IR-AC-Control-Guide.md`](docs/IR-AC-Control-Guide.md) | Guide for an earlier MQTT/IR experiment, **not** instructions for the current build |
| [`docs/ENGINEERING_REVIEW.md`](docs/ENGINEERING_REVIEW.md) | Verified source issues, review limits and a safe validation plan |

## Inspect or build the firmware

**Requirements:** ESP32 board, an ESP-IDF v5.x environment, compatible sensors and actuators, and a Firebase project if cloud functions are to be investigated. Physical pinouts and electrical safety must be checked against the actual modules and board. Do not connect a mains load for initial testing.

```bash
cp main/secrets.h.example main/secrets.h
# Fill in your own Wi-Fi and Firebase project values locally.
idf.py set-target esp32
idf.py build
```

The above commands are a build recipe, **not a recorded successful build**. `main/secrets.h` is git-ignored but credentials can still leak through logs or shared binaries. This review could not run ESP-IDF, flash an ESP32 or inspect physical hardware. Do not deploy against a live database until the security issues above are addressed. For safe bench testing, use an isolated, current-limited setup and simulated or low-voltage loads.

The browser file can be opened locally to inspect the UI. Its Firebase configuration card has `display: none`, and its current automatic startup requires configuration that the UI does not expose. Its AC setpoint button sends `AC_SET_TEMP:<n>` but the current firmware command handler does not implement that command. The dashboard should be treated as a **prototype**, not a working end-to-end demonstration without further work.

## Hardware interface documented by the current firmware

| Device | GPIO | Status in current firmware |
| --- | --- | --- |
| Outer / inner break-beams | 13 / 14 | Polled for directional detection |
| PIR sensor | 25 | Used to confirm entries |
| Relay control | 23 | Active-low output |
| Atomizer button/control | 21 | Timed pulse; software state assumes successful toggle |
| DFPlayer Mini TX | 17 (UART2) | One-way playback commands |
| DHT sensor pin | 26 | Defined but no current sensor-read/publish loop was established in this source review |
| AC infrared output | Historical experiment | Not implemented by the compiled `main/main.c` command handler |

## What still needs verification

The system counts **detected doorway sequences**, not independently verified people. Two people crossing together, a stalled sensor, PIR false positives, queue overflow, loss of power, and missed events can desynchronize the counter. Specifically, the last-exit network handler publishes `EMPTY` without publishing the zero count, so the dashboard can show contradictory values. The current command node is a single shared value; overwrites and delete failures are not acknowledged transactionally. Sensor sampling is also paused during the cooldown rather than preserving edge history.

Before representing this as an end-to-end working demo, verify authenticated database requests with restrictive rules; certificate validation; token refresh/recovery; count transitions and queue pressure; command acknowledgment/replay protection; sensor calibration; actuator state after restart; and dashboard behavior with a real board. A detailed source-based checklist is in [`docs/ENGINEERING_REVIEW.md`](docs/ENGINEERING_REVIEW.md).

## Team and attribution

The original project documentation records the team as **Omar Saqr**, **Mostafa Gaafar** and **Farida Bey**. This repository preserves their work and history. Individual file-by-file authorship and hardware test responsibility were not independently established by this review; do not infer sole authorship from the repository owner.

**License:** [Apache-2.0](LICENSE), unchanged by this review.
