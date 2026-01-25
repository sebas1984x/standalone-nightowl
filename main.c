#include <stdio.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

/*
  Standalone NightOwl / ERB RP2040 firmware (2 lanes)
  - Switches are C/NO -> active LOW when filament present (assuming switch to GND)
  - Uses pull-ups on all switches
  - Feeds only when buffer is LOW (after LOW_DELAY_S)
  - Auto-load: when filament is inserted in a lane, prefeed until lane OUT sees filament
  - Auto-swap: when active lane OUT no longer sees filament while buffer requests feed, swap to other lane

  You MUST set GPIO pins correctly below.
*/

// ---------------------------- CONFIG ----------------------------

// ---- Switch pins (active low, pull-up) ----
// Lane 1
#define PIN_L1_IN      24   // CHANGE ME
#define PIN_L1_OUT     25   // CHANGE ME
// Lane 2
#define PIN_L2_IN      22   // CHANGE ME
#define PIN_L2_OUT     23   // per your note: lane 2 out on gpio23

// Y splitter switch (optional but useful)
#define PIN_Y_SPLIT    21   // per your note: Y split on gpio21

// Buffer switches (Wisepro / TurtleNeck style)
#define PIN_BUF_LOW    5   // CHANGE ME
#define PIN_BUF_HIGH   6   // CHANGE ME (optional, used for hysteresis)

// ---- Stepper pins (TMC2209 in STEP/DIR/EN mode, onboard drivers) ----
// Lane 1 motor
#define PIN_M1_EN      8   // CHANGE ME
#define PIN_M1_DIR     9   // CHANGE ME
#define PIN_M1_STEP    10   // CHANGE ME
// Lane 2 motor
#define PIN_M2_EN      14    // CHANGE ME
#define PIN_M2_DIR     15    // CHANGE ME
#define PIN_M2_STEP    16   // CHANGE ME

// Direction invert (fix “lane 2 draait verkeerd om” here)
#define M1_DIR_INVERT  0
#define M2_DIR_INVERT  0    // <-- set to 1 to flip lane 2 direction (your issue)

// Enable polarity (most boards: EN low = enabled, but check your ERB schematic)
// If your motors never move, flip this.
#define EN_ACTIVE_LOW  1

// Feeding behavior
#define FEED_STEPS_PER_SEC   2500     // tune: how hard we push when buffer low
#define STEP_PULSE_US        3        // STEP pulse width
#define LOW_DELAY_S          0.75f    // buffer low must persist this long before feeding
#define SWAP_COOLDOWN_S      0.50f    // wait after swap before feeding again

// Auto-load behavior
#define AUTOLOAD_STEPS_PER_SEC 1200
#define AUTOLOAD_TIMEOUT_S     6.0f   // stop trying if OUT never triggers (prevents infinite run)

// Debounce
#define DEBOUNCE_MS          10

// Status LED (on Pico is GPIO25, but ERB may not have a LED; safe to ignore)
#define PIN_STATUS_LED       25
#define USE_STATUS_LED       0

// -------------------------- END CONFIG --------------------------

// Simple debounced digital input
typedef struct {
    uint pin;
    bool stable;             // stable raw reading
    absolute_time_t last_change;
} din_t;

static inline void din_init(din_t *d, uint pin) {
    d->pin = pin;
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);       // active-low switches
    d->stable = gpio_get(pin);
    d->last_change = get_absolute_time();
}

static inline void din_update(din_t *d) {
    bool raw = gpio_get(d->pin);
    if (raw != d->stable) {
        // only accept change if it stays for DEBOUNCE_MS
        if (absolute_time_diff_us(d->last_change, get_absolute_time()) > (int64_t)DEBOUNCE_MS * 1000) {
            d->stable = raw;
            d->last_change = get_absolute_time();
        }
    } else {
        d->last_change = get_absolute_time();
    }
}

// Active-low filament present
static inline bool filament_present(const din_t *d) {
    return d->stable == 0;   // C/NO -> closed to GND when filament present
}

typedef struct {
    uint en, dir, step;
    bool dir_invert;
} stepper_t;

static inline void stepper_init(stepper_t *m, uint en, uint dir, uint step, bool dir_invert) {
    m->en = en; m->dir = dir; m->step = step; m->dir_invert = dir_invert;

    gpio_init(m->en);
    gpio_init(m->dir);
    gpio_init(m->step);

    gpio_set_dir(m->en, GPIO_OUT);
    gpio_set_dir(m->dir, GPIO_OUT);
    gpio_set_dir(m->step, GPIO_OUT);

    // disable by default
    if (EN_ACTIVE_LOW) gpio_put(m->en, 1);
    else               gpio_put(m->en, 0);

    gpio_put(m->step, 0);
    gpio_put(m->dir, 0);
}

static inline void stepper_enable(stepper_t *m, bool on) {
    if (EN_ACTIVE_LOW) gpio_put(m->en, on ? 0 : 1);
    else               gpio_put(m->en, on ? 1 : 0);
}

static inline void stepper_set_dir(stepper_t *m, bool forward) {
    bool d = forward ^ m->dir_invert;
    gpio_put(m->dir, d ? 1 : 0);
}

static inline void stepper_pulse(stepper_t *m) {
    gpio_put(m->step, 1);
    sleep_us(STEP_PULSE_US);
    gpio_put(m->step, 0);
}

// Run motor at rate for duration or until stop condition
static void run_steps_until(stepper_t *m, int steps_per_sec, float timeout_s, bool (*stop_fn)(void*), void *ctx) {
    const int delay_us = (steps_per_sec <= 0) ? 0 : (1000000 / steps_per_sec);
    absolute_time_t start = get_absolute_time();
    while (true) {
        if (stop_fn && stop_fn(ctx)) break;
        if (timeout_s > 0 && absolute_time_diff_us(start, get_absolute_time()) > (int64_t)(timeout_s * 1000000)) break;
        stepper_pulse(m);
        sleep_us(delay_us);
    }
}

