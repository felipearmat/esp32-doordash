# Setup — Smart Intercom via Home Assistant

## 1. ESPHome
Copy the `esphome/` folder into an ESPHome Device Builder project folder.
Create `secrets.yaml` from `secrets.example.yaml` (it already includes
`api_key_a`, `api_key_b`, and `api_key_c` — generate your own key per
device).

Compile `intercom_a.yaml` first, then each internal module
(`intercom_b.yaml`, `intercom_c.yaml`, ...). This project ships with two
example internal modules (B and C) — to add a third, copy
`intercom_c.yaml`, change `devicename`, `friendly_name` (e.g. "Intercom D"),
the `api_key_d` key, and `endpoint: "D"`. No other firmware change is
needed.

After they join Home Assistant, confirm the real entity names — the
`friendly_name` values follow the `Intercom <LETTER>` pattern on purpose, so
that `homeassistant/packages/intercom.yaml` works without manually adjusting
IDs (e.g. `Intercom D` → `binary_sensor.intercom_d_...`).

## 2. Home Assistant package
In `configuration.yaml`, enable:

```yaml
homeassistant:
  packages: !include_dir_named packages
```

Copy `homeassistant/packages/intercom.yaml` to `/config/packages/intercom.yaml`
and restart Home Assistant.

If you add a new module (D, E...), duplicate the blocks marked "PER-MODULE
PATTERN" in the file (input_boolean, LED automation, button automation),
swapping in the new letter — the generic scripts
(`intercom_toggle_module`, `intercom_check_hangup`) need no changes at all,
they enumerate modules dynamically.

## 3. Gateway local add-on
On Home Assistant OS/Supervised, create `/addons/intercom_gateway/` and copy
the files from `homeassistant/addons/intercom_gateway/`. Install and start
the local add-on.

Ports used:
- UDP 6055: inbound from the ESP32 modules
- UDP 6056: audio to module A
- UDP 6057: audio to module B
- TCP/WebSocket 8099: browser/app

**AEC is mandatory in this add-on** — the `Dockerfile` installs
`speexdsp-dev`, `swig`, and the build toolchain needed to compile the
Python binding for `libspeexdsp`, then removes those build packages
afterward (the `speexdsp` shared library itself stays installed to run). If
the add-on build fails, check the log: `server.py` raises a clear error if
`import speexdsp` fails, instead of starting in a degraded mode with no
echo cancellation.

## 4. Card
Copy `homeassistant/www/intercom-card.js` to `/config/www/intercom-card.js`.
Add the Lovelace resource `/local/intercom-card.js` as a **JavaScript
Module** and use the YAML from `homeassistant/dashboard-card.yaml`.

## 5. Incremental test
1. A, B, and C online in Home Assistant.
2. Press A's button: state becomes `ringing`, B and C ring.
3. Press B's button: state becomes `in_call`; B joins the conference with A.
4. Press C's button (while the call is still ongoing): C joins too — now A,
   B, and C all hear each other simultaneously.
5. Press B's button again: B leaves the call; A and C continue.
6. Press C's button: C leaves — since nobody is left, the call ends on its
   own (A's audio is shut off, state returns to `idle`).
7. Check the gateway's `/health` endpoint at `http://HA_IP:8099/health` —
   `active_participants` shows in real time who's in the conference right
   now, and `aec`/`aec_filter_tail_ms` confirm echo cancellation is active
   for everyone.

## Behavior notes

- **Conference, not point-to-point**: any combination of modules can be in
  the call at the same time. There's no automatic speaker switching (no
  VOX) — every participant is always "open"; the gateway's AEC is what
  makes that usable without feedback.
- **Single controller**: Home Assistant only. There's no "gateway running
  on a phone/tablet" mode — any additional client is just that, a client,
  never the controller.
- **Leave vs. hang up**: a module leaving the call doesn't drop the others.
  The call only ends for everyone once the last participant (physical
  module or digital pickup) leaves — via `intercom_check_hangup`. To force
  the call to end for everyone at once (e.g. an emergency button), call the
  `intercom_hangup_all` script.
- **Less per-module tuning**: there are no `vox_threshold`, `nlms_taps`, or
  similar parameters to calibrate per device — fine-tuning is centralized
  in a single place, `FILTER_TAIL_MS`.
