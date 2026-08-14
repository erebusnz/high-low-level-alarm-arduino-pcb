/*
 * Replacement PCB for Wallace High Low Level Alarm (1950610)
 * Firmware for ATtiny202 — megaTinyCore (Spence Konde)
 *
 * Clock:   20 MHz (megaTinyCore sets the prescaler per the Tools menu —
 *          CLK_PER = F_CPU, not the factory ÷6 default)
 * BOD:     level 7 (~4.5 V — set via fuses in Tools menu; code keeps it on in
 *          sleep so WDT reset is clean during brownout)
 * WDT:     1 s timeout
 *
 * ═══════════════════════════════════════════════════════════════════════════════
 *  BUILD SETTINGS (Arduino IDE — Tools menu)
 * ═══════════════════════════════════════════════════════════════════════════════
 *   Board:                "ATtiny202/402/204/404"
 *   Chip:                 "ATtiny202" (2 KB flash, SOIC-8)
 *   Clock:                "20 MHz internal" (the core programs the prescaler
 *                          to this selection — CLK_PER = 20 MHz)
 *   Millis timer:         "RTC"  ← REQUIRED — TCA0 is taken over for PWM
 *   BOD:                  "4.5 V" (level 7)
 *   Attach:               "UPDI (Microchip) — 4.7k + Schottky"
 *
 * The TCA0 takeover means micros() is unavailable.
 * micros() is not used by this sketch.
 *
 * ═══════════════════════════════════════════════════════════════════════════════
 *  PIN ASSIGNMENTS (ATtiny202, SOIC-8)
 * ═══════════════════════════════════════════════════════════════════════════════
 *   Pin 1  VDD   +5 V
 *   Pin 2  PA6   FLOAT_SW (in, divider → 4.59 V when float closed)
 *   Pin 3  PA7   LED flash string gate (out → Q1 2N7002, active high)
 *   Pin 4  PA1   Piezo gate (out → Q2 2N7002, TCA0 WO1)
 *   Pin 5  PA2   Relay gate (out → Q3 2N7002, level-shifts to Q4 PNP high-side)
 *   Pin 6  PA0   UPDI (programming only — not used by firmware)
 *   Pin 7  PA3   Mute button (in, internal pull-up, active low)
 *   Pin 8  GND   Ground
 *
 * ═══════════════════════════════════════════════════════════════════════════════
 *  BEHAVIOUR
 * ═══════════════════════════════════════════════════════════════════════════════
 *   - Float closed (PA6 high) → 2 Hz LED flash starts immediately.
 *   - Float closed ≥ 2 s    → relay engages (CN2-4 sources +12 V to pump).
 *   - Float closed ≥ 5 s    → piezo alarm sounds (unless muted).
 *   - Mute button toggles   → piezo only — relay and LEDs are unaffected.
 *   - Float opens           → LEDs off, piezo off, mute auto-clears.
 *     Relay stays on ≥ 5 s to prevent chatter.
 *
 * ═══════════════════════════════════════════════════════════════════════════════
 *  ALARM SIGNAL
 * ═══════════════════════════════════════════════════════════════════════════════
 * The alarm combines frequency modulation (warbling around the piezo element's
 * mechanical resonant frequency) with temporal modulation (periodic silence).
 *
 *   FAST modulation (warble):
 *     F_LOW  = F_RES × 0.85
 *     F_HIGH = F_RES × 1.15
 *     Sweep  F_LOW → F_HIGH in 150 ms, then F_HIGH → F_LOW in 150 ms.
 *     Each complete warble cycle = 300 ms (~3.3 Hz).
 *     At 3.33 MHz CLK_PER, PER is linearly interpolated between per_low
 *     and per_high at ~1.7 ticks/ms — the ~2 % frequency error at the
 *     midpoint of the sweep is well inside the piezo's resonance bandwidth.
 *
 *   SLOW modulation (temporal):
 *     3000 ms warbling, 300 ms silence.  ~0.7 Hz.
 *     The silence-to-sound transition creates a strong perceptual onset
 *     each cycle; the interruption reduces adaptation to the warble.
 *
 *   Pattern:
 *     ┌──── 3000 ms ─────┐
 *     │ WARBLE WARBLE …   │  300 ms
 *     │ (10 complete      │  SILENCE
 *     │  warble cycles)   │
 *
 *   Every sweep crosses F_RES, producing maximum or near-maximum acoustic
 *   output repeatedly, while the changing frequency attracts attention and
 *   is harder to perceptually tune out than a constant tone.
 *
 *   The constant F_RES should be measured for your specific piezo element.
 *   The Murata PKM13EPYH4000 (recommended in the BOM) is specced at
 *   4.0 kHz ± 0.5 kHz.  Default: 4000 Hz.
 *
 * ═══════════════════════════════════════════════════════════════════════════════
 *  SAFETY PROPERTIES
 * ═══════════════════════════════════════════════════════════════════════════════
 *   - All outputs start LOW in setup().
 *   - R9 (10 kΩ hardware pull-down on Q3 gate) holds the relay OFF during
 *     reset, brownout, and UPDI programming — before setup() runs.
 *   - Q2 (piezo gate) can float during reset without sounding — a passive
 *     piezo element passes no DC.
 *   - Watchdog resets the MCU if loop() hangs for >1 s.
 *   - BOD resets the MCU before VDD drops below ~4.5 V, preventing undefined
 *     behaviour that could glitch the relay.
 *   - Mute never gates the relay — silencing the alarm must not stop the pump.
 *   - Mute auto-clears when the float drops so the next alarm event always
 *     sounds.
 *
 * License: CERN-OHL-W-2.0
 */