// Context structs for stop functions
typedef struct { din_t *out; } stop_out_ctx_t;
static bool stop_when_out_present(void *p) {
    stop_out_ctx_t *c = (stop_out_ctx_t*)p;
    din_update(c->out);
    return filament_present(c->out);
}

int main() {
    stdio_init_all();

#if USE_STATUS_LED
    gpio_init(PIN_STATUS_LED);
    gpio_set_dir(PIN_STATUS_LED, GPIO_OUT);
    gpio_put(PIN_STATUS_LED, 0);
#endif

    // Inputs
    din_t l1_in, l1_out, l2_in, l2_out, y_split, buf_low, buf_high;
    din_init(&l1_in, PIN_L1_IN);
    din_init(&l1_out, PIN_L1_OUT);
    din_init(&l2_in, PIN_L2_IN);
    din_init(&l2_out, PIN_L2_OUT);
    din_init(&y_split, PIN_Y_SPLIT);
    din_init(&buf_low, PIN_BUF_LOW);
    din_init(&buf_high, PIN_BUF_HIGH);

    // Motors
    stepper_t m1, m2;
    stepper_init(&m1, PIN_M1_EN, PIN_M1_DIR, PIN_M1_STEP, M1_DIR_INVERT);
    stepper_init(&m2, PIN_M2_EN, PIN_M2_DIR, PIN_M2_STEP, M2_DIR_INVERT);

    int active_lane = 1;
    absolute_time_t low_since = get_absolute_time();
    absolute_time_t swap_cooldown_until = get_absolute_time();

    bool prev_l1_in = false;
    bool prev_l2_in = false;

    while (true) {
        // update all inputs
        din_update(&l1_in); din_update(&l1_out);
        din_update(&l2_in); din_update(&l2_out);
        din_update(&y_split);
        din_update(&buf_low); din_update(&buf_high);

        bool l1_in_present = filament_present(&l1_in);
        bool l2_in_present = filament_present(&l2_in);
        bool l1_out_present = filament_present(&l1_out);
        bool l2_out_present = filament_present(&l2_out);

        bool buffer_low = filament_present(&buf_low);   // active low switch: present==low asserted
        bool buffer_high = filament_present(&buf_high);

        // --- Auto-load: when filament is inserted, push until OUT sees filament ---
        if (l1_in_present && !prev_l1_in && !l1_out_present) {
            // new filament inserted in lane 1
            stepper_enable(&m1, true);
            stepper_set_dir(&m1, true);
            stop_out_ctx_t ctx = { .out = &l1_out };
            run_steps_until(&m1, AUTOLOAD_STEPS_PER_SEC, AUTOLOAD_TIMEOUT_S, stop_when_out_present, &ctx);
            stepper_enable(&m1, false);
        }
        if (l2_in_present && !prev_l2_in && !l2_out_present) {
            // new filament inserted in lane 2
            stepper_enable(&m2, true);
            stepper_set_dir(&m2, true);
            stop_out_ctx_t ctx = { .out = &l2_out };
            run_steps_until(&m2, AUTOLOAD_STEPS_PER_SEC, AUTOLOAD_TIMEOUT_S, stop_when_out_present, &ctx);
            stepper_enable(&m2, false);
        }

        prev_l1_in = l1_in_present;
        prev_l2_in = l2_in_present;

        // --- Buffer low timing ---
        if (!buffer_low) {
            low_since = get_absolute_time();
        }

        bool low_persist = (absolute_time_diff_us(low_since, get_absolute_time()) >
                            (int64_t)(LOW_DELAY_S * 1000000));

        // --- Decide if we need to feed ---
        bool need_feed = buffer_low && low_persist;

        // --- Swap if active lane appears empty when we need feed ---
        // (We swap based on OUT sensor: if OUT no longer sees filament, lane is empty upstream)
        if (need_feed) {
            if (active_lane == 1 && !l1_out_present && l2_out_present) {
                active_lane = 2;
                swap_cooldown_until = delayed_by_ms(get_absolute_time(), (int32_t)(SWAP_COOLDOWN_S * 1000));
            } else if (active_lane == 2 && !l2_out_present && l1_out_present) {
                active_lane = 1;
                swap_cooldown_until = delayed_by_ms(get_absolute_time(), (int32_t)(SWAP_COOLDOWN_S * 1000));
            }
        }

        // --- Feed ---
        bool in_cooldown = absolute_time_diff_us(get_absolute_time(), swap_cooldown_until) > 0;

        if (need_feed && !in_cooldown) {
            if (active_lane == 1 && l1_out_present) {
                stepper_enable(&m1, true);
                stepper_set_dir(&m1, true);
                stepper_pulse(&m1);
                sleep_us(1000000 / FEED_STEPS_PER_SEC);
                stepper_enable(&m1, false);
            } else if (active_lane == 2 && l2_out_present) {
                stepper_enable(&m2, true);
                stepper_set_dir(&m2, true);
                stepper_pulse(&m2);
                sleep_us(1000000 / FEED_STEPS_PER_SEC);
                stepper_enable(&m2, false);
            } else {
                // active lane can't feed (no filament at OUT). do nothing.
                sleep_ms(5);
            }
        } else {
            sleep_ms(5);
        }

#if USE_STATUS_LED
        // blink LED if buffer low (very basic status)
        gpio_put(PIN_STATUS_LED, buffer_low ? 1 : 0);
#endif
    }
}

