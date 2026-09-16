# Engineering review — 2026-09-16

**Baseline:** `b3fcdb37f66ebb62fa0d112840b0cdce4525b729` on `main`. This is a limited source audit and documentation correction, **not** a successful build, circuit test or safety certification.

## Coverage and review strategy

The recursive GitHub tree contains 20 tracked files: firmware, browser dashboard, configuration, four legacy files, two documentation files and a binary `.docx` project report. This review examined the current firmware's Firebase request/sign-in logic, detection and network task, initialization, the dashboard's configuration/authentication/polling/command logic, the README, credential template, CMake files, devcontainer files, ignore rules and the historical AC guide. The complete dashboard CSS/markup, all legacy C source, IDE settings and the binary report were **not** fully audited; figures and claims in the `.docx` were not verified. The inventory is complete; substantive file review is not exhaustive.

The execution environment could access the connected GitHub API but could not resolve `github.com` to clone the repository, and ESP-IDF, an ESP32 board, sensors, database credentials and a safe actuator test bench were unavailable. No firmware build, flash, live HTTP request, end-to-end UI test or physical electrical check was run.

## Findings from the source

| Priority | Evidence | Classification / handling |
| --- | --- | --- |
| Critical for deployment | `main/main.c::firebase_signin_anonymous` obtains `fb_id_token`, but `firebase_put/get/delete` use `?key=FIREBASE_API_KEY`, not `auth=<ID_TOKEN>`. `dashboard.html::fbUrl` does the same. Firebase's REST docs distinguish web API keys from authenticated database requests. | **Confirmed implementation defect.** Source is unchanged pending an integrated, testable firmware + dashboard fix. Restrictive rules may reject requests; permissive rules would expose room state and actuation. Do not deploy. |
| High | HTTP client configurations use HTTPS URLs but do not attach a certificate / ESP-IDF certificate bundle in these functions. | **Strongly supported transport-configuration concern.** Establish certificate validation with an IDF target build and a negative-certificate test before remote operation. |
| High | `network_task` handles a last exit by publishing room `EMPTY` and actuator states, but not `pub_count(evt.people_count)` when count reaches zero. | **Confirmed state-publication defect.** Test and fix last-exit and burst-event sequences. |
| High | `dashboard.html` has a hidden configuration card (`display: none`) and an empty API key field, yet starts polling on page load; its `fbUrl` also does not use the acquired ID token. | **Confirmed UX/authentication inconsistency.** Enable a safe configuration flow and auth lifecycle after Firebase security is fixed. |
| Medium | Dashboard sends `AC_SET_TEMP:<n>`; current `network_task` only handles light, atomizer, music and status commands. The historical AC guide documents MQTT rather than the compiled Firebase firmware. | **Confirmed feature mismatch.** AC setpoint is a UI prototype, not a working control feature. |
| Medium | DHT pin/path constants are declared but a current periodic DHT acquisition-and-publish routine was not identified in `main/main.c`. | **Source-supported documentation correction.** Do not promise live environmental readings from current firmware. |
| Medium | The detection task skips beam sampling during the cooldown and drops `xQueueSend` failures without recovery; the state count assumes one person per sequential crossing. | **Strongly supported robustness concerns.** Validate simultaneous crossings, prolonged beam occlusion, PIR behavior and full queues with deterministic tests. |
| Medium | A single shared database `/command` value is overwritten by later writes and deleted after processing without verifying delete success. Token refresh constants exist, but a refresh path was not identified in the active network loop. | **Source-supported reliability concerns.** Introduce command IDs, acknowledgments and token renewal with fault-injection tests. |

**Reference specifications:** [Firebase Realtime Database REST authentication](https://firebase.google.com/docs/database/rest/auth) and [ESP-IDF HTTP client HTTPS verification](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/esp_http_client.html). These describe expected APIs; they are not evidence that the repository implementation was tested.

## Verification record

| Check | Result | Why |
| --- | --- | --- |
| Exact repository identity, branch and SHA | PASS | GitHub metadata and tracked recursive tree obtained. |
| Documentation-only changes on review branch | PASS | README and this audit note committed without changing the current firmware. |
| Current ESP-IDF firmware build | BLOCKED | No checkout or ESP-IDF toolchain. |
| Dashboard JavaScript syntax / browser execution | NOT RUN | No complete local checkout / browser environment attached to repository. |
| Firebase authenticated rule test | NOT RUN | No authorized isolated Firebase test project; existing code does not send ID tokens to RTDB. |
| Hardware occupancy, actuator and reboot tests | NOT RUN | No hardware or circuit; potentially unsafe to assume relay/atomizer behavior. |
| Project report / figures independent verification | BLOCKED | Binary `.docx` not downloadable through connected GitHub text reader. |

## Safe acceptance plan for a follow-up code fix

1. Extract occupancy transitions into a pure state-machine module and add deterministic traces for entry, exit, bounced beams, two people, PIR timeout, cooldown and saturated queue. Assert exact count and output events.
2. Implement and validate Firebase ID-token authentication in **both** clients, token expiry/renewal, restrictive RTDB rules, bounded URL/response handling and TLS trust anchors. Confirm a deliberately invalid certificate and unauthenticated request are rejected.
3. Replace the single overwritten command value with an acknowledged command record; simulate repeated commands, duplicate delivery, network interruption and failed delete.
4. Ensure final-exit writes the zero count; test dashboard behavior with a locally mocked API, including missing configuration and unsupported AC commands.
5. Build with a pinned ESP-IDF release on a clean checkout; then use an isolated bench with low-voltage loads and measure actual timing, I/O levels and startup/recovery behavior. Only then discuss live deployment.

No live configuration, deployment, merge, license change, or destructive repository clean-up is part of this review.
