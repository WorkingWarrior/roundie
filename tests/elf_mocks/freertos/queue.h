#pragma once
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned count, unsigned size);
int xQueueReceive(QueueHandle_t queue, void *out, unsigned wait);
int xQueueSend(QueueHandle_t queue, const void *value, unsigned wait);
void vQueueDelete(QueueHandle_t queue);
