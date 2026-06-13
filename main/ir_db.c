#include "ir_db.h"

#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "ir_db";

/* Embedded by EMBED_FILES "assets/tv_power.irdb" in main/CMakeLists.txt. */
extern const uint8_t tv_power_irdb_start[] asm("_binary_tv_power_irdb_start");
extern const uint8_t tv_power_irdb_end[] asm("_binary_tv_power_irdb_end");

typedef struct {
    uint32_t carrier_hz;
    uint8_t duty_pct;
    uint16_t count;
    const uint8_t *timings; /* points into the embedded blob (LE u32 each) */
} ir_entry_t;

static ir_entry_t *s_entries;
static size_t s_count;
static bool s_init_done;

static uint16_t rd_u16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t rd_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void ir_db_init(void)
{
    if (s_init_done) return;
    s_init_done = true;

    const uint8_t *p = tv_power_irdb_start;
    const uint8_t *end = tv_power_irdb_end;

    if ((size_t)(end - p) < 8 || memcmp(p, "IRDB", 4) != 0) {
        ESP_LOGE(TAG, "bad IRDB header");
        return;
    }

    uint16_t signal_count = rd_u16(p + 6);
    p += 8;

    s_entries = calloc(signal_count, sizeof(ir_entry_t));
    if (!s_entries) {
        ESP_LOGE(TAG, "calloc index failed");
        return;
    }

    for (uint16_t i = 0; i < signal_count; ++i) {
        if (p + 8 > end) {
            ESP_LOGE(TAG, "truncated record %u", i);
            break;
        }
        ir_entry_t *e = &s_entries[s_count];
        e->carrier_hz = rd_u32(p);
        e->duty_pct = p[4];
        e->count = rd_u16(p + 6);
        e->timings = p + 8;
        p += 8 + (size_t)e->count * 4;
        if (p > end) {
            ESP_LOGE(TAG, "truncated timings %u", i);
            break;
        }
        s_count++;
    }

    ESP_LOGI(TAG, "loaded %zu IR signals", s_count);
}

size_t ir_db_count(void)
{
    ir_db_init();
    return s_count;
}

bool ir_db_get(size_t index, uint32_t *carrier_hz, uint8_t *duty_pct,
               uint32_t **timings_out, uint16_t *count_out)
{
    ir_db_init();
    if (index >= s_count) return false;

    const ir_entry_t *e = &s_entries[index];
    uint32_t *buf = malloc((size_t)e->count * sizeof(uint32_t));
    if (!buf) return false;

    /* Blob and target are both little-endian, so a flat copy is correct. */
    memcpy(buf, e->timings, (size_t)e->count * sizeof(uint32_t));

    if (carrier_hz) *carrier_hz = e->carrier_hz;
    if (duty_pct) *duty_pct = e->duty_pct;
    if (count_out) *count_out = e->count;
    *timings_out = buf;
    return true;
}