#include <Arduino.h>
#include <avr/io.h>
#include <avr/wdt.h>

// ══════════════════════════════════════════════════════════════════════════════
//  Compile‑time guard — TCA0 is taken over for piezo PWM
// ══════════════════════════════════════════════════════════════════════════════

#ifdef MILLIS_USE_TIMERA0
  #error \
    "This sketch takes over TCA0 for piezo PWM. " \
    "In the Arduino IDE Tools menu, set 'Millis timer' to 'RTC' " \
    "(or TCB0 if your part has one). RTC-based millis() is standard " \
    "on megaTinyCore ≥ 1.1.5. micros() will be unavailable — this " \
    "sketch does not use it."
#endif

// ══════════════════════════════════════════════════════════════════════════════
//  Pin definitions
// ══════════════════════════════════════════════════════════════════════════════

#define PIN_FLOAT     PIN_PA6   // Float switch (digital input, R1/R2 divider)
#define PIN_LED       PIN_PA7   // LED flash string gate (Q1 2N7002, active high)
#define PIN_PIEZO     PIN_PA1   // Piezo gate (Q2 2N7002, TCA0 WO1 PWM)
#define PIN_RELAY     PIN_PA2   // Relay gate (Q3 2N7002, active high)
#define PIN_MUTE      PIN_PA3   // Mute button (internal pull-up, LOW = pressed)

// ══════════════════════════════════════════════════════════════════════════════
//  Tuning constants
// ══════════════════════════════════════════════════════════════════════════════

#define DEBOUNCE_FLOAT_MS     50UL
#define DEBOUNCE_MUTE_MS      20UL
#define ALARM_DELAY_MS        5000UL   // Float closed → piezo after 5 s
#define PUMP_DELAY_MS         2000UL   // Float closed → relay after 2 s
#define PUMP_MIN_RUN_MS       5000UL   // Minimum relay on-time (anti-chatter)

// ══════════════════════════════════════════════════════════════════════════════
//  Alarm pattern — measure F_RES for your piezo element
// ══════════════════════════════════════════════════════════════════════════════
//
// The PKM13EPYH4000 (BOM) is specced at 4.0 kHz ± 0.5 kHz resonance.
// Replace F_RES below with the value you measure on your specific element.

#define F_RES              4000UL   // Piezo resonant frequency (Hz)
#define F_LOW              ((F_RES) * 85 / 100)   // 0.85 × F_RES
#define F_HIGH             ((F_RES) * 115 / 100)  // 1.15 × F_RES

// Fast modulation — one sweep direction
#define SWEEP_TIME_MS      150UL

// Slow modulation — temporal on/off
#define ALARM_ON_MS        3000UL   // Warbling duration
#define ALARM_OFF_MS       300UL    // Silence duration

// ══════════════════════════════════════════════════════════════════════════════
//  TCA0 PER values — computed at runtime from F_RES and F_CPU
// ══════════════════════════════════════════════════════════════════════════════
//
// CLK_PER = F_CPU. NOTE: megaTinyCore's init() reprograms the main clock
// prescaler to match the Tools ▸ Clock menu selection — the chip does NOT
// stay at the factory OSC20M ÷ 6 default. With "20 MHz internal" selected,
// CLK_PER is a full 20 MHz. Hardcoding 3,333,333 here made every PWM
// frequency 6× too high (the 4 kHz warble came out at ~20–28 kHz —
// ultrasonic, hence "no sound"). Use the core's F_CPU macro, which always
// matches the menu selection.
// PER = F_CPU / freq − 1
//
// per_low  is the PER value for F_LOW  (larger  — lower frequency)
// per_high is the PER value for F_HIGH (smaller — higher frequency)
//
// The sweep linearly interpolates PER between these endpoints each ms.
// PER and CMPn are double-buffered in TCA0 single-slope mode — writes take
// effect atomically on the next counter wrap, so no glitch is possible.

