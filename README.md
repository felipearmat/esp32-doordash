# Smart Intercom via Home Assistant

This project implements a multi-module smart intercom / gate entry system
with a deliberate architecture choice: **the ESP32 modules are not
autonomous devices — they are "dumb" remote terminals**. All the
intelligence (call logic, routing, echo/noise cancellation) is centralized
in Home Assistant (and, in the future, could also expose the same interface
to a dedicated Android endpoint as an additional client).

## Design summary

| | |
|---|---|
| ESPHome component | `intercom_remote` — no local audio processing at all |
| Who decides "who talks now" | Nobody locally; full-duplex raw audio, the central gateway processes it |
| Echo/noise cancellation | **Mandatory and always on** in the gateway, via `libspeexdsp` (`speex_echo_state`) — there is no "no AEC" mode |
| Call-state logic | Lives entirely in Home Assistant |
| Central controller | **Home Assistant only.** Any additional client (e.g. a native app) only consumes the same WebSocket the browser card uses — never owns the logic/AEC |
| Internal modules | **N modules** (B, C, D...), each one joins/leaves the call independently — a conference, not a fixed point-to-point link |

The hardware and wiring are described in `docs/HARDWARE.md`. All the design
effort here is in the firmware and in where audio processing happens.

## Why this design

A classic ESP32 never has enough CPU/RAM to do real acoustic echo
cancellation — a DIY NLMS filter leaves audible residual echo, and even the
S3's hardware-accelerated AEC3 is incomplete for this use case. Instead of
fighting that limitation, this project assumes **a real host does the audio
processing**: the Home Assistant server, running `libspeexdsp` (a mature C
library used by open-source softphones like Asterisk/FreeSWITCH). The
modules stay simple, cheap to maintain, and easy to debug — they only
stream PCM and report button presses. AEC is not an optional extra: without
it, the modules' raw full-duplex audio would be unusable (each side would
hear its own voice echoing back), so the gateway refuses to start if the
library isn't available, instead of silently degrading to a pass-through
mode.

**Why speexdsp and not the WebRTC Audio Processing Module**: WebRTC's APM is
more robust (it's what Chrome/Meet use), but its source is a large C++ tree
with heavy build dependencies (`gn`/`ninja`, internal Chromium headers) —
impractical to compile inside an Alpine-based add-on without a complex build
chain. `libspeexdsp` does one thing well (basic AEC/NS/AGC), has a native
Alpine package (`speexdsp-dev`), and a small Python binding that compiles in
seconds.

## Architecture

```
[Module A: 1 button]  --raw UDP PCM-->  [HA Gateway: server.py]  --raw UDP PCM-->  [Module B: 2 buttons]
        |                                     |  (AEC/routing here)                       |
        |                                     v                                            |
        +-----------------> [Home Assistant: input_select.intercom_state] <----------------+
                                       |
                                       v
                          [Lovelace card / future companion app]
```

- **Module A (outdoor unit)**: 1 button (call), mic + LM386 + analog speaker, gate lock relay.
- **Module B (indoor unit)**: 2 buttons (answer/hang up, open gate), same audio chain.
- **Gateway (`intercom_gateway` add-on)**: receives raw PCM from every module, applies AEC/NS, and mixes/routes audio between them and the browser/app.
- **Home Assistant**: owns the call state (`idle` / `ringing` / `in_call`), decides who answers, and triggers automations (missed call, notifications).

## Project structure

```
esphome/
  components/intercom_remote/   ← ESPHome component: no VOX/NLMS/AEC on-device
  intercom_a.yaml                ← outdoor module, 1 button
  intercom_b.yaml                ← indoor module, 2 buttons
  common.yaml, secrets.example.yaml
homeassistant/
  packages/intercom.yaml         ← call state machine + per-module automations
  addons/intercom_gateway/       ← gateway add-on (UDP mixer + AEC + WebSocket)
  www/intercom-card.js, dashboard-card.yaml
docs/
  HARDWARE.md                    ← pinout/wiring reference
SETUP.md
```

## Recommended install order

See `SETUP.md` for the full walkthrough — flash the ESPHome modules, install
the Home Assistant package, then the gateway add-on, then the Lovelace card.

## Multiple internal modules

Any number of internal modules (B, C, D...) is supported, not just one. Each
one can join and leave the call independently:

- **Join**: press the answer button. If the doorbell is ringing, the call
  moves to `in_call` and the module joins. If the call is already `in_call`
  (another module already joined), it simply adds itself — the conference
  scales to N simultaneous participants, each hearing the mix of everyone
  else (with per-participant AEC, see `server.py`).
- **Leave**: press the same button again. Only that module leaves; the
  others stay in the call normally.
- **Hang up for everyone**: happens automatically once the last participant
  (the last internal module to leave, and the digital pickup if it was
  active) leaves the call — only then is module A's audio also shut off and
  the state returns to `idle`.

This is handled by the gateway (`server.py`), which is a **conference
mixer**: every 20ms it cancels the echo for each active participant (one
speexdsp `EchoCanceller` per participant) and sends each of them the sum of
everyone else. See `docs/HARDWARE.md` and `esphome/intercom_c.yaml` for the
pattern to add a new module — copy `intercom_b.yaml`, change the endpoint
letter, and replicate the "PER-MODULE PATTERN" block in
`homeassistant/packages/intercom.yaml`.

**Outdoor module (A) hardware**: unaffected by the multi-module
architecture — it only affects internal modules and the gateway.
`docs/HARDWARE.md` applies exactly as written.

**Ring-to-answer duration**: `input_number.intercom_ring_seconds` controls
how long the system rings before it's considered a missed call, regardless
of how many internal modules you have.

## Tuning / next steps

- Calibrate `FILTER_TAIL_MS` in `server.py` (default 250ms) against the real
  acoustics of your installation — more reverberant environments may need a
  longer tail.
- Add a native companion app as an **additional client** (consuming the same
  gateway WebSocket as the browser card, in parallel) — never as the
  controller; the controller stays Home Assistant only.
