#include <stdio.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

/*
  Standalone NightOwl / ERB RP2040 firmware (2 lanes)

  - Switches wired C/NO to GND -> active LOW when triggered
  - Internal pull-ups enabled on all switch pins
  - Two steppers via STEP/DIR/EN (TMC2209 onboard)

  Features:
  - Non-blocking autoload: inserting filament in a lane feeds until that lane OUT is hit (or timeout),
    without pausing feeding on the active lane.
  - Buffer-driven feed: feeds only when buffer LOW persists for LOW_DELAY_S
  - Auto-swap: arm when active lane IN becomes empty (spool end). Execute swap when buffer needs feed
    and the other lane is "ready".
  - Optional: require Y-split to be clear before swap (recommended).
*/

// ---------------------------- CONFIG ----------------------------

// ---- Switch pins (active low, pull-up) ----
// Lane 1
#define PIN_L1_IN      24
#define PIN_L1_OUT     25
// Lane 2
#define PIN_L2_IN      22
#define PIN_L2_OUT     12

// Y splitter switch
#define PIN_Y_SPLIT    2

// Buffer switches
#define PIN_BUF_LOW    6
#define PIN_BUF_HIGH   7

// ---- Stepper pins (TMC2209 STEP/DIR/EN) ----
// Lane 1 motor
#define PIN_M1_EN      8
#define PIN_M1_DIR     9
#define PIN_M1_STEP    10
// Lane 2 motor
#define PIN_M2_EN      14
#define PIN_M2_DIR     15
#define PIN_M2_STEP    16

// Direction invert (set per your mechanics)
#define M1_DIR_INVERT  0
#define M2_DIR_INVERT  1

// Enable polarity (most boards: EN low = enabled)
#define EN_ACTIVE_LOW  1

// Feeding behavior
#define FEED_STEPS_PER_SEC      5000
#define STEP_PULSE_US           3
#define LOW_DELAY_S             0.50f
#define SWAP_COOLDOWN_S         0.50f

// Autoload behavior (non-blocking)
#define AUTOLOAD_STEPS_PER_SEC  2000
#define AUTOLOAD_TIMEOUT_S      7.0f

// Debounce
#define DEBOUNCE_MS             10

// Swap safety
#define REQUIRE_Y_CLEAR_FOR_SWAP 1

// Debug prints
#define DEBUG_PRINTS            1
#define DEBUG_PERIOD_US         500000

// -------------------------- END CONFIG --------------------------

// Simple debounced digital input (active-low switches)
typedef struct {
    uint pin;
    bool stable;
    absolute_time_t last_change;
} din_t;

static inline void din_init(din_t *d, uint pin) {
    d->pin = pin;
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
    d->stable = gpio_get(pin);
    d->last_change = get_absolute_time();
}

static inline void din_update(din_t *d) {
    bool raw = gpio_get(d->pin);
    if (raw != d->stable) {
        if (absolute_time_diff_us(d->last_change, get_absolute_time()) >
            (int64_t)DEBOUNCE_MS * 1000) {
            d->stable = raw;
            d->last_change = get_absolute_time();
        }
    } else {
        d->last_change = get_absolute_time();
    }
}

static inline bool filament_present(const din_t *d) {
    // active-low: 0 = triggered
    return d->stable == 0;
}

// Stepper (STEP/DIR/EN)
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

static inline bool time_after(absolute_time_t a, absolute_time_t b) {
    // true if a is after b (a > b)
    return absolute_time_diff_us(b, a) < 0;
}

