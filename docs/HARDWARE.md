# Prototype hardware

## Per unit
- 1x ESP32 DevKit V1 / classic ESP32
- 1x electret capsule
- 1x MAX98357A I2S Class-D amplifier breakout (replaced the LM386 + BD137/Q2/Q3
  power-gate chain entirely — see "Amplifier (MAX98357A)" below)
- 1x 8 Ω speaker
- 1x pushbutton
- power: 5 V for the ESP32 and for the MAX98357A's VIN

**Module A specifically** (built standalone, not reusing the Hello+ factory
board — see "Gate lock trigger" below): 1x Q1 (NPN Darlington, MJD122G,
salvaged from the Hello+ board) + 2x resistor (1kΩ, 15kΩ — 15kΩ substituted
for the originally-specced 10kΩ pulldown, negligible difference here) + 1x
1N4007 diode, plus a mic-active status LED (1x LED + 1x 1kΩ on GPIO32 —
closes the "no mic-active indicator" gap noted below) and reverse-polarity
protection on the +12V input (1x 1N5819/SS34 Schottky in series). All on
one hand-fabricated board, a from-scratch design that does not reuse the
Hello+ layout — supersedes the older FIG. 6 PNP+pilot design. Also uses 2x
2-position screw terminal blocks (5mm pitch) for the gate lock's OUT/+12V
pair and the mic capsule's 2 wires, in addition to the bare pads for each.

**Fabrication method for this board: hand-engraving with a rotary tool**
(not chemical etching) — every different-net copper clearance was widened
to ≥1.0mm (verified with an automated geometric checker, not just eyeballed)
and terminal pitch bumped to ≥4mm. Each functional block also gets its own
local GND terminal rather than sharing one long trunk trace, since fewer
long traces crossing the board makes hand-engraving far more forgiving.

## Power supply (outdoor and indoor modules)
Module A gets **12 V** directly from the gate/intercom building wiring (no
longer through the Hello+ factory board — that board isn't part of this
build anymore). Modules B/C still receive **12 V** from their own factory
board — and that board's supply is **not** a simple 78M05 linear regulator
as originally assumed; it's a small isolated flyback switching supply, ID'd
by its markings:

```
AC in ── MB10F (bridge rectifier, 2× AC pins → 2× DC pins)
              │
              ▼
        Q0365R (Fairchild Power Switch — PWM controller + power
                 MOSFET in one DIP-8 package, drives the flyback
                 transformer's primary)
              │
     ┌────────┴────────┐
     │                 │
  transformer      ORPC-817C (U4) — PC817-type optocoupler,
  secondary          isolated feedback from the DC output back
     │                to Q0365R's control loop
     ▼
  SS34 (Schottky, 3A/40V) — sits between the filter caps and the
  input of the 78M05-style linear regulator (confirmed role below)
     │
     ▼
  regulator input (originally 78M05, now replaced by the LM2596
  step-down — wired in downstream of SS34, so the diode still does
  its job)
```

This means: **don't assume the 78M05 efficiency-upgrade recommendation below
applies to B/C** — a flyback switcher is already ~80%+ efficient, so there's
no linear-regulator heat problem to fix there. The 78M05/LM2596 swap is only
relevant if a module's 12V→5V step actually turns out to be linear (verify
per-module before touching anything).

**SS34's confirmed role**: it sits right between the flyback's filter
caps and the regulator's input (not on the final output rail as first
guessed) — a **reverse-current / back-feed isolation diode**, protecting the
flyback supply's output caps (and the Q0365R control loop feeding off them)
from anything that could push current backward from the regulator side —
e.g. a backup battery sharing that same input node, or charge stored in the
regulator's own input/output capacitors after AC is removed. Confirmed
important when swapping the regulator for the LM2596 step-down: **keep the
step-down's input wired downstream of SS34** (same node the old 78M05's
input used), never tapped directly off the filter caps — this preserves the
isolation. This has already been done correctly on the module this was
checked on.

That original 78M05-style regulator (where still applicable) is typically
low-current (500 mA) and dissipates the voltage difference as heat — with
the ESP32 added on top of the rest of the circuit, the combined current draw
gets close to its limit.

**Recommendation, where a module's regulator is confirmed linear**: use a
switching step-down converter (e.g. LM2596, set to 5.0 V) fed directly from
the 12 V rail — more efficient (80-90% vs. ~40% for a linear regulator) and
with no risk of overheating.

## Pinout
### A (outdoor module)
- GPIO18: CALL button → GND (`INPUT_PULLUP`)
- GPIO34: microphone analog input
- GPIO26: I2S BCLK → MAX98357A
- GPIO25: I2S LRC (word select) → MAX98357A
- GPIO33: I2S DIN → MAX98357A
- GPIO14: amplifier power-gate — drives the MAX98357A's SD (shutdown) pin
  directly, no transistor stage needed anymore. See "Amplifier (MAX98357A)"
  below.
