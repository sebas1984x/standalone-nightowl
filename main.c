#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/adc.h"

/*
  Standalone NightOwl / ERB RP2040 firmware (2 lanes)
  - Buffer-driven feed + autoswap + autoload (non-blocking)
  - Manual reverse per lane (2 buttons)
  - Potmeter controls FEED rate (steps/sec)

  All switches/buttons wired C/NO to GND -> active LOW with pull-ups.
*/

// ---------------------------- CONFIG ----------------------------

// Switch pins (active low, pull-up)
#define PIN_L1_IN      24
#define PIN_L1_OUT     25
#define PIN_L2_IN      22
#define PIN_L2_OUT     12
#define PIN_Y_SPLIT    2
#define PIN_BUF_LOW    6
#define PIN_BUF_HIGH   7

// Manual REV buttons (active low, pull-up)
#define PIN_BTN_REV_L1  28
#define PIN_BTN_REV_L2  29

// Speed pot (ADC) -> FEED rate
#define USE_FEED_POT        1
#define PIN_POT_ADC_GPIO    26      // GPIO26 = ADC0
#define POT_ADC_CHANNEL     0
#define POT_READ_PERIOD_MS  50

// Feed rate range from pot (steps/sec)
#define FEED_SPS_MIN        1000
#define FEED_SPS_MAX        9000

// Manual reverse speed (fixed)
#define REV_STEPS_PER_SEC   4000

// Steppers
#define PIN_M1_EN      8
#define PIN_M1_DIR     9
#define PIN_M1_STEP    10
#define PIN_M2_EN      14
#define PIN_M2_DIR     15
#define PIN_M2_STEP    16

#define M1_DIR_INVERT  0
#define M2_DIR_INVERT  1
#define EN_ACTIVE_LOW  1

// Autoload speed (fixed)
#define AUTOLOAD_STEPS_PER_SEC  5000

// Timing
#define STEP_PULSE_US           3
#define LOW_DELAY_S             0.40f
#define SWAP_COOLDOWN_S         0.50f
#define AUTOLOAD_TIMEOUT_S      6.0f
#define DEBOUNCE_MS             10

#define REQUIRE_Y_CLEAR_FOR_SWAP  1

// Debug
#define DEBUG_PRINTS      1
#define DEBUG_PERIOD_US   500000

// Status LED (GPIO)
#define STATUS_LED_MODE   1
#define PIN_STATUS_LED    17
#define STATUS_LED_ACTIVE_HIGH 1

// -------------------------- END CONFIG --------------------------

static inline bool time_reached(absolute_time_t t) {
    return absolute_time_diff_us(get_absolute_time(), t) <= 0;
}

static inline int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// ------------------------ Debounced input -----------------------

typedef struct {
    uint pin;
    bool stable;
    bool last_raw;
    absolute_time_t last_edge;
} din_t;

static inline void din_init(din_t *d, uint pin) {
    d->pin = pin;
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);

    bool raw = gpio_get(pin);
    d->stable = raw;
    d->last_raw = raw;
    d->last_edge = get_absolute_time();
}

static inline void din_update(din_t *d) {
    absolute_time_t now = get_absolute_time();
    bool raw = gpio_get(d->pin);

    if (raw != d->last_raw) {
        d->last_raw = raw;
        d->last_edge = now;
    }

    if (raw != d->stable) {
        if (absolute_time_diff_us(d->last_edge, now) >= (int64_t)DEBOUNCE_MS * 1000) {
            d->stable = raw;
        }
    }
}

static inline bool active_low_on(const din_t *d) {
    return d->stable == 0;
}

// ---------------------------- Stepper ---------------------------

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

// ---------------------- Status LED layer ------------------------

typedef enum {
    LED_IDLE = 0,
    LED_FEEDING,
    LED_AUTOLOAD,
    LED_SWAP_ARMED,
    LED_MANUAL_REV,
    LED_ERROR
} led_state_t;

#if STATUS_LED_MODE == 1
static inline void status_led_init(void) {
    gpio_init(PIN_STATUS_LED);
    gpio_set_dir(PIN_STATUS_LED, GPIO_OUT);
    gpio_put(PIN_STATUS_LED, STATUS_LED_ACTIVE_HIGH ? 0 : 1);
}