#define F_CPU_HZ           ((uint32_t)F_CPU)

// ══════════════════════════════════════════════════════════════════════════════
//  State
// ══════════════════════════════════════════════════════════════════════════════

static uint16_t high_ms      = 0;       // ms float has been continuously high
static uint32_t relay_on_ms  = 0;       // ms relay has been energised
static bool     mute         = false;   // true → piezo silenced

// ── Float debounce state ────────────────────────────────────────────────────

static uint8_t  float_db_ctr     = 0;
static bool     float_stable     = false;   // debounced float reading

// ── Mute button state ────────────────────────────────────────────────────────

static uint8_t  mute_db_ctr      = 0;
static bool     mute_stable      = true;    // pull-up → idle HIGH
static bool     mute_prev_stable = true;    // for falling-edge detection

// ── Alarm modulation state ───────────────────────────────────────────────────

static uint16_t per_low;     // PER at F_LOW (larger value)
static uint16_t per_high;    // PER at F_HIGH (smaller value)

typedef enum {
    PHASE_ACTIVE,             // 1200 ms warbling
    PHASE_SILENT,             // 200 ms silence
} alarm_phase_t;

static alarm_phase_t alarm_phase  = PHASE_ACTIVE;
static uint16_t      alarm_timer  = 0;      // ms in current phase
static uint16_t      sweep_ms     = 0;      // position within current sweep direction
static bool          sweep_up     = true;   // true → F_LOW→F_HIGH (PER decreasing)

// ══════════════════════════════════════════════════════════════════════════════
//  PWM — TCA0 single-slope, WO1 on PA1
// ══════════════════════════════════════════════════════════════════════════════
//
// megaTinyCore defaults TCA0 to split mode (six 8‑bit PWM channels).
// We need 16‑bit single mode, so we take full ownership.
//
// On 8‑pin parts megaTinyCore remaps WO0 from PA3 to PA7 via PORTMUX.
// We only enable WO1 (CMP1) which is always on PA1 — PORTMUX is irrelevant.

static void piezo_pwm_init(void) {

    // Step 1 — disable split mode, issue hardware reset, release TCA0
    takeOverTCA0();

    // Step 2 — compute PER endpoints for the frequency sweep
    //
    //   F_LOW  → longest period  → largest  PER = F_CPU / F_LOW  − 1
    //   F_HIGH → shortest period → smallest PER = F_CPU / F_HIGH − 1
    //
    // These are uint16_t: at 20 MHz and F_LOW ≥ 2550 Hz (0.85 × 3000),
    // PER ≤ 7842, well within range.

    per_low  = F_CPU_HZ / F_LOW  - 1;
    per_high = F_CPU_HZ / F_HIGH - 1;

    // Step 3 — configure single-slope PWM on WO1 only
    PORTA.DIRSET = PIN1_bm;

    TCA0.SINGLE.CTRLB = TCA_SINGLE_WGMODE_SINGLESLOPE_gc   // single-slope
                       | TCA_SINGLE_CMP1EN_bm;              // WO1 on PA1

    TCA0.SINGLE.PER   = per_low;                           // safe initial value
    TCA0.SINGLE.CMP1  = 0;                                 // start silent

    // Step 4 — start the timer (enable and clock-select in one write to avoid
    //          a glitch period with the wrong prescaler)
    TCA0.SINGLE.CTRLA = TCA_SINGLE_CLKSEL_DIV1_gc           // CLK_PER = 3.33 MHz
                       | TCA_SINGLE_ENABLE_bm;
}

/*
 * Update the TCA0 period and duty registers for the current sweep position.
 * Called once per ms while the alarm is in PHASE_ACTIVE.
 *
 * PER is linearly interpolated between per_low and per_high, crossing the
 * resonant PER at the midpoint of each sweep direction.  The ~2 % frequency
 * error from the linear-PER approximation is well inside the piezo's
 * resonance bandwidth.
 *
 * PER and CMP1 are double-buffered — writes take effect together at the
 * next counter wrap, so there is never a glitch on the output pin.
 */