- GPIO27: gate lock trigger — drives the base of Q1 (salvaged NPN
  Darlington) directly through a 1kΩ resistor, no pilot transistor needed.
  See "Gate lock trigger" below for the full board.

**Known gap**: module A has no LED wired to indicate that its microphone is
live. Module B/C's "Call Active LED" follows `switch.intercom_<x>_audio_active`
directly (see `homeassistant/packages/intercom.yaml`), so it lights up
regardless of how that switch got turned on. Module A has no equivalent —
if you want the same guarantee on the outdoor unit, wire an LED to a free
GPIO (e.g. GPIO32) and add a matching `light`/automation pair; this isn't
done by default because it requires a hardware change, not just config.

## Gate lock trigger (Q1 salvaged, standalone board)

**Module A no longer reuses the Hello+ factory board at all.** The only part
salvaged from it is **Q1** (NPN Darlington, marked `J122G` = MJD122G, DPAK —
tab = collector, left leg = base, right leg = emitter, confirmed by hFE
testing — same confirmed orientation as Q3, which is *not* used here).
Everything else — Q2, Q3, D4, the R5/R58/R6/RM1/C6 bias network, and the
factory PCB itself — is left behind; none of it is used anymore. This
section's earlier FIG. 6 design (Q3 as a PNP high-side switch with a BC548
pilot transistor) is superseded — Q1 is NPN, so it works as a much simpler
low-side switch instead.

**Approach**: Q1 wired as a standalone **low-side NPN switch** — GPIO27
drives its base directly through a resistor, no pilot transistor needed,
since Q1's emitter sits at GND (the same reference GPIO27 uses).

```
GPIO27 ──[R_base, 1kΩ]── Q1 base ──[R_pulldown, 15kΩ]── GND
                          Q1 emitter ── GND
                          Q1 collector (tab) ── OUT

+12V raw ──[PROT, SS34 Schottky, series]── +12V protected
+12V protected ── pole B of the gate lock mechanism
OUT ── pole A of the gate lock mechanism
D1 (1N4007) across OUT/+12V protected: anode = OUT, cathode = +12V (flyback clamp)
```

GPIO27 high → Q1 saturates → pole A pulled to GND while pole B sits fixed
at +12V → gate mechanism energized. GPIO27 low → Q1 cuts off → mechanism
de-energized. R_pulldown (new — keeps the lock off if GPIO27 floats during
ESP32 boot/reset, the same lesson the old Q2 floating-base issue taught).
D1 (new; D4 stays behind on the old board) clamps the inductive kickback —
same orientation as the original factory D4 (cathode on the fixed +12V
side, anode on the switched side), since Q1 is low-side just like the
original factory design was. PROT (new, 1x SS34 Schottky) sits in series
with the raw +12V input, protecting the board from reverse polarity —
everything downstream (D1, the lock mechanism) runs off its protected side.

**Standalone board terminals** (5 wires cross to the rest of the system):
`GPIO27`, `GND` (2 separate local-ground wires in practice — one for the
base pulldown, one for Q1's emitter, both landing on the same system
ground), `+12V raw` (in, ahead of PROT), and the gate lock's own 2 wires
— `OUT` (pole A) / `+12V protected` (pole B) — which can land on bare pads
or on the 2-position terminal block mentioned above.

## Amplifier (MAX98357A — replaced LM386 + BD137/Q2+Q3)

The whole analog output chain (DAC → coupling cap → LM386, power-gated by a
transistor stage) was replaced after the LM386 itself was damaged and, in
parallel, the Q2+Q3 power-gate turned out to have a real bug (+12V was
leaking into Q2's base from somewhere on the reused factory board, keeping
the amp on regardless of GPIO14 — see the project's own debugging history
for that chase, kept below since the *gate lock* section's lessons still
apply there).

**MAX98357A** takes **I2S digital audio in** and drives the speaker directly
— no DAC pin, no coupling capacitor, no power transistor:

```
VIN  ── 5V
GND  ── GND
SD   ── GPIO14 (amp_enable — same firmware output as before, now a direct
         logic-level pin instead of a transistor base)
GAIN ── left floating (default 9dB; see datasheet for other fixed gains)
BCLK ── GPIO26
LRC  ── GPIO25
DIN  ── GPIO33
Speaker +/- ── straight to the 8Ω speaker, no capacitor needed
```

Logic: GPIO14 high → MAX98357A active → I2S audio plays. GPIO14 low →
MAX98357A shuts down (its own internal low-power state, not a physical power
cut) → silent. Same active-high `amp_enable` semantics the firmware already
used; `amp_warmup_ms`/`amp_idle_timeout_ms` still apply unchanged.

