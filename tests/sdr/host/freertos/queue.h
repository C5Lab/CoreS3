#pragma once
#include "FreeRTOS.h"
struct HostQueue;
using QueueHandle_t = HostQueue *;
QueueHandle_t xQueueCreate(unsigned length, unsigned item_size);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueOverwrite(QueueHandle_t, const void *);
void vQueueDelete(QueueHandle_t);
