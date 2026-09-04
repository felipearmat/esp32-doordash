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
- GPIO14: amplifier power-gate — drives the base of Q2 (reused factory
  transistor), which in turn drives Q3 (reused factory PNP Darlington). See
  "Amplifier power-gate" below — this replaced an earlier BD137-based plan
  that was abandoned before being installed.
- GPIO27: gate lock trigger — drives the base of Q1 (reused factory NPN
  Darlington) directly. See "Gate lock trigger" below — this replaced the
  original `GPIO27 → PT11 pad` plan, which turned out to be a dead end (see
  that section for why).

**Known gap**: module A has no LED wired to indicate that its microphone is
live. Module B/C's "Call Active LED" follows `switch.intercom_<x>_audio_active`
directly (see `homeassistant/packages/intercom.yaml`), so it lights up
regardless of how that switch got turned on. Module A has no equivalent —
if you want the same guarantee on the outdoor unit, wire an LED to a free
GPIO (e.g. GPIO32) and add a matching `light`/automation pair; this isn't
done by default because it requires a hardware change, not just config.

## Gate lock trigger (Q1, reused from the factory board)

The factory board already had a discrete driver stage for the gate lock
output, built around two complementary Darlington power transistors — **Q1**
(NPN, marked `J122G` = MJD122G) and **Q3** (PNP, marked `J127G` = MJD127G) —
plus a small transistor **Q2** and a bias network (R5, R58, R6, RM1, C6).
Both Q1 and Q3 have their collector tab already soldered to one pole each of
the gate lock mechanism's 2-wire output (call them pole A = Q1's collector,
pole B = Q3's collector). A flyback diode, **D4**, is already present across
these two poles (cathode on the pole B/+12V side, anode on the pole A side)
— correctly oriented, no change needed there.

**Why the original design doesn't work as-is**: the original trigger path was
`(removed daughter-board module) → PT11 pad → connector 1 (out to that
module) ... connector 2 (back from that module) → Q1's base`. That daughter
module is no longer part of this build, so `PT11` is now a dead end — wiring
a GPIO to it does nothing, since it only ever led *out* to the missing
module, never directly to Q1's base. Q3's own base/emitter were driven
through the same missing module via the R5/R58/R6/RM1/C6 bias network
(alongside the small transistor Q2), which this design does not attempt to
reconstruct.

**New approach**: use only Q1, isolated from the legacy network, as a simple
GPIO-driven low-side switch. Q3's collector tab is fully desoldered from pole
B and reused elsewhere (see "Amplifier power-gate" below) — pole B gets its
own direct wire to +12V instead.

```
Q1 collector (tab)        — unchanged, still wired to pole A of the gate mechanism
Q1 base (left leg)        — NEW wire → 1kΩ resistor → GPIO27
Q1 emitter (right leg)    — NEW wire → GND (shared with ESP32 GND)

Pole B of the gate mechanism (where Q3's tab used to be) — NEW wire → +12V
```

GPIO27 high → Q1 saturates → pulls pole A to GND while pole B sits fixed at
+12V → gate mechanism energized. GPIO27 low → Q1 cuts off → mechanism
de-energized. D4 (already in place) protects Q1 from the inductive
kickback when it switches off.

## Amplifier power-gate (Q2 + Q3, reused from the factory board)

Reuses the same **Q2** (small NPN, SOT-23-style package — two legs together
on one side of the body, a third leg alone on the opposite side, centered;
pinout confirmed by hFE testing on this specific unit — the two same-side
legs are base and emitter, the lone opposite-side leg is collector; do not
assume this matches other small transistors, verify each unit) and **Q3**
(PNP MJD127G, DPAK, freed from the gate lock circuit above) instead of
adding a new BD137.

Both Q1 and Q3 (DPAK) have a "middle" lead-frame position that is trimmed at
the factory, since it's internally tied to the same node as the tab — the
tab is that connection's only accessible point (soldered flat against the
PCB, doubling as heatsink), not a separate front pin. Both Q1 and Q3 were
confirmed by direct diode-mode/hFE testing (not inferred from tracing the
legacy circuit — that inference turned out wrong for Q1, see below) to
share the **same** orientation: tab = collector, left leg = base, right leg
= emitter.

**Correction (found during bring-up)**: Q1's base/emitter were originally
assumed from tracing the legacy circuit — the leg wired to Connector 2 was
assumed to be the base. That assumption was wrong: direct testing confirmed
the **left** leg is Q1's base (not the right leg as first documented). If
you're following this doc, wire the base resistor to Q1's left leg and the
GND wire to Q1's right leg — the reverse of what an earlier version of this
document said. Because Q3 is PNP, it cannot be driven
directly by a GPIO the way an NPN low-side switch can — Q2 is wired as a
small inverter stage to drive it correctly. Two resistors were reused from
the same legacy bias network: **R5 (4.7kΩ)** and **R6 (10kΩ)**.

```
GPIO14 ──[R5, 4.7kΩ]── Q2 base
Q2 emitter ── GND
Q2 collector ──┬── Q3 base
               │
            [R6, 10kΩ]
               │
             +Vcc (confirmed: the 12V rail, through a series resistor
               │   already on the factory board — not 5V)
          Q3 emitter
               │
          Q3 collector (tab) ──→ LM386 module's V+ pin
```

**Important**: this switches the LM386's **V+ line**, not its GND line — the
V+ trace between the supply rail and the LM386 module must be the one
physically interrupted here, not GND as an earlier (abandoned) BD137-based
plan would have done.

Logic: GPIO14 high → Q2 saturates → pulls Q3's base down near GND → Q3
(PNP) turns on (base far enough below its emitter) → LM386 powered → amp on.
GPIO14 low → Q2 cuts off → R6 pulls Q3's base back up to +Vcc → Q3 turns
off → LM386 unpowered → amp off. This preserves the same active-high
`amp_enable` logic the firmware already expects — no firmware change
needed.

**Verified**: GPIO14 activation tested directly from the ESP32 and confirmed
working correctly.

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