**Firmware note**: this required a real code change, not just rewiring —
`intercom_remote`'s playback path moved from a per-sample `dacWrite()` loop
to the ESP32's I2S peripheral (`driver/i2s.h`), and the component's config
changed from a single `dac_pin` to `i2s_bclk_pin`/`i2s_lrc_pin`/`i2s_din_pin`.
Capture (mic → ADC) is unchanged.

**Leftover parts**: Q2 and Q3 (and R5/R6, the pull-down) are no longer used
for anything and can stay desoldered/unused. It's **Q1** that's salvaged and
reused in the new gate lock driver board (see "Gate lock trigger" above) —
don't discard it along with the rest of the old amp-switch chain.

<details>
<summary>History: the LM386 + BD137/Q2+Q3 chain this replaced</summary>

Reused **Q2** (small NPN, SOT-23-style package — two legs together on one
side of the body, a third leg alone on the opposite side, centered; pinout
confirmed by hFE testing on this specific unit — the two same-side legs are
base and emitter, the lone opposite-side leg is collector) and **Q3** (PNP
MJD127G, DPAK, freed from the gate lock circuit) instead of adding a new
BD137, driving the LM386's **V+ line** (not GND):

```
GPIO14 ──[R5, 4.7kΩ]── Q2 base
Q2 emitter ── GND
Q2 collector ──┬── Q3 base
               │
            [R6, 10kΩ]
               │
             +Vcc (the 12V rail, through a series resistor already on the
               │   factory board — not 5V)
          Q3 emitter
               │
          Q3 collector (tab) ──→ LM386 module's V+ pin
```

This worked in the sense that GPIO14 correctly turned the amp on — but a
real bug remained: with GPIO14 disconnected/floating, the amp still turned
on, and Q2's base measured ~12V directly, well past what a 47kΩ pull-down
(added to rule out a floating base picking up noise) could hold down. That
pointed to a genuine low-impedance leak from the +12V rail into Q2's base —
likely a stray trace on the reused factory board — never fully traced before
switching to the MAX98357A instead of continuing the hunt.

Both Q1 and Q3 (DPAK) have a "middle" lead-frame position trimmed at the
factory (tied internally to the tab, which is the only accessible point for
that connection — soldered flat, doubling as heatsink). Both were confirmed
by direct diode-mode/hFE testing to share the same orientation: tab =
collector, left leg = base, right leg = emitter. This confirmed pinout is
exactly what the new Q1-only gate lock board (see "Gate lock trigger" above)
relies on — no need to re-test it, just don't confuse it with an untested
datasheet assumption.
</details>

### B (indoor module) and C (second indoor module)
Both share the same pinout — separate physical ESP32 boards, same YAML shape
(`intercom_b.yaml` / `intercom_c.yaml`):
- GPIO18: ANSWER/HANG UP button → GND
- GPIO19: OPEN GATE button → GND
- GPIO35: microphone analog input
- GPIO4: ringing LED
- GPIO5: call-active LED
- GPIO14: amplifier power-gate — MAX98357A SD pin, same as A
- GPIO26: I2S BCLK → MAX98357A
- GPIO25: I2S LRC (word select) → MAX98357A
- GPIO33: I2S DIN → MAX98357A

Checked against this module's own GPIO usage (2, 4, 5, 14, 18, 19, 35 — see
above) before reusing A's 26/25/33 numbers: none of them are wired to
anything else on B/C, so reusing the same pin numbers is safe here too.

## Electret capsule
A bare capsule needs bias voltage and, ideally, pre-amplification. Circuit
used on Module A's board (values as actually built, not just a minimal
reference design):

```
3V3 ──[R1, 2.2kΩ]── MIC+ ──[C1a 470µF ‖ C1b 220µF ‖ C1c 220µF, all
                             rectangular polyester, in parallel]── ADC node
MIC- ── GND (the capsule's minus leg and the system GND wire share this pad)
3V3 ──[R2, 100kΩ]── ADC node ──[R3, 100kΩ]── GND   (bias divider, ~1.65V)
ADC node ──[C2, 100nF]── GND                        (filter)
ADC node ── GPIO34 (single wire, direct to the ESP32)
```

3 smaller capacitors in parallel (470µF+220µF+220µF) were used in place of
one larger coupling cap, matching parts actually on hand — same net effect
as a single ~910µF cap.

**Recommended**: add a microphone preamp (MAX4466/MAX9814 or equivalent)
before the ADC. Without one, expect low volume and noise.

## MAX98357A
I2S (BCLK/LRC/DIN) straight from the ESP32 to the module — no DAC pin, no
coupling capacitor, no separate power transistor. See "Amplifier (MAX98357A)"
above for the full pinout and the firmware change it required. The ESP32 and
amplifier grounds must still be shared.

Keep the speaker physically away from the capsule and use foam/mechanical
isolation to reduce acoustic feedback.
