#include "led_indicator.h"

#include "led_strip.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include <string.h>

#define LED_STRIP_GPIO       5
#define LED_STRIP_NUM        10
#define LED_TICK_PERIOD_US   50000  /* 50 ms */
#define LED_GREEN_HOLD_MS    1000
#define LED_BLINK_PERIOD_MS  80

static const char *TAG = "led_indicator";

typedef enum {
    LED_MODE_IDLE = 0,
    LED_MODE_GREEN,
    LED_MODE_RED_BLINK,
} led_mode_t;

static led_strip_handle_t s_strip;
static esp_timer_handle_t s_tick_timer;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static led_mode_t s_mode;
static uint64_t   s_mode_until_ms;   /* 0 = no auto-stop */
static uint64_t   s_blink_next_ms;
static bool       s_blink_on;
static led_mode_t s_last_applied_mode = (led_mode_t)-1;
static bool       s_last_applied_blink_on;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void apply_pixels_locked(led_mode_t mode, bool blink_on)
{
    if (!s_strip) return;

    if (mode == s_last_applied_mode && blink_on == s_last_applied_blink_on) {
        return;
    }
    s_last_applied_mode = mode;
    s_last_applied_blink_on = blink_on;

    switch (mode) {
        case LED_MODE_GREEN:
            for (int i = 0; i < LED_STRIP_NUM; i++) {
                led_strip_set_pixel(s_strip, i, 0, 64, 0);
            }
            break;
        case LED_MODE_RED_BLINK:
            if (blink_on) {
                for (int i = 0; i < LED_STRIP_NUM; i++) {
                    led_strip_set_pixel(s_strip, i, 96, 0, 0);
                }
            } else {
                led_strip_clear(s_strip);
            }
            break;
        case LED_MODE_IDLE:
        default:
            led_strip_clear(s_strip);
            break;
    }

    led_strip_refresh(s_strip);
}

static void tick_cb(void *arg)
{
    (void)arg;

    led_mode_t mode;
    bool blink_on;
    uint64_t now = now_ms();

    portENTER_CRITICAL(&s_lock);

    if (s_mode != LED_MODE_IDLE && s_mode_until_ms != 0 && now >= s_mode_until_ms) {
        s_mode = LED_MODE_IDLE;
        s_mode_until_ms = 0;
        s_blink_on = false;
    }

    if (s_mode == LED_MODE_RED_BLINK && now >= s_blink_next_ms) {
        s_blink_on = !s_blink_on;
        s_blink_next_ms = now + LED_BLINK_PERIOD_MS;
    }

    mode = s_mode;
    blink_on = s_blink_on;

    portEXIT_CRITICAL(&s_lock);

    apply_pixels_locked(mode, blink_on);
}

void led_indicator_init(void)
{
    if (s_strip) return;

    led_strip_config_t strip_cfg = {
        .strip_gpio_num   = LED_STRIP_GPIO,
        .max_leds         = LED_STRIP_NUM,
        .led_model        = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000, /* 10 MHz */
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        s_strip = NULL;
        return;
    }

    led_strip_clear(s_strip);

    const esp_timer_create_args_t targs = {
        .callback        = tick_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "led_indicator",
    };
    err = esp_timer_create(&targs, &s_tick_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return;
    }
    esp_timer_start_periodic(s_tick_timer, LED_TICK_PERIOD_US);

    ESP_LOGI(TAG, "LED indicator ready (GPIO%d, %d LEDs)", LED_STRIP_GPIO, LED_STRIP_NUM);
}

void led_indicator_signal_received(void)
{
    if (!s_strip) return;

    uint64_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    /* TX (red blink) takes priority and is not interrupted by RX pulses. */
    if (s_mode != LED_MODE_RED_BLINK) {
        s_mode = LED_MODE_GREEN;
        s_mode_until_ms = now + LED_GREEN_HOLD_MS;
    }
    portEXIT_CRITICAL(&s_lock);
}

void led_indicator_tx_start(void)
{
    if (!s_strip) return;

    uint64_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    s_mode = LED_MODE_RED_BLINK;
    s_mode_until_ms = 0;
    s_blink_on = true;
    s_blink_next_ms = now + LED_BLINK_PERIOD_MS;
    portEXIT_CRITICAL(&s_lock);
}

void led_indicator_tx_stop(void)
{
    if (!s_strip) return;

    portENTER_CRITICAL(&s_lock);
    if (s_mode == LED_MODE_RED_BLINK) {
        s_mode = LED_MODE_IDLE;
        s_mode_until_ms = 0;
        s_blink_on = false;
    }
    portEXIT_CRITICAL(&s_lock);
}

void led_indicator_tx_pulse(uint32_t ms)
{
    if (!s_strip) return;

    uint64_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    s_mode = LED_MODE_RED_BLINK;
    s_mode_until_ms = now + ms;
    s_blink_on = true;
    s_blink_next_ms = now + LED_BLINK_PERIOD_MS;
    portEXIT_CRITICAL(&s_lock);
}
