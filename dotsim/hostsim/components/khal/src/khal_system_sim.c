/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_system_sim.c
 * @brief The system seam on the host: restart and panic end the sim, the heap
 *        is the process's, tasks are FreeRTOS tasks of the linux port.
 */
#include "khal_system.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "kdotsim.h"

#define SIM_HEAP_BYTES (32U * 1024U * 1024U)   /* PSRAM on the Dot */
#define SIM_INTERNAL_BYTES (512U * 1024U)

void khal_restart(void)
{
    kdotsim_exit("restart");
}

void khal_panic(const char *why)
{
    char line[256];
    int const n = snprintf(line, sizeof(line), "PANIC: %s\n", (NULL != why) ? why : "?");
    kdotsim_log(line, (size_t)n);
    kdotsim_exit("panic");
}

uint32_t khal_random(void)
{
    uint32_t r = 0U;
    if ( getrandom(&r, sizeof(r), 0) != (ssize_t)sizeof(r) )
    {
        r = (uint32_t)rand();
    }
    return r;
}

void khal_panic_print(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int const n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if ( n > 0 )
    {
        fputs(line, stderr);
        kdotsim_log(line, ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1U);
    }
}

static size_t used_bytes(void)
{
    struct mallinfo2 const mi = mallinfo2();
    return mi.uordblks;
}

size_t khal_heap_free(void)
{
    size_t const u = used_bytes();
    return (u < SIM_HEAP_BYTES) ? (SIM_HEAP_BYTES - u) : 0U;
}
size_t khal_psram_free(void) { return khal_heap_free(); }
size_t khal_psram_largest(void) { return khal_heap_free(); }
size_t khal_heap_internal_free(void) { return SIM_INTERNAL_BYTES / 2U; }
size_t khal_heap_internal_largest(void) { return SIM_INTERNAL_BYTES / 4U; }
size_t khal_heap_internal_low(void) { return SIM_INTERNAL_BYTES / 2U; }
void khal_heap_watch(void) {}
uint32_t khal_heap_alloc_failures(void) { return 0U; }

size_t khal_task_list(khal_task_info_t *out, size_t max, size_t *total)
{
    (void)out; (void)max;
    if ( NULL != total ) { *total = 0U; }
    return 0U;
}

uint32_t khal_task_priority_set(uint32_t priority)
{
    uint32_t const was = (uint32_t)uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, (UBaseType_t)priority);
    return was;
}

uint32_t khal_task_stack_free(void)
{
    if ( xTaskGetCurrentTaskHandle() == NULL ) { return 0U; }
    return 16384U;   /* the linux port runs tasks on pthread stacks */
}

bool khal_task_stack_in_psram(void) { return false; }

const char *khal_task_name(void)
{
    TaskHandle_t const self = xTaskGetCurrentTaskHandle();
    return (NULL == self) ? NULL : pcTaskGetName(self);
}

typedef struct {
    khal_task_fn_t fn;
    void *arg;
} sys_task_ctx_t;

static void sys_task_main(void *ctx_ptr)
{
    sys_task_ctx_t *const ctx = (sys_task_ctx_t *)ctx_ptr;
    khal_task_fn_t const fn = ctx->fn;
    void *const arg = ctx->arg;
    free(ctx);
    fn(arg);
    vTaskDelete(NULL);
}

kode_status_t khal_task_start(khal_task_fn_t fn, const char *name, uint32_t stack_bytes, uint32_t prio, void *arg)
{
    if ( (NULL == fn) || (NULL == name) || (0U == stack_bytes) ) { return KODE_DENIED; }
    sys_task_ctx_t *const ctx = malloc(sizeof(*ctx));
    if ( NULL == ctx ) { return KODE_NO_RESOURCES; }
    ctx->fn = fn;
    ctx->arg = arg;
    /* Pthread stacks: give every task enough room for host libc. */
    uint32_t const stack = (stack_bytes < 32768U) ? 32768U : stack_bytes;
    if ( xTaskCreate(sys_task_main, name, stack, ctx, (UBaseType_t)prio, NULL) != pdPASS )
    {
        free(ctx);
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

void *khal_task_self(void) { return (void *)xTaskGetCurrentTaskHandle(); }

void *khal_task_slot_get(void)
{
    return pvTaskGetThreadLocalStoragePointer(NULL, (BaseType_t)KHAL_TASK_SLOT_INDEX);
}

void khal_task_slot_set(void *p)
{
    vTaskSetThreadLocalStoragePointer(NULL, (BaseType_t)KHAL_TASK_SLOT_INDEX, p);
}

void khal_task_slot_set_owned(void *p, khal_task_slot_free_t free_fn)
{
    /* The linux port has no deletion callbacks; a slot's owner is freed with the process. */
    (void)free_fn;
    khal_task_slot_set(p);
}

kode_status_t khal_watch_start(void (*pass)(void), uint32_t period_ms)
{
    (void)pass; (void)period_ms;
    return KODE_DENIED;
}

void khal_watch_rest(bool rest) { (void)rest; }

kode_status_t khal_chip_info(khal_chip_info_t *out)
{
    if ( NULL == out ) { return KODE_DENIED; }
    memset(out, 0, sizeof(*out));
    out->model = "ESP32-P4 (sim)";
    out->rev_major = 1U;
    out->flash_bytes = 32U * 1024U * 1024U;
    out->psram_bytes = SIM_HEAP_BYTES;
    static const uint8_t mac[KHAL_CHIP_MAC_LEN] = { 0x02, 0x4B, 0x4F, 0x44, 0x45, 0x01 };
    memcpy(out->mac, mac, sizeof(mac));
    return KODE_OK;
}

void *khal_alloc_psram(size_t align, size_t size)
{
    if ( align < sizeof(void *) ) { align = sizeof(void *); }
    void *p = NULL;
    if ( posix_memalign(&p, align, (size + align - 1U) & ~(align - 1U)) != 0 ) { return NULL; }
    memset(p, 0, size);
    return p;
}

void *khal_realloc_psram(void *p, size_t size) { return realloc(p, size); }

void khal_free(void *p) { free(p); }

/* klog asks whether it runs in a task; on the host the sim's "ISRs" are tasks too. */
int xPortCanYield(void)
{
    return 1;
}
