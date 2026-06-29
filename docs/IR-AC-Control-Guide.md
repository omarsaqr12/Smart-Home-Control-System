# IR AC Control — Addition Guide

## What Was Added

The latest commit adds the ability to **turn the AC on and off using an IR LED** connected to the ESP32, controlled over MQTT.

The ESP32 acts exactly like a TV remote — it blasts a pattern of infrared pulses at 38 kHz that the AC unit reads as a command. Nothing is "read back" from the AC; it's a one-way signal.

### What triggers the AC

| Trigger | How |
|---|---|
| **Manual** | Publish `AC_ON` or `AC_OFF` to `smarthome/room001/command` via MQTT |
| **Automatic** | AC turns OFF by itself when the last person exits the room (people count → 0) |

There is currently **no auto-ON** when someone enters — this is intentional for now and can be added later.

---

## Hardware Required

- 1× IR LED (940 nm recommended) + 100Ω resistor
- Or a pre-built IR transmitter module (easier, no resistor needed)

**Wiring (bare IR LED):**
```
ESP32 GPIO18  →  100Ω resistor  →  IR LED anode (+)
                                    IR LED cathode (−)  →  GND
```

> For longer range (> 3 m), use a 2N2222 transistor between GPIO18 and the LED.

---

## Before You Flash — Get Your AC's IR Codes

The placeholder codes in `ac_on_cmd[]` and `ac_off_cmd[]` inside `main.c` **will not work** with your AC. Every AC model has its own unique signal. You need to capture yours first.

### Step 1 — Capture mode

In `app_main()` at the bottom of `main.c`, uncomment this line:

```c
xTaskCreate(ir_capture_task, "ir_cap", 4096, NULL, 5, NULL);
```

### Step 2 — Flash and open the serial monitor

```bash
idf.py build
idf.py -p COMx flash monitor
```

### Step 3 — Capture ON code

Point your **original AC remote** at **GPIO13** (the outer IR receiver) and press the **Power ON** button. You will see output like:

```
[CAPTURE] Got 35 symbols. Copy into ac_on_cmd[] or ac_off_cmd[]:
static const rmt_symbol_word_t ac_REPLACE_cmd[] = {
    {.level0=1,.duration0=9024  .level1=0,.duration1=4488 },
    {.level0=1,.duration0=558   .level1=0,.duration1=1694 },
    ...
};
```

Copy the printed array and paste it into `ac_on_cmd[]` in `main.c`.

### Step 4 — Capture OFF code

Repeat Step 3 pressing the **Power OFF** button and paste the result into `ac_off_cmd[]`.

### Step 5 — Re-comment the capture task

```c
// xTaskCreate(ir_capture_task, "ir_cap", 4096, NULL, 5, NULL);
```

Then rebuild and reflash.

---

## Testing It Works

With everything flashed and the IR LED wired up, publish an MQTT message to test:

```bash
# Using mosquitto_pub (replace host/user/pass with your broker's)
mosquitto_pub -h <BROKER_IP> -u <USER> -P <PASSWORD> \
  -t smarthome/room001/command -m AC_ON

mosquitto_pub -h <BROKER_IP> -u <USER> -P <PASSWORD> \
  -t smarthome/room001/command -m AC_OFF
```

You can also check the AC state at any time:

```bash
# Subscribe to AC state feedback
mosquitto_sub -h <BROKER_IP> -u <USER> -P <PASSWORD> \
  -t smarthome/room001/ac

# Or request current state
mosquitto_pub -h <BROKER_IP> -u <USER> -P <PASSWORD> \
  -t smarthome/room001/command -m AC_STATUS
```

The AC feedback topic `smarthome/room001/ac` will publish `ON` or `OFF` after every command.

---

## If It Works — Next Steps

- [ ] **Add auto-ON on entry** — turn AC on when the first person enters the room. The hook is already inside `DETECT_AWAIT_PIR` in `detection_poll()`, just needs the `ir_send()` call added.
- [ ] **Temperature-based control** — the DHT22 is already publishing temperature every 5 s. Add logic to auto-off AC if temp drops below a threshold.
- [ ] **Set temperature / mode** — AC remotes encode full state (temperature, fan speed, mode). Capture separate codes for each setting you want to support and add new MQTT commands like `AC_SET_24` or `AC_COOL`.
- [ ] **IR range test** — verify the LED reaches the AC from the ESP32's mounting position. Aim for direct line of sight; use the transistor driver circuit if range is insufficient.

---

## Pin Summary

| Pin | GPIO | Role |
|---|---|---|
| DHT22 data | 26 | Temperature & humidity |
| PIR sensor | 25 | Motion confirmation |
| IR outer receiver | 13 | Door beam (also used for code capture) |
| IR inner receiver | 14 | Door beam |
| **IR TX LED** | **18** | **AC control (new)** |