int main() {
    stdio_init_all();
    sleep_ms(1500); // give USB-serial time

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

    // State
    int active_lane = 1;
    bool swap_armed = false;
    absolute_time_t low_since = get_absolute_time();
    absolute_time_t swap_cooldown_until = get_absolute_time();

    // Autoload (non-blocking)
    int autoload_lane = 0;                 // 0 none, 1 lane1, 2 lane2
    absolute_time_t autoload_until = {0};
    absolute_time_t last_autoload_step = {0};

    // Edge detect for insert
    bool prev_l1_in = false;
    bool prev_l2_in = false;

#if DEBUG_PRINTS
    absolute_time_t last_dbg = {0};
#endif

    while (true) {
        // --- Update all inputs ---
        din_update(&l1_in);  din_update(&l1_out);
        din_update(&l2_in);  din_update(&l2_out);
        din_update(&y_split);
        din_update(&buf_low); din_update(&buf_high);

        bool l1_in_present  = filament_present(&l1_in);
        bool l2_in_present  = filament_present(&l2_in);
        bool l1_out_present = filament_present(&l1_out);
        bool l2_out_present = filament_present(&l2_out);

        bool buffer_low  = filament_present(&buf_low);
        bool buffer_high = filament_present(&buf_high);

        bool y_filament_present = filament_present(&y_split);
        bool y_clear = !y_filament_present;

        // --- Buffer LOW timing -> need_feed ---
        if (!buffer_low) {
            low_since = get_absolute_time();
        }
        bool low_persist =
            (absolute_time_diff_us(low_since, get_absolute_time()) > (int64_t)(LOW_DELAY_S * 1000000));
        bool need_feed = buffer_low && low_persist;

        // --- Detect new filament insert -> start autoload (non-blocking) ---
        // Only start if that lane OUT is not already present.
        if (l1_in_present && !prev_l1_in && !l1_out_present) {
            autoload_lane = 1;
            autoload_until = delayed_by_ms(get_absolute_time(), (int32_t)(AUTOLOAD_TIMEOUT_S * 1000));
        }
        if (l2_in_present && !prev_l2_in && !l2_out_present) {
            autoload_lane = 2;
            autoload_until = delayed_by_ms(get_absolute_time(), (int32_t)(AUTOLOAD_TIMEOUT_S * 1000));
        }
        prev_l1_in = l1_in_present;
        prev_l2_in = l2_in_present;

        // Safety: don't autoload active lane (avoid double stepping)
        if (autoload_lane == active_lane) {
            autoload_lane = 0;
            stepper_enable(&m1, false);
            stepper_enable(&m2, false);
        }

        // --- Autoload runner (background) ---
        if (autoload_lane != 0) {
            bool timeout = time_after(get_absolute_time(), autoload_until);

            // refresh OUT signal for lane being autoloaded
            bool done = false;
            if (autoload_lane == 1) done = l1_out_present;
            if (autoload_lane == 2) done = l2_out_present;

            if (timeout || done) {
                if (autoload_lane == 1) stepper_enable(&m1, false);
                if (autoload_lane == 2) stepper_enable(&m2, false);
                autoload_lane = 0;
            } else {
                if (absolute_time_diff_us(last_autoload_step, get_absolute_time()) >
                    (int64_t)(1000000 / AUTOLOAD_STEPS_PER_SEC)) {

                    last_autoload_step = get_absolute_time();

                    if (autoload_lane == 1) {
                        stepper_enable(&m1, true);
                        stepper_set_dir(&m1, true);
                        stepper_pulse(&m1);
                    } else if (autoload_lane == 2) {
                        stepper_enable(&m2, true);
                        stepper_set_dir(&m2, true);
                        stepper_pulse(&m2);
                    }
                }
            }
        }

        // --- Arm swap when active lane IN is empty (spool end) ---
        if (active_lane == 1 && !l1_in_present) swap_armed = true;
        if (active_lane == 2 && !l2_in_present) swap_armed = true;

        // --- Lane ready condition for swapping ---
        // We consider the lane ready when its OUT sees filament.
        bool lane1_ready = l1_out_present;
        bool lane2_ready = l2_out_present;

        // --- Execute swap when buffer needs feed and other lane is ready ---
        bool allow_swap = need_feed && swap_armed;
#if REQUIRE_Y_CLEAR_FOR_SWAP
        allow_swap = allow_swap && y_clear;
#endif
        if (allow_swap) {
            if (active_lane == 1 && lane2_ready) {
                active_lane = 2;
                swap_armed = false;
                swap_cooldown_until = delayed_by_ms(get_absolute_time(), (int32_t)(SWAP_COOLDOWN_S * 1000));
            } else if (active_lane == 2 && lane1_ready) {
                active_lane = 1;
                swap_armed = false;
                swap_cooldown_until = delayed_by_ms(get_absolute_time(), (int32_t)(SWAP_COOLDOWN_S * 1000));
            }
        }

        // --- Debug prints ---
#if DEBUG_PRINTS
        if (absolute_time_diff_us(last_dbg, get_absolute_time()) > DEBUG_PERIOD_US) {
            last_dbg = get_absolute_time();
            printf("A=%d armed=%d need=%d auto=%d  l1in=%d l1out=%d  l2in=%d l2out=%d  y=%d yclr=%d  bufL=%d bufH=%d\n",
                   active_lane, swap_armed, need_feed, autoload_lane,
                   l1_in_present, l1_out_present,
                   l2_in_present, l2_out_present,
                   y_filament_present, y_clear,
                   buffer_low, buffer_high);
        }
#endif

        // --- Feed ---
        bool in_cooldown = absolute_time_diff_us(get_absolute_time(), swap_cooldown_until) > 0;

        if (need_feed && !in_cooldown) {
            if (active_lane == 1 && l1_out_present) {
                // If lane1 is active, keep it feeding regardless of autoload in other lane.
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
                sleep_ms(5);
            }
        } else {
            sleep_ms(5);
        }

        (void)buffer_high; // reserved for future hysteresis
    }

    return 0;
}
