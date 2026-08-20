/**
 * @file workqueue.c
 * @brief 串行执行SNTP和HTTP工作的单任务FreeRTOS队列。
 *
 * 队列让Timer Service和业务任务快速投递网络工作，所有ESP请求最终在同一个
 * 工作任务中执行，避免多个长请求同时占用AT通道。
 */

#include "workqueue.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

typedef struct
{
    work_t work;
    void *param;
} work_message_t;

static QueueHandle_t work_msg_queue;

static void workqueue_task(void *parameter)
{
    work_message_t message;

    (void)parameter;
    while (1)
    {
        xQueueReceive(work_msg_queue, &message, portMAX_DELAY);
        message.work(message.param);
    }
}

void workqueue_init(void)
{
    work_msg_queue = xQueueCreate(16, sizeof(work_message_t));
    configASSERT(work_msg_queue != NULL);
    configASSERT(xTaskCreate(workqueue_task, "workqueue", 1024, NULL, 5,
                             NULL) == pdPASS);
}

bool workqueue_run(work_t work, void *param)
{
    work_message_t message = {work, param};

    configASSERT(work_msg_queue != NULL);
    return xQueueSend(work_msg_queue, &message, 0u) == pdTRUE;
}
