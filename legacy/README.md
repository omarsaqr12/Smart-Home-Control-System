# Legacy / earlier iterations

These files are **earlier prototypes** kept for reference. They are **not built** —
the production firmware is [`../main/main.c`](../main/main.c) (the only source listed
in [`../main/CMakeLists.txt`](../main/CMakeLists.txt)).

| File | What it is |
|---|---|
| `main_old.c` | The original **MQTT** build: a hand-rolled MQTT 3.1.1 client over a raw TCP socket, a mutex-guarded transmit path, and a bit-banged **DHT22** temperature/humidity driver. This was later replaced by the Firebase REST design in `main/main.c`. |
| `main_ac.c` | Firebase build extended with **IR transmission** for AC control via the ESP32 RMT peripheral (capture + replay of remote codes). |
| `main_pir.c` | Standalone bench test for the HC-SR501 PIR sensor used during bring-up. |

Credentials in these files have been replaced with placeholders.
