/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file esp_timer_sim.c
 * @brief esp_timer for the linux target, which ships the header only: the
 *        clock is CLOCK_MONOTONIC since start, and callbacks run on one
 *        FreeRTOS task ("esp_timer"), as ESP_TIMER_TASK dispatch does on the chip.
 *        Also mbedTLS's allocator hooks, which IDF leaves out on the host.
 */
#include "esp_timer.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct esp_timer {
    esp_timer_cb_t cb;
    void *arg;
    int64_t due;       /* 0 = not armed */
    int64_t period;    /* 0 = one shot */
    struct esp_timer *next;
};

static struct esp_timer *s_list;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static int64_t s_t0;

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t const now = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    if ( 0 == s_t0 )
    {
        s_t0 = now - 1;
    }
    return now - s_t0;
}

static void timer_task(void *arg)
{
    (void)arg;
    for ( ;; )
    {
        int64_t const now = esp_timer_get_time();
        esp_timer_cb_t cb = NULL;
        void *cb_arg = NULL;
        int64_t next_due = INT64_MAX;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for ( struct esp_timer *t = s_list; t; t = t->next )
        {
            if ( (0 != t->due) && (t->due <= now) && (NULL == cb) )
            {
                cb = t->cb;
                cb_arg = t->arg;
                t->due = (0 != t->period) ? (t->due + t->period) : 0;
                if ( (0 != t->due) && (t->due < now) )
                {
                    t->due = now + t->period;   /* do not race to catch up */
                }
            }
            if ( (0 != t->due) && (t->due < next_due) )
            {
                next_due = t->due;
            }
        }
        xSemaphoreGive(s_lock);
        if ( NULL != cb )
        {
            cb(cb_arg);
            continue;
        }
        int64_t wait_us = (INT64_MAX == next_due) ? 100000 : (next_due - now);
        if ( wait_us > 100000 ) { wait_us = 100000; }
        TickType_t ticks = pdMS_TO_TICKS((uint32_t)(wait_us / 1000));
        (void)ulTaskNotifyTake(pdTRUE, (0U == ticks) ? 1U : ticks);
    }
}

static void ensure(void)
{
    if ( NULL == s_lock )
    {
        s_lock = xSemaphoreCreateMutex();
        (void)xTaskCreate(timer_task, "esp_timer", 32768, NULL, configMAX_PRIORITIES - 2, &s_task);
    }
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    if ( (NULL == args) || (NULL == args->callback) || (NULL == out) )
    {
        return ESP_ERR_INVALID_ARG;
    }
    ensure();
    struct esp_timer *t = calloc(1, sizeof(*t));
    if ( NULL == t )
    {
        return ESP_ERR_NO_MEM;
    }
    t->cb = args->callback;
    t->arg = args->arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    t->next = s_list;
    s_list = t;
    xSemaphoreGive(s_lock);
    *out = t;
    return ESP_OK;
}

static esp_err_t arm(esp_timer_handle_t t, uint64_t us, bool periodic)
{
    if ( NULL == t ) { return ESP_ERR_INVALID_ARG; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t rc = ESP_OK;
    if ( 0 != t->due )
    {
        rc = ESP_ERR_INVALID_STATE;
    }
    else
    {
        t->period = periodic ? (int64_t)us : 0;
        t->due = esp_timer_get_time() + (int64_t)((0U == us) ? 1U : us);
    }
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return rc;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us) { return arm(t, timeout_us, false); }
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period) { return arm(t, period, true); }

esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    if ( NULL == t ) { return ESP_ERR_INVALID_ARG; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t const rc = (0 != t->due) ? ESP_OK : ESP_ERR_INVALID_STATE;
    t->due = 0;
    xSemaphoreGive(s_lock);
    return rc;
}

esp_err_t esp_timer_delete(esp_timer_handle_t t)
{
    if ( NULL == t ) { return ESP_ERR_INVALID_ARG; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if ( 0 != t->due )
    {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    for ( struct esp_timer **pp = &s_list; *pp; pp = &(*pp)->next )
    {
        if ( *pp == t )
        {
            *pp = t->next;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    free(t);
    return ESP_OK;
}

void *esp_mbedtls_mem_calloc(size_t n, size_t size) { return calloc(n, size); }
void esp_mbedtls_mem_free(void *ptr) { free(ptr); }