static inline void status_led_put(bool on) {
    gpio_put(PIN_STATUS_LED, STATUS_LED_ACTIVE_HIGH ? (on ? 1 : 0) : (on ? 0 : 1));
}

static void status_led_update(led_state_t st, int64_t t_us) {
    bool on = false;
    switch (st) {
        case LED_IDLE: {
            int64_t phase = t_us % 1000000;
            on = (phase < 60000);
        } break;
        case LED_FEEDING:
            on = true;
            break;
        case LED_AUTOLOAD: {
            int64_t phase = t_us % 200000;
            on = (phase < 100000);
        } break;
        case LED_SWAP_ARMED: {
            int64_t phase = t_us % 1000000;
            on = (phase < 250000);
        } break;
        case LED_MANUAL_REV: {
            int64_t phase = t_us % 120000;
            on = (phase < 60000);
        } break;
        case LED_ERROR: {
            int64_t phase = t_us % 1200000;
            on = (phase < 80000) || (phase >= 160000 && phase < 240000);
        } break;
    }
    status_led_put(on);
}
#else
static inline void status_led_init(void) {}
static inline void status_led_update(led_state_t st, int64_t t_us) { (void)st; (void)t_us; }
#endif

// ---------------------------- Lane ------------------------------

typedef enum {
    TASK_IDLE = 0,
    TASK_AUTOLOAD,
    TASK_FEED,
    TASK_MANUAL
} task_mode_t;

typedef struct {
    din_t in_sw;
    din_t out_sw;
    stepper_t m;

    bool prev_in_present;

    task_mode_t mode;
    absolute_time_t next_step;
    absolute_time_t autoload_deadline;

    int steps_per_sec;
    bool forward;
} lane_t;

static inline bool lane_in_present(lane_t *L)  { return active_low_on(&L->in_sw); }
static inline bool lane_out_present(lane_t *L) { return active_low_on(&L->out_sw); }

static void lane_init(lane_t *L,
                      uint pin_in, uint pin_out,
                      uint pin_en, uint pin_dir, uint pin_step,
                      bool dir_invert) {
    din_init(&L->in_sw, pin_in);
    din_init(&L->out_sw, pin_out);
    stepper_init(&L->m, pin_en, pin_dir, pin_step, dir_invert);

    L->prev_in_present = false;
    L->mode = TASK_IDLE;
    L->next_step = get_absolute_time();
    L->autoload_deadline = get_absolute_time();
    L->steps_per_sec = 0;
    L->forward = true;
}

static inline int32_t step_interval_us(int sps) {
    if (sps <= 0) return 1000000;
    int32_t base = (int32_t)(1000000 / sps);
    int32_t adj = base - (int32_t)STEP_PULSE_US;
    if (adj < 10) adj = 10;
    return adj;
}

static inline void lane_start_task(lane_t *L, task_mode_t mode, int sps, bool forward, float timeout_s) {
    L->mode = mode;
    L->steps_per_sec = sps;
    L->forward = forward;

    stepper_enable(&L->m, true);
    stepper_set_dir(&L->m, forward);
    L->next_step = get_absolute_time();

    if (mode == TASK_AUTOLOAD && timeout_s > 0) {
        L->autoload_deadline = delayed_by_us(get_absolute_time(), (int64_t)(timeout_s * 1000000));
    }
}

static inline void lane_stop_task(lane_t *L) {
    L->mode = TASK_IDLE;
    stepper_enable(&L->m, false);
}

static void lane_update_inputs(lane_t *L) {
    din_update(&L->in_sw);
    din_update(&L->out_sw);
}

static void lane_process(lane_t *L) {
    if (L->mode == TASK_AUTOLOAD) {
        if (lane_out_present(L) || time_reached(L->autoload_deadline)) {
            lane_stop_task(L);
            return;
        }
    }

    if (L->mode != TASK_IDLE && time_reached(L->next_step)) {
        stepper_pulse(&L->m);
        int32_t interval = step_interval_us(L->steps_per_sec);
        L->next_step = delayed_by_us(get_absolute_time(), interval);
    }
}

// ------------------------ FEED pot (ADC) ------------------------

#if USE_FEED_POT
static void feed_pot_init(void) {
    adc_init();
    adc_gpio_init(PIN_POT_ADC_GPIO);
    adc_select_input(POT_ADC_CHANNEL);
}

