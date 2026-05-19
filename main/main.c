#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "uart_handler.h"
#include "parse_worker.h"
#include "cardkb.h"
#include "led_indicator.h"
#include "ui_helpers.h"
#include "home_screen.h"
#include "device_info.h"
#include <math.h>
#include <string.h>

static const char *TAG = "app";

#define BOOT_SOUND_VOLUME  69
#define MELODY_SR          48000

static esp_codec_dev_handle_t spk_codec = NULL;

static lv_obj_t  *splash_screen         = NULL;
static lv_obj_t  *splash_loading_label   = NULL;
static lv_obj_t  *splash_detecting_label = NULL;
static lv_timer_t *splash_timer          = NULL;
static int         glitch_frame          = 0;
static bool        splash_detection_started = false;

/* ------------------------------------------------------------------ */
/*  Startup melody (sine-wave synthesis via esp_codec_dev)              */
/* ------------------------------------------------------------------ */

typedef struct { float freq; int ms; } note_t;

static const note_t nokia_tune[] = {
    { 659.25f, 125 }, { 587.33f, 125 }, { 369.99f, 250 }, { 415.30f, 250 },
    { 554.37f, 125 }, { 493.88f, 125 }, { 293.66f, 250 }, { 329.63f, 250 },
    { 493.88f, 125 }, { 440.00f, 125 }, { 277.18f, 250 }, { 329.63f, 250 },
    { 440.00f, 500 },
};

static const note_t intel_jingle[] = {
    { 622.25f, 100 }, { 659.25f, 100 }, { 493.88f, 110 },
    { 622.25f, 120 }, { 466.16f, 280 },
};

static const note_t star_wars_motif[] = {
    { 440.00f, 380 }, { 440.00f, 380 }, { 440.00f, 380 },
    { 349.23f, 250 }, { 523.25f, 150 }, { 440.00f, 380 },
    { 349.23f, 250 }, { 523.25f, 150 }, { 440.00f, 680 },
};

static void play_startup_beep(void *arg)
{
    (void)arg;
    boot_sound_mode_t mode = boot_sound_mode;
    if (mode == BOOT_SOUND_OFF || !spk_codec) {
        vTaskDelete(NULL);
        return;
    }

    const note_t *melody;
    int melody_notes;
    const char *melody_name;

    switch (mode) {
        default:
        case BOOT_SOUND_NOKIA:
            melody = nokia_tune;
            melody_notes = sizeof(nokia_tune) / sizeof(nokia_tune[0]);
            melody_name = "Nokia";
            break;
        case BOOT_SOUND_INTEL:
            melody = intel_jingle;
            melody_notes = sizeof(intel_jingle) / sizeof(intel_jingle[0]);
            melody_name = "Intel";
            break;
        case BOOT_SOUND_STARWARS:
            melody = star_wars_motif;
            melody_notes = sizeof(star_wars_motif) / sizeof(star_wars_motif[0]);
            melody_name = "Star Wars";
            break;
    }

    ESP_LOGI(TAG, "Playing startup melody: %s", melody_name);

    const int pause_ms = (mode == BOOT_SOUND_STARWARS) ? 42 : 20;
    const int final_silence_ms = 160;
    int total_ms = final_silence_ms;
    for (int n = 0; n < melody_notes; n++)
        total_ms += melody[n].ms + pause_ms;

    const int total_samples = MELODY_SR * total_ms / 1000;
    const size_t buf_bytes = total_samples * sizeof(int16_t);

    int16_t *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(buf_bytes);
    }
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate melody buffer (%d bytes)", (int)buf_bytes);
        vTaskDelete(NULL);
        return;
    }
    memset(buf, 0, buf_bytes);

    int pos = 0;
    for (int n = 0; n < melody_notes; n++) {
        float freq = melody[n].freq;
        int note_samples = MELODY_SR * melody[n].ms / 1000;
        int attack  = MELODY_SR * 5  / 1000;
        int release = MELODY_SR * 15 / 1000;

        for (int i = 0; i < note_samples && pos < total_samples; i++) {
            float t = (float)i / MELODY_SR;
            float env = 1.0f;
            if (i < attack)
                env = (float)i / attack;
            else if (i >= note_samples - release)
                env = (float)(note_samples - 1 - i) / (release > 1 ? release - 1 : 1);

            buf[pos++] = (int16_t)(sinf(2.0f * (float)M_PI * freq * t) * env * 0.85f * 32767.0f);
        }

        int gap = MELODY_SR * pause_ms / 1000;
        for (int i = 0; i < gap && pos < total_samples; i++)
            buf[pos++] = 0;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .channel_mask    = 0,
        .sample_rate     = MELODY_SR,
        .mclk_multiple   = 0,
    };

    if (esp_codec_dev_open(spk_codec, &fs) == ESP_CODEC_DEV_OK) {
        esp_codec_dev_set_out_vol(spk_codec, BOOT_SOUND_VOLUME);
        esp_codec_dev_write(spk_codec, buf, pos * sizeof(int16_t));
        vTaskDelay(pdMS_TO_TICKS(final_silence_ms + 40));
        esp_codec_dev_close(spk_codec);
    } else {
        ESP_LOGW(TAG, "Failed to open codec for melody");
    }

    free(buf);
    ESP_LOGI(TAG, "Startup melody done (%s)", melody_name);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Splash screen                                                      */
