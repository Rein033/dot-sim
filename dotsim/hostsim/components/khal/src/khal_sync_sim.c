/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_sync_sim.c
 * @brief khal_sync on the linux FreeRTOS port: the same semaphores as the
 *        board, and a FreeRTOS mutex for the environment lock (glibc has no
 *        picolibc ENV_LOCK).
 */
#include "khal_sync.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

khal_mutex_t khal_mutex_create(void)
{
    return (khal_mutex_t)xSemaphoreCreateMutex();
}

void khal_mutex_lock(khal_mutex_t m)
{
    if ( NULL != m )
    {
        (void)xSemaphoreTake((SemaphoreHandle_t)m, portMAX_DELAY);
    }
}

void khal_mutex_unlock(khal_mutex_t m)
{
    if ( NULL != m )
    {
        (void)xSemaphoreGive((SemaphoreHandle_t)m);
    }
}

khal_sem_t khal_sem_create(void)
{
    return (khal_sem_t)xSemaphoreCreateBinary();
}

bool khal_sem_wait_timeout(khal_sem_t s, uint32_t ms)
{
    if ( NULL == s )
    {
        return false;
    }
    return xSemaphoreTake((SemaphoreHandle_t)s, pdMS_TO_TICKS(ms)) == pdTRUE;
}

void khal_sem_delete(khal_sem_t s)
{
    if ( NULL != s )
    {
        vSemaphoreDelete((SemaphoreHandle_t)s);
    }
}

void khal_sem_signal(khal_sem_t s)
{
    if ( NULL != s )
    {
        (void)xSemaphoreGive((SemaphoreHandle_t)s);
    }
}

static SemaphoreHandle_t s_env;

void khal_env_lock(void)
{
    if ( NULL == s_env )
    {
        s_env = xSemaphoreCreateRecursiveMutex();
    }
    (void)xSemaphoreTakeRecursive(s_env, portMAX_DELAY);
}

void khal_env_unlock(void)
{
    if ( NULL != s_env )
    {
        (void)xSemaphoreGiveRecursive(s_env);
    }
}
