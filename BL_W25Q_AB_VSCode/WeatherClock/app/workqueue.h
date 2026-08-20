#ifndef __APP_WORKQUEUE_H__
#define __APP_WORKQUEUE_H__

#include <stdbool.h>

typedef void (*work_t)(void *param);

void workqueue_init(void);

/**
 * @brief 尝试把一项工作放入后台队列。
 * @return 成功入队返回 true；队列已满时返回 false。
 *
 * 该接口也会被 FreeRTOS 软件定时器回调调用，因此不得等待队列空位，否则一个
 * 拥堵的网络任务可能连带阻塞整个 Timer Service 任务。
 */
bool workqueue_run(work_t work, void *param);

#endif /* __APP_WORKQUEUE_H__ */
