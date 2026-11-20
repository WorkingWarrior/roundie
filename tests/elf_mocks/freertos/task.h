#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(TickType_t ticks);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack, void *arg,
                 unsigned priority, TaskHandle_t *out);
