#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreate(void (*function)(void *), const char *, unsigned, void *, unsigned, void *);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(void *);