/* ------------------------------------------------------------------ */

static void detection_complete_cb(lv_timer_t *timer)
{
    (void)timer;

    if (splash_timer) {
        lv_timer_del(splash_timer);
        splash_timer = NULL;
    }

    if (splash_screen) {
        lv_obj_del(splash_screen);
        splash_screen = NULL;
    }
    splash_loading_label   = NULL;
    splash_detecting_label = NULL;

    show_home_screen();
    ESP_LOGI(TAG, "Splash done, home screen shown");
}

static void splash_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    glitch_frame++;

    if (splash_loading_label) {
        static const char *frames[] = {
            "LOADING", "LOADING.", "LOADING..", "LOADING..."
        };
        lv_label_set_text(splash_loading_label, frames[(glitch_frame / 3) % 4]);
        lv_obj_set_style_text_opa(splash_loading_label,
            (glitch_frame % 10 < 6) ? LV_OPA_100 : LV_OPA_60, 0);
    }

    if (glitch_frame >= 22 && splash_detecting_label) {
        lv_obj_clear_flag(splash_detecting_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (!splash_detection_started && glitch_frame >= 28) {
        splash_detection_started = true;
        lv_timer_t *det = lv_timer_create(detection_complete_cb, 700, NULL);
        lv_timer_set_repeat_count(det, 1);
    }
}

static void show_splash_screen(void)
{
    ESP_LOGI(TAG, "Showing splash screen...");

    glitch_frame = 0;
    splash_detection_started = false;

    splash_screen = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(splash_screen);
    lv_obj_set_size(splash_screen, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(splash_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(splash_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(splash_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(splash_screen);
    lv_label_set_text(title, "LAB5");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, COLOR_MAGENTA, 0);
    lv_obj_set_style_text_letter_space(title, 4, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *subtitle = lv_label_create(splash_screen);
    lv_label_set_text(subtitle, "LABORATORIUM");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x93A6BC), 0);
    lv_obj_set_style_text_letter_space(subtitle, 2, 0);
    lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 4);

    splash_loading_label = lv_label_create(splash_screen);
    lv_label_set_text(splash_loading_label, "LOADING...");
    lv_obj_set_style_text_font(splash_loading_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(splash_loading_label, lv_color_hex(0xD8D8D8), 0);
    lv_obj_align(splash_loading_label, LV_ALIGN_BOTTOM_MID, 0, -50);

    splash_detecting_label = lv_label_create(splash_screen);
    lv_label_set_text(splash_detecting_label, "Detecting devices...");
    lv_obj_set_style_text_font(splash_detecting_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(splash_detecting_label, lv_color_hex(0x9FE7D8), 0);
    lv_obj_align(splash_detecting_label, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_add_flag(splash_detecting_label, LV_OBJ_FLAG_HIDDEN);

    splash_timer = lv_timer_create(splash_timer_cb, 100, NULL);

    if (boot_sound_mode != BOOT_SOUND_OFF) {
        xTaskCreate(play_startup_beep, "melody", 8192, NULL, 3, NULL);
    }
}

/* ------------------------------------------------------------------ */
/*  app_main                                                           */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "M5Stack Core S3 – LABORATORIUM");

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    load_settings_from_nvs();

    /* Speaker codec */
    spk_codec = bsp_audio_codec_speaker_init();
    if (!spk_codec) {
        ESP_LOGW(TAG, "Speaker codec init failed, boot sound disabled");
    }

    /* BSP: display + touch + LVGL */
    bsp_display_start();
    bsp_display_brightness_set(UI_DEFAULT_BRIGHTNESS);
    ui_screen_timeout_init();

    parse_worker_init();

    /* UART to ESP32C5 */
    uart_handler_init();

    /* Probe firmware for the provisioned board_name (used as SubGHz title). */
    device_info_init();

    /* M5GO Bottom3 LED strip (10x WS2812 on GPIO5) */
    led_indicator_init();

    /* CardKB on Grove Port A (I2C) */
    ESP_ERROR_CHECK(cardkb_init());

    bsp_display_lock(0);
    show_splash_screen();
    bsp_display_unlock();

    ESP_LOGI(TAG, "Init complete");
}
