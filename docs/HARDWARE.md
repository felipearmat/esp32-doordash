# Prototype hardware

## Per unit
- 1x ESP32 DevKit V1 / classic ESP32
- 1x electret capsule
- 1x LM386/LM386G (module or bare IC depending on your part)
- 1x 8 Ω speaker
- 1x pushbutton
- power: 5 V for the ESP32; 5–12 V for the LM386 depending on module/circuit

## Power supply (outdoor and indoor modules)
Both modules receive **12 V** from the factory board and have an onboard
linear regulator (78M05 or equivalent) stepping it down to **5 V** — same
setup on both units.

That regulator is typically low-current (500 mA) and dissipates the voltage
difference as heat — with the ESP32 added on top of the rest of the
circuit, the combined current draw gets close to its limit.

**Recommendation for both modules**: use a switching step-down converter
(e.g. LM2596, set to 5.0 V) fed directly from the 12 V rail, replacing the
original linear regulator — more efficient (80-90% vs. ~40% for the linear
regulator) and with no risk of overheating. Apply the same fix already
validated on the outdoor module to the indoor one as well.

## Pinout
### A (outdoor module)
- GPIO18: CALL button → GND (`INPUT_PULLUP`)
- GPIO34: microphone analog input
- GPIO25: DAC → LM386 input
- GPIO14: amplifier power-gate (BD137 base, via 330Ω)
- GPIO27: gate lock trigger pulse (→ PT11 pad on the original board)

**Known gap**: module A has no LED wired to indicate that its microphone is
live. Module B/C's "Call Active LED" follows `switch.intercom_<x>_audio_active`
directly (see `homeassistant/packages/intercom.yaml`), so it lights up
regardless of how that switch got turned on. Module A has no equivalent —
if you want the same guarantee on the outdoor unit, wire an LED to a free
GPIO (e.g. GPIO32) and add a matching `light`/automation pair; this isn't
done by default because it requires a hardware change, not just config.

### B (indoor module)
- GPIO18: ANSWER/HANG UP button → GND
- GPIO19: OPEN GATE button → GND
- GPIO35: microphone analog input
- GPIO26: DAC → LM386 input
- GPIO4: ringing LED
- GPIO5: call-active LED

## Electret capsule
A bare capsule needs bias voltage and, ideally, pre-amplification. Minimal
test circuit:

3V3 -- 2.2 kΩ --+-- MIC+
                |
              1 µF (coupling) --> ADC node
MIC- -------- GND

Bias the ADC node to ~1.65 V with a 100 kΩ/100 kΩ divider between 3V3 and
GND. Add 100 nF from the bias node to GND.

**Recommended**: add a microphone preamp (MAX4466/MAX9814 or equivalent)
before the ADC. Without one, expect low volume and noise.

## LM386
DAC GPIO25/26 → coupling capacitor (100 nF–1 µF) → LM386 input. Use the
circuit recommended by your module/datasheet. The ESP32 and amplifier
grounds must be shared.

Keep the speaker physically away from the capsule and use foam/mechanical
isolation to reduce acoustic feedback.
