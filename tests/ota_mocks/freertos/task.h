#pragma once
#include "FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
unsigned uxTaskGetStackHighWaterMark(void *task);
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetHandle(const char *name);
unsigned uxTaskGetNumberOfTasks(void);
