#include "parse_worker.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

static const char *TAG = "parse_worker";

#define QUEUE_DEPTH 12
#define WORKER_STACK 8192
#define WORKER_PRIO  3

typedef struct {
    parse_worker_fn_t fn;
    void             *arg;
} parse_job_t;

static QueueHandle_t s_queue;

static void worker_task(void *unused)
{
    (void)unused;
    parse_job_t job;

    for (;;) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE)
            continue;
        if (job.fn)
            job.fn(job.arg);
    }
}

void parse_worker_init(void)
{
    if (s_queue)
        return;

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(parse_job_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        worker_task, "parse_wk", WORKER_STACK, NULL, WORKER_PRIO,
        NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return;
    }
    ESP_LOGI(TAG, "PSRAM-stack worker on CPU0 ready");
}

bool parse_worker_post(parse_worker_fn_t fn, void *arg)
{
    if (!s_queue || !fn) {
        ESP_LOGW(TAG, "post skipped (no queue or null fn)");
        return false;
    }
    parse_job_t job = { .fn = fn, .arg = arg };
    if (xQueueSend(s_queue, &job, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, job dropped");
        return false;
    }
    return true;
}
