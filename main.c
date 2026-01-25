#include <stdio.h>
#include <stdbool.h>
#include "pico/stdlib.h"

/* =========================================================
   TUNING (aanpassen = rebuild + reflash)
   ========================================================= */
static const uint32_t FEED_STEPS_PER_SEC      = 2200;  // actieve lane bijvullen
static const uint32_t PRELOAD_STEPS_PER_SEC   = 1200;  // inactieve lane preload
static const float    LOW_DELAY_S             = 2.0f;  // buffer_low moet zo lang aanhouden
static const uint32_t SWAP_COOLDOWN_MS        = 800;   // anti-pingpong
static const uint32_t PRELOAD_TIMEOUT_MS      = 15000; // max preload tijd
static const uint32_t DEBOUNCE_MS             = 25;    // debounce switches

/* =========================================================
   PINMAP (jouw setup)
   Alle switches: C -> GND, NO -> GPIO, interne pull-up aan
   ========================================================= */
// ERB v2 onboard TMC2209 pin labels uit je diagram
#define LANE1_EN    8
#define LANE1_DIR   9
#define LANE1_STEP 10

#define LANE2_EN   14
#define LANE2_DIR  15
#define LANE2_STEP 16

// Sensors
#define PIN_L1_IN    24
#define PIN_L1_OUT   25
#define PIN_L2_IN    22
#define PIN_L2_OUT   23
#define PIN_Y_SPLIT  21
#define PIN_BUF_LOW   6
#define PIN_BUF_HIGH  7

/* =========================================================
   Helpers: Debounce (active-low switches)
   ========================================================= */
typedef struct {
    uint pin;
    bool stable;
    bool last;
    absolute_time_t changed;
} debounced_t;

static void debounce_init(debounced_t *d, uint pin) {
    d->pin = pin;
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);           // C/NO -> GND, dus idle HIGH
    d->stable = gpio_get(pin);
    d->last = d->stable;
    d->changed = get_absolute_time();
}

static void debounce_update(debounced_t *d) {
    bool v = gpio_get(d->pin);
    if (v != d->last) {
        d->last = v;
        d->changed = get_absolute_time();
        return;
    }
    int64_t ms = absolute_time_diff_us(d->changed, get_absolute_time()) / 1000;
    if (ms >= (int64_t)DEBOUNCE_MS) {
        d->stable = v;
    }
}

// “filament present” = switch gesloten naar GND = LOW
static inline bool filament_present(const debounced_t *d) {
    return d->stable == false;
}

/* =========================================================
   Stepper (zeer simpel: STEP pulsen op vaste rate)
   ========================================================= */
typedef struct {
    uint en, dir, step;
    bool enabled;
    bool dir_invert;
    uint32_t sps;                 // steps per second
    absolute_time_t next_step;
} stepper_t;

static void stepper_init(stepper_t *s, uint en, uint dir, uint step, bool invert_dir) {
    s->en = en; s->dir = dir; s->step = step;
    s->enabled = false;
    s->dir_invert = invert_dir;
    s->sps = 0;
    s->next_step = get_absolute_time();

    gpio_init(en);   gpio_set_dir(en, GPIO_OUT);
    gpio_init(dir);  gpio_set_dir(dir, GPIO_OUT);
    gpio_init(step); gpio_set_dir(step, GPIO_OUT);

    gpio_put(step, 0);
    gpio_put(dir, 0);
    gpio_put(en, 1); // disable (TMC EN is active-low)
}

static void stepper_enable(stepper_t *s, bool on) {
    s->enabled = on;
    gpio_put(s->en, on ? 0 : 1);
}

static void stepper_set_dir(stepper_t *s, bool forward) {
    bool v = forward ^ s->dir_invert;
    gpio_put(s->dir, v ? 1 : 0);
}

static inline void stepper_pulse(stepper_t *s) {
    gpio_put(s->step, 1);
    sleep_us(2);
    gpio_put(s->step, 0);
}

static void stepper_run(stepper_t *s) {
    if (!s->enabled || s->sps == 0) return;

    if (absolute_time_diff_us(get_absolute_time(), s->next_step) <= 0) {
        stepper_pulse(s);
        s->next_step = delayed_by_us(get_absolute_time(), 1000000 / s->sps);
    }
}

/* =========================================================
   Logic
   ========================================================= */
typedef enum { LANE_1 = 1, LANE_2 = 2 } lane_t;

static inline lane_t other_lane(lane_t l) {
    return (l == LANE_1) ? LANE_2 : LANE_1;
}