static int feed_pot_read_sps(void) {
    uint16_t raw = adc_read(); // 0..4095
    int span = (FEED_SPS_MAX - FEED_SPS_MIN);
    int sps = FEED_SPS_MIN + (int)((raw * (uint32_t)span) / 4095u);
    return clamp_i(sps, FEED_SPS_MIN, FEED_SPS_MAX);
}
#endif

// ---------------------------- MAIN -----------------------------

#if DEBUG_PRINTS
  #define DBG_PRINTF(...) printf(__VA_ARGS__)
#else
  #define DBG_PRINTF(...) do{}while(0)
#endif

int main() {
    stdio_init_all();
    sleep_ms(1500);

    status_led_init();

#if USE_FEED_POT
    feed_pot_init();
#endif

    // Inputs
    din_t y_split, buf_low, buf_high;
    din_init(&y_split, PIN_Y_SPLIT);
    din_init(&buf_low, PIN_BUF_LOW);
    din_init(&buf_high, PIN_BUF_HIGH);

    // Manual buttons
    din_t btn_rev_l1, btn_rev_l2;
    din_init(&btn_rev_l1, PIN_BTN_REV_L1);
    din_init(&btn_rev_l2, PIN_BTN_REV_L2);

    // Lanes
    lane_t L1, L2;
    lane_init(&L1, PIN_L1_IN, PIN_L1_OUT, PIN_M1_EN, PIN_M1_DIR, PIN_M1_STEP, M1_DIR_INVERT);
    lane_init(&L2, PIN_L2_IN, PIN_L2_OUT, PIN_M2_EN, PIN_M2_DIR, PIN_M2_STEP, M2_DIR_INVERT);

    int active_lane = 1;
    bool swap_armed = false;
    absolute_time_t swap_cooldown_until = get_absolute_time();
    absolute_time_t low_since = get_absolute_time();

#if DEBUG_PRINTS
    absolute_time_t last_dbg = {0};
#endif

    // Pot throttling + live feed rate
    absolute_time_t next_pot_read = get_absolute_time();
    int feed_sps = 5000;

    while (true) {
        absolute_time_t now = get_absolute_time();
        int64_t t_us = to_us_since_boot(now);

        // Update inputs
        lane_update_inputs(&L1);
        lane_update_inputs(&L2);
        din_update(&y_split);
        din_update(&buf_low);
        din_update(&buf_high);
        din_update(&btn_rev_l1);
        din_update(&btn_rev_l2);

        bool l1_in_present  = lane_in_present(&L1);
        bool l2_in_present  = lane_in_present(&L2);
        bool l1_out_present = lane_out_present(&L1);
        bool l2_out_present = lane_out_present(&L2);

        bool buffer_low  = active_low_on(&buf_low);
        bool buffer_high = active_low_on(&buf_high);

        bool y_present = active_low_on(&y_split);
        bool y_clear = !y_present;

        bool rev_l1 = active_low_on(&btn_rev_l1);
        bool rev_l2 = active_low_on(&btn_rev_l2);
        bool any_manual = rev_l1 || rev_l2;

#if USE_FEED_POT
        if (time_reached(next_pot_read)) {
            next_pot_read = delayed_by_ms(now, POT_READ_PERIOD_MS);
            feed_sps = feed_pot_read_sps();
        }
#endif

        // ---------- Manual reverse per lane (fixed speed) ----------
        if (rev_l1) {
            if (L1.mode != TASK_MANUAL || L1.forward != false || L1.steps_per_sec != REV_STEPS_PER_SEC) {
                lane_start_task(&L1, TASK_MANUAL, REV_STEPS_PER_SEC, false, 0.0f);
            }
        } else if (L1.mode == TASK_MANUAL) {
            lane_stop_task(&L1);
        }

        if (rev_l2) {
            if (L2.mode != TASK_MANUAL || L2.forward != false || L2.steps_per_sec != REV_STEPS_PER_SEC) {
                lane_start_task(&L2, TASK_MANUAL, REV_STEPS_PER_SEC, false, 0.0f);
            }
        } else if (L2.mode == TASK_MANUAL) {
            lane_stop_task(&L2);
        }

        // ---------- Normal behavior (only if no manual) ----------
        if (!any_manual) {
            // Autoload on IN rising edge
            if (l1_in_present && !L1.prev_in_present && !l1_out_present && L1.mode == TASK_IDLE) {
                lane_start_task(&L1, TASK_AUTOLOAD, AUTOLOAD_STEPS_PER_SEC, true, AUTOLOAD_TIMEOUT_S);
            }
            if (l2_in_present && !L2.prev_in_present && !l2_out_present && L2.mode == TASK_IDLE) {
                lane_start_task(&L2, TASK_AUTOLOAD, AUTOLOAD_STEPS_PER_SEC, true, AUTOLOAD_TIMEOUT_S);
            }

            // Buffer hysteresis: need_feed when LOW persists and HIGH not active
            if (!buffer_low) low_since = now;
            bool low_persist = absolute_time_diff_us(low_since, now) > (int64_t)(LOW_DELAY_S * 1000000);
            bool need_feed = buffer_low && low_persist && !buffer_high;

            // Arm swap when active lane IN empty
            if (active_lane == 1 && !l1_in_present) swap_armed = true;
            if (active_lane == 2 && !l2_in_present) swap_armed = true;

            bool in_cooldown = !time_reached(swap_cooldown_until);

            // Execute swap
            bool allow_swap = need_feed && swap_armed;
#if REQUIRE_Y_CLEAR_FOR_SWAP
            allow_swap = allow_swap && y_clear;
#endif
            if (!in_cooldown && allow_swap) {
                if (active_lane == 1 && l2_out_present) {
                    active_lane = 2;
                    swap_armed = false;
                    swap_cooldown_until = delayed_by_ms(now, (int32_t)(SWAP_COOLDOWN_S * 1000));
                } else if (active_lane == 2 && l1_out_present) {
                    active_lane = 1;
                    swap_armed = false;
                    swap_cooldown_until = delayed_by_ms(now, (int32_t)(SWAP_COOLDOWN_S * 1000));
                }
            }

            // Feed management (pot controls feed_sps)
            lane_t *A = (active_lane == 1) ? &L1 : &L2;
            bool A_out_ok = (active_lane == 1) ? l1_out_present : l2_out_present;

            if (!in_cooldown && need_feed && A_out_ok) {
                if (A->mode == TASK_IDLE) {
                    lane_start_task(A, TASK_FEED, feed_sps, true, 0.0f);
                } else if (A->mode == TASK_FEED) {
                    // live update from pot
                    A->steps_per_sec = feed_sps;
                }
            } else {
                if (A->mode == TASK_FEED) lane_stop_task(A);
            }
        } else {
            // Manual active: stop any auto-feed to avoid fighting
            if (L1.mode == TASK_FEED) lane_stop_task(&L1);
            if (L2.mode == TASK_FEED) lane_stop_task(&L2);
        }

        // Update prev flags
        L1.prev_in_present = l1_in_present;
        L2.prev_in_present = l2_in_present;

        // Process lanes
        lane_process(&L1);
        lane_process(&L2);

        // LED
        led_state_t led = LED_IDLE;
        if (any_manual) led = LED_MANUAL_REV;
        else {
            if (swap_armed) led = LED_SWAP_ARMED;
            if (L1.mode == TASK_AUTOLOAD || L2.mode == TASK_AUTOLOAD) led = LED_AUTOLOAD;
            if (L1.mode == TASK_FEED || L2.mode == TASK_FEED) led = LED_FEEDING;
        }
        status_led_update(led, t_us);

#if DEBUG_PRINTS
        if (absolute_time_diff_us(last_dbg, now) > DEBUG_PERIOD_US) {
            last_dbg = now;
            DBG_PRINTF(
                "A=%d armed=%d man=%d feed_sps=%d  rev1=%d rev2=%d  "
                "l1[in=%d out=%d mode=%d]  l2[in=%d out=%d mode=%d]  "
                "y=%d yclr=%d  bufL=%d bufH=%d\n",
                active_lane, swap_armed, any_manual, feed_sps,
                rev_l1, rev_l2,
                l1_in_present, l1_out_present, (int)L1.mode,
                l2_in_present, l2_out_present, (int)L2.mode,
                y_present, y_clear,
                buffer_low, buffer_high
            );
        }
#endif

        sleep_ms(1);
    }

    return 0;
}