static void update_piezo_sweep(void) {
    uint32_t scaled = (uint32_t)(per_low - per_high) * sweep_ms / SWEEP_TIME_MS;
    uint16_t per;

    if (sweep_up) {
        // Rising frequency, falling PER: per_low ~→ per_high
        per = per_low - (uint16_t)scaled;
    } else {
        // Falling frequency, rising PER: per_high ~→ per_low
        per = per_high + (uint16_t)scaled;
    }

    TCA0.SINGLE.CMP1 = per >> 1;   // 50 % duty (write before PER — both buffered)
    TCA0.SINGLE.PER  = per;

    sweep_ms++;
    if (sweep_ms >= SWEEP_TIME_MS) {
        sweep_ms = 0;
        sweep_up = !sweep_up;       // reverse direction
    }
}

/*
 * Silence the piezo — CMP1 = 0 means the output is held low for the entire
 * cycle (CNT is never < 0).  The output pin stays at its inactive level.
 */
static inline void piezo_silent(void) {
    TCA0.SINGLE.CMP1 = 0;
}

// ══════════════════════════════════════════════════════════════════════════════
//  Input helpers — each call is one "tick" (1 ms)
// ══════════════════════════════════════════════════════════════════════════════

/*
 * Generic debounce: returns the stable value of a digital pin.
 * `threshold` is in ticks (ms at the 1 kHz call rate).
 * The counter resets whenever the raw reading matches the stable value,
 * so noise shorter than `threshold` is rejected entirely.
 */
static bool debounce(uint8_t pin, uint8_t threshold,
                     uint8_t *ctr, bool *stable) {
    bool raw = digitalReadFast(pin);     // ~1 cycle vs. ~30 for digitalRead()

    if (raw == *stable) {
        *ctr = 0;
    } else {
        (*ctr)++;
        if (*ctr >= threshold) {
            *stable = raw;
            *ctr = 0;
        }
    }
    return *stable;
}

/*
 * Falling-edge detector — returns true once when the debounced mute button
 * transitions from HIGH (released) to LOW (pressed).
 */
static bool button_press_event(uint8_t pin, uint8_t threshold) {
    bool raw = digitalReadFast(pin);

    if (raw == mute_stable) {
        mute_db_ctr = 0;
    } else {
        mute_db_ctr++;
        if (mute_db_ctr >= threshold) {
            mute_stable = raw;
            mute_db_ctr = 0;
        }
    }

    bool press = (mute_prev_stable == true && mute_stable == false);
    mute_prev_stable = mute_stable;
    return press;
}

// ══════════════════════════════════════════════════════════════════════════════
//  Arduino entry points
// ══════════════════════════════════════════════════════════════════════════════

void setup() {

    // ── Fix RTC millis prescaler ────────────────────────────────────────────
    //  megaTinyCore ≤ 2.6.11 bug (SpenceKonde/megaTinyCore#1288): timers.h
    //  defines _RTC_PRESCALE_VALUE as 0x05 — the DIV32 field value unshifted —
    //  so init_millis() lands it in CTRLA bits 0/2 instead of the PRESCALER
    //  field (bits 6:3). The RTC then runs at DIV1 (32.768 kHz) and millis()
    //  counts 32× fast: every delay in this sketch shrinks 32×, and the 2 Hz
    //  LED flash becomes 62.5 Hz (looks solidly lit). Rewrite CTRLA with the
    //  properly shifted DIV32 group code. Remove once PR #1289 ships.
    while (RTC.STATUS & RTC_CTRLABUSY_bm);
    RTC.CTRLA = RTC_RUNSTDBY_bm | RTC_PRESCALER_DIV32_gc | RTC_RTCEN_bm;

    // ── GPIO ────────────────────────────────────────────────────────────────
    //  Safe state first: all outputs LOW before they become outputs.
    //  (Already LOW from reset, but explicit is cheap insurance.)
    digitalWrite(PIN_LED,   LOW);
    digitalWrite(PIN_RELAY, LOW);

    pinMode(PIN_FLOAT, INPUT);
    pinMode(PIN_LED,   OUTPUT);
    pinMode(PIN_RELAY, OUTPUT);
    pinMode(PIN_MUTE,  INPUT_PULLUP);

    // ── Brown‑out detector ──────────────────────────────────────────────────
    //  Fuse level 7 (~4.5 V) is set at programming time.
    //  Keep BOD active during sleep so the WDT reset is clean if the rail
    //  droops below the threshold.
    _PROTECTED_WRITE(BOD.CTRLA, BOD_SLEEP_ENABLED_gc | BOD_ACTIVE_ENABLED_gc);

    // ── PWM — must come before WDT enable (takes a few register writes) ─────
    piezo_pwm_init();

    // ── Watchdog ────────────────────────────────────────────────────────────
    //  1 s timeout, no window.  The 1 kHz loop calls wdt_reset() every ~1 ms
    //  so there is a 1000× margin.  1 s is chosen because it's long enough
    //  to survive any reasonable interrupt storm or millis() rollover, but
    //  short enough that a hung MCU won't leave the pump relay energised
    //  for more than a second.
    //
    //  WDT_PERIOD_1KCLK_gc = 1024 cycles of the 1.024 kHz ULP oscillator
    //                       ≈ 1.0 s (ATtiny202 datasheet Table 19-2).
    _PROTECTED_WRITE(WDT.CTRLA, WDT_PERIOD_1KCLK_gc | WDT_WINDOW_OFF_gc);
}