int main() {
    stdio_init_all();
    sleep_ms(200);

    // Set dir forward for feeding; invert if your motor runs backward
    stepper_t m1, m2;
    stepper_init(&m1, LANE1_EN, LANE1_DIR, LANE1_STEP, false);
    stepper_init(&m2, LANE2_EN, LANE2_DIR, LANE2_STEP, false);
    stepper_set_dir(&m1, true);
    stepper_set_dir(&m2, true);

    // Debounced sensors
    debounced_t l1_in, l1_out, l2_in, l2_out, y_sw, buf_low, buf_high;
    debounce_init(&l1_in,  PIN_L1_IN);
    debounce_init(&l1_out, PIN_L1_OUT);
    debounce_init(&l2_in,  PIN_L2_IN);
    debounce_init(&l2_out, PIN_L2_OUT);
    debounce_init(&y_sw,   PIN_Y_SPLIT);
    debounce_init(&buf_low,  PIN_BUF_LOW);
    debounce_init(&buf_high, PIN_BUF_HIGH);

    lane_t active = LANE_1;

    bool swap_armed = false;
    absolute_time_t low_since = get_absolute_time();
    absolute_time_t last_swap = get_absolute_time();

    // Preload state per lane
    bool preload1 = false, preload2 = false;
    absolute_time_t preload1_start = get_absolute_time();
    absolute_time_t preload2_start = get_absolute_time();

    while (true) {
        // Update sensors
        debounce_update(&l1_in);
        debounce_update(&l1_out);
        debounce_update(&l2_in);
        debounce_update(&l2_out);
        debounce_update(&y_sw);
        debounce_update(&buf_low);
        debounce_update(&buf_high);

        // Interpret
        bool f_l1_in   = filament_present(&l1_in);
        bool f_l1_out  = filament_present(&l1_out);
        bool f_l2_in   = filament_present(&l2_in);
        bool f_l2_out  = filament_present(&l2_out);

        bool y_clear   = !filament_present(&y_sw);     // clear when switch is OPEN (no filament)
        bool buf_is_low  = !filament_present(&buf_low);  // depends on your buffer wiring logic
        bool buf_is_high = filament_present(&buf_high);

        // === Buffer demand ===
        bool need_feed = false;
        if (buf_is_low) {
            float s = (float)(absolute_time_diff_us(low_since, get_absolute_time())) / 1000000.0f;
            if (s > LOW_DELAY_S) need_feed = true;
        } else {
            low_since = get_absolute_time();
        }

        // === Arm swap if active lane is empty at IN ===
        if (active == LANE_1 && !f_l1_in) swap_armed = true;
        if (active == LANE_2 && !f_l2_in) swap_armed = true;

        // === Preload logic (inactieve lane) ===
        // Start preload when filament is inserted (IN becomes true), stop when OUT becomes true.
        // Only preload the inactive lane, so you can reload a lane while the other prints.
        if (active == LANE_2) {
            // lane1 inactive
            if (!preload1 && f_l1_in && !f_l1_out) {
                preload1 = true;
                preload1_start = get_absolute_time();
            }
        } else {
            // lane2 inactive
            if (!preload2 && f_l2_in && !f_l2_out) {
                preload2 = true;
                preload2_start = get_absolute_time();
            }
        }

        // Stop preload when OUT reached or timeout
        if (preload1) {
            if (f_l1_out) preload1 = false;
            int64_t ms = absolute_time_diff_us(preload1_start, get_absolute_time()) / 1000;
            if (ms > (int64_t)PRELOAD_TIMEOUT_MS) preload1 = false;
        }
        if (preload2) {
            if (f_l2_out) preload2 = false;
            int64_t ms = absolute_time_diff_us(preload2_start, get_absolute_time()) / 1000;
            if (ms > (int64_t)PRELOAD_TIMEOUT_MS) preload2 = false;
        }

        // === Swap decision ===
        if (swap_armed && need_feed && y_clear) {
            int64_t ms = absolute_time_diff_us(last_swap, get_absolute_time()) / 1000;
            if (ms > (int64_t)SWAP_COOLDOWN_MS) {
                lane_t candidate = other_lane(active);
                bool candidate_has_filament = (candidate == LANE_1) ? f_l1_in : f_l2_in;

                // Only swap if the other lane actually has filament present
                if (candidate_has_filament) {
                    active = candidate;
                    last_swap = get_absolute_time();
                    swap_armed = false;
                }
            }
        }

        // === Motor control ===
        // Enable only the lanes we might move: active for feed, inactive for preload
        bool run1 = false, run2 = false;

        // Active lane feeds buffer
        if (need_feed) {
            if (active == LANE_1 && f_l1_in) run1 = true;
            if (active == LANE_2 && f_l2_in) run2 = true;
        }

        // Inactive lane preloads
        if (preload1) run1 = true;
        if (preload2) run2 = true;

        // Apply enables + speeds
        stepper_enable(&m1, run1);
        stepper_enable(&m2, run2);

        // Speed selection: feed > preload
        m1.sps = 0;
        m2.sps = 0;

        if (run1) {
            m1.sps = (preload1 && !(need_feed && active == LANE_1)) ? PRELOAD_STEPS_PER_SEC : FEED_STEPS_PER_SEC;
        }
        if (run2) {
            m2.sps = (preload2 && !(need_feed && active == LANE_2)) ? PRELOAD_STEPS_PER_SEC : FEED_STEPS_PER_SEC;
        }

        // Optional: if buffer is high, don't keep feeding (but preload may still run)
        if (buf_is_high) {
            if (active == LANE_1) {
                // allow preload only
                if (!preload1) m1.sps = 0;
            } else {
                if (!preload2) m2.sps = 0;
            }
        }

        // Run steppers
        stepper_run(&m1);
        stepper_run(&m2);

        sleep_ms(1);
    }

    return 0;
}
