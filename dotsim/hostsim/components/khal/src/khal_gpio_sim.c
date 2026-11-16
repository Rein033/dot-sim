/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_gpio_sim.c
 * @brief The P4's GPIO, simulated: a level per pin and edge interrupts.
 *
 * An interrupt on the board runs a handler in ISR context. Here the handlers
 * run in one FreeRTOS task of the highest priority (sim.irq), woken by the
 * bridge through a flag it polls every millisecond: the bridge is a plain
 * pthread and may not call FreeRTOS. The handlers call the FromISR APIs, which
 * the linux port allows from a task.
 */
#include "khal_gpio.h"

#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kdotsim.h"

#define PINS 64U

typedef struct {
    khal_gpio_cb_t cb;
    void *arg;
    khal_gpio_irq_t kind;
    atomic_bool armed;
    bool last;
} sim_irq_t;

static atomic_bool s_level[PINS];
static atomic_bool s_out[PINS];
static sim_irq_t s_irq[PINS];
static atomic_bool s_dirty;
static TaskHandle_t s_task;

static void irq_task(void *arg)
{
    (void)arg;
    for ( ;; )
    {
        vTaskDelay(1);
        if ( !atomic_exchange(&s_dirty, false) )
        {
            /* Level interrupts keep firing while the level holds. */
            bool any = false;
            for ( uint32_t p = 0U; p < PINS; ++p )
            {
                if ( (NULL != s_irq[p].cb) && atomic_load(&s_irq[p].armed)
                     && (KHAL_GPIO_IRQ_ANY_EDGE != s_irq[p].kind) )
                {
                    any = true;
                }
            }
            if ( !any )
            {
                continue;
            }
        }
        for ( uint32_t p = 0U; p < PINS; ++p )
        {
            sim_irq_t *const q = &s_irq[p];
            if ( NULL == q->cb )
            {
                continue;
            }
            bool const lv = atomic_load(&s_level[p]);
            bool fire = false;
            switch ( q->kind )
            {
            case KHAL_GPIO_IRQ_ANY_EDGE:
                fire = (lv != q->last);
                break;
            case KHAL_GPIO_IRQ_LOW_LEVEL:
                fire = !lv;
                break;
            case KHAL_GPIO_IRQ_HIGH_LEVEL:
                fire = lv;
                break;
            default:
                break;
            }
            q->last = lv;
            if ( fire && atomic_load(&q->armed) )
            {
                if ( KHAL_GPIO_IRQ_ANY_EDGE != q->kind )
                {
                    /* A level interrupt disarms itself until the owner rearms it, as on the chip's driver. */
                    atomic_store(&q->armed, false);
                }
                q->cb(q->arg);
            }
        }
    }
}

static void ensure_task(void)
{
    if ( NULL == s_task )
    {
        (void)xTaskCreate(irq_task, "sim.irq", 8192, NULL, configMAX_PRIORITIES - 1, &s_task);
    }
}

void kdotsim_gpio_set(uint8_t pin, bool level)
{
    if ( pin < PINS )
    {
        atomic_store(&s_level[pin], level);
        atomic_store(&s_dirty, true);
    }
}

kode_status_t khal_gpio_input(uint8_t pin, khal_gpio_pull_t pull)
{
    if ( pin >= PINS )
    {
        return KODE_DENIED;
    }
    atomic_store(&s_out[pin], false);
    if ( KHAL_GPIO_PULL_UP == pull )
    {
        /* Buttons and open-drain lines idle high. */
        static atomic_bool once[PINS];
        if ( !atomic_exchange(&once[pin], true) )
        {
            atomic_store(&s_level[pin], true);
        }
    }
    return KODE_OK;
}

kode_status_t khal_gpio_output(uint8_t pin, bool level)
{
    if ( pin >= PINS )
    {
        return KODE_DENIED;
    }
    atomic_store(&s_out[pin], true);
    atomic_store(&s_level[pin], level);
    return KODE_OK;
}

kode_status_t khal_gpio_write(uint8_t pin, bool level)
{
    if ( pin >= PINS )
    {
        return KODE_DENIED;
    }
    atomic_store(&s_level[pin], level);
    return KODE_OK;
}

void khal_gpio_release(uint8_t pin)
{
    if ( pin < PINS )
    {
        atomic_store(&s_out[pin], false);
    }
}

bool khal_gpio_level(uint8_t pin)
{
    return (pin < PINS) ? atomic_load(&s_level[pin]) : false;
}

bool khal_gpio_driven_low(uint8_t pin)
{
    return (pin < PINS) && atomic_load(&s_out[pin]) && !atomic_load(&s_level[pin]);
}

kode_status_t khal_gpio_service(void)
{
    ensure_task();
    return KODE_OK;
}

kode_status_t khal_gpio_irq_attach(uint8_t pin, khal_gpio_irq_t kind, khal_gpio_cb_t cb, void *arg)
{
    if ( (pin >= PINS) || (NULL == cb) )
    {
        return KODE_DENIED;
    }
    ensure_task();
    s_irq[pin].kind = kind;
    s_irq[pin].arg = arg;
    s_irq[pin].last = atomic_load(&s_level[pin]);
    s_irq[pin].cb = cb;
    atomic_store(&s_irq[pin].armed, true);
    atomic_store(&s_dirty, true);
    return KODE_OK;
}

void khal_gpio_irq_enable(uint8_t pin)
{
    if ( pin < PINS )
    {
        atomic_store(&s_irq[pin].armed, true);
        atomic_store(&s_dirty, true);
    }
}

kode_status_t khal_gpio_wake(uint8_t pin, bool on)
{
    (void)pin;
    (void)on;
    return KODE_OK;
}

kode_status_t khal_gpio_wake_low(uint8_t pin)
{
    (void)pin;
    return KODE_OK;
}