void loop() {

    // ── Rate‑limit to 1 kHz ─────────────────────────────────────────────────
    //  All logic (debounce, timing, sweep, flash) runs on a ~1 ms cadence.
    //  millis() on RTC ticks at exactly 1 kHz.
    static uint32_t last_ms = 0;
    uint32_t now = millis();
    if (now == last_ms) return;
    last_ms = now;

    // ── Pet the watchdog ────────────────────────────────────────────────────
    wdt_reset();

    // ── Read debounced inputs ───────────────────────────────────────────────

    bool float_high = debounce(PIN_FLOAT, DEBOUNCE_FLOAT_MS,
                               &float_db_ctr, &float_stable);

    if (button_press_event(PIN_MUTE, DEBOUNCE_MUTE_MS))
        mute = !mute;

    // ── LED flash: 2 Hz while float is high ─────────────────────────────────
    //  (millis() & 0x1FF) cycles 0…511 ticks (512 ms period).
    //  ON  for ticks 0…255  (256 ms)
    //  OFF for ticks 256…511 (256 ms)
    //  → f = 1000/512 ≈ 1.95 Hz.

    digitalWrite(PIN_LED, float_high && ((now & 0x1FF) < 256) ? HIGH : LOW);

    // ── Piezo alarm with frequency sweep and temporal modulation ────────────

    bool alarm_condition = float_high && (high_ms >= ALARM_DELAY_MS) && !mute;

    if (float_high) {
        if (high_ms < ALARM_DELAY_MS)
            high_ms++;
    } else {
        high_ms = 0;
    }

    if (alarm_condition) {
        // ── Temporal modulation state machine ───────────────────────────
        //  PHASE_ACTIVE:  update the frequency sweep each ms for ALARM_ON_MS,
        //                 then transition to silence.
        //  PHASE_SILENT:  output held at 0 for ALARM_OFF_MS, then transition
        //                 to active and reset the sweep.

        alarm_timer++;

        if (alarm_phase == PHASE_ACTIVE) {
            update_piezo_sweep();

            if (alarm_timer >= ALARM_ON_MS) {
                alarm_phase = PHASE_SILENT;
                alarm_timer = 0;
                piezo_silent();
            }
        } else /* PHASE_SILENT */ {
            if (alarm_timer >= ALARM_OFF_MS) {
                alarm_phase = PHASE_ACTIVE;
                alarm_timer = 0;
                sweep_ms = 0;
                sweep_up = true;          // start rising from F_LOW
            }
        }
    } else {
        // ── Alarm not sounding — reset everything ───────────────────────
        alarm_phase = PHASE_ACTIVE;       // next activation starts with warble
        alarm_timer = 0;
        sweep_ms    = 0;
        sweep_up    = true;
        piezo_silent();

        if (!float_high)
            mute = false;                 // alarm cleared → re-arm sounder
    }

    // ── Relay (pump) control ────────────────────────────────────────────────
    //  Energise after a stable 2 s float.  Minimum 5 s run after the float
    //  opens — slosh can't chatter the contactor.  Mute never gates this.

    if (float_high) {
        if (high_ms >= PUMP_DELAY_MS) {
            digitalWrite(PIN_RELAY, HIGH);
            relay_on_ms++;
        }
    } else {
        if (digitalReadFast(PIN_RELAY) == HIGH) {
            if (relay_on_ms >= PUMP_MIN_RUN_MS) {
                digitalWrite(PIN_RELAY, LOW);
                relay_on_ms = 0;
            } else {
                relay_on_ms++;
            }
        }
    }
}
