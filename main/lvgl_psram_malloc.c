/* Custom LVGL allocator that routes all internal LVGL heap traffic
 * (widgets, styles, draw buffers, children arrays...) into PSRAM.
 *
 * Replaces LV_USE_BUILTIN_MALLOC (64 KB static SRAM TLSF pool) which is
 * far too small for screens with 50+ flex children. With 8 MB PSRAM
 * the practical cap disappears.
 *
 * Activated by CONFIG_LV_USE_CUSTOM_MALLOC=y in sdkconfig — when that flag
 * is set, the bundled builtin/clib backends compile out and we must
 * provide every lv_mem_* core symbol below.
 */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

#include "lvgl.h"

static const char *TAG = "lvgl_psram";

/* Try PSRAM first; if PSRAM is full or unavailable for the requested size,
 * fall back to internal RAM so the UI keeps working instead of crashing. */
static inline void *psram_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
    p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (!p) ESP_LOGE(TAG, "alloc %u B failed (PSRAM+SRAM)", (unsigned)size);
    return p;
}

static inline void *psram_realloc(void *ptr, size_t size)
{
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p || size == 0) return p;
    p = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    if (!p) ESP_LOGE(TAG, "realloc %u B failed (PSRAM+SRAM)", (unsigned)size);
    return p;
}

void lv_mem_init(void)
{
    /* Nothing to init: PSRAM heap is already up by the time LVGL starts. */
}

void lv_mem_deinit(void)
{
    /* Nothing to release. */
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    return psram_alloc(size);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return psram_realloc(p, new_size);
}

void lv_free_core(void *p)
{
    if (p) heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    if (mon_p) memset(mon_p, 0, sizeof(*mon_p));
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
