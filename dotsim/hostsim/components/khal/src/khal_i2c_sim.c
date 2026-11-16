/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_i2c_sim.c
 * @brief The board's I2C bus, simulated: a register file per chip, with the
 *        few registers that move (buttons, touch, LED) modelled.
 *
 * The SDK's own drivers (kdrv_tca6408a, kdrv_cst820, kdrv_led, ...) run on top
 * unchanged. A chip that is not modelled does not ACK, as an empty footprint.
 */
#include "khal_i2c.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "kbsp.h"
#include "kdotsim.h"

struct i2c_master_dev_t {
    uint16_t addr;
};

typedef struct {
    uint16_t addr;
    uint8_t reg[256];
    bool present;
} chip_t;

enum { C_IOEXP, C_TOUCH, C_LED, C_HAPTIC, C_COUNT };

static chip_t s_chip[C_COUNT];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_init;
static uint8_t s_tca_last;     /* input port as last read: INT is low while it differs */

/* The expander's input port from the buttons and the touch line (all active low). */
static uint8_t tca_inputs(void)
{
    uint32_t const b = kdotsim_buttons();
    uint8_t in = 0xFFU;
    if ( b & KDOTSIM_BTN_BOT )   { in &= (uint8_t)~(1U << KBSP_IOEXP_BIT_BTN_BOT); }
    if ( b & KDOTSIM_BTN_UP )    { in &= (uint8_t)~(1U << KBSP_IOEXP_BIT_PAD_UP); }
    if ( b & KDOTSIM_BTN_LEFT )  { in &= (uint8_t)~(1U << KBSP_IOEXP_BIT_PAD_LEFT); }
    if ( b & KDOTSIM_BTN_DOWN )  { in &= (uint8_t)~(1U << KBSP_IOEXP_BIT_PAD_DOWN); }
    if ( b & KDOTSIM_BTN_RIGHT ) { in &= (uint8_t)~(1U << KBSP_IOEXP_BIT_PAD_RIGHT); }
    uint16_t x, y;
    if ( kdotsim_touch(&x, &y) ) { in &= (uint8_t)~(1U << KBSP_IOEXP_BIT_TP_INT); }
    return in;
}

static void tca_int_update(void)
{
    /* Called with the lock held. */
    uint8_t const in = tca_inputs();
    s_chip[C_IOEXP].reg[0] = in;
    kdotsim_gpio_set(KBSP_PIN_IOEXP_INT, in == s_tca_last);
}

void kdotsim_inputs_changed(void)
{
    pthread_mutex_lock(&s_lock);
    if ( s_init )
    {
        tca_int_update();
    }
    pthread_mutex_unlock(&s_lock);
    kdotsim_gpio_set(KBSP_PIN_BUTTON_TOP, (kdotsim_buttons() & KDOTSIM_BTN_TOP) == 0U);
}

static void led_push(void)
{
    const uint8_t *r = s_chip[C_LED].reg;
    uint8_t const en = r[0x04];
    /* OUT1 green, OUT2 blue, OUT3 red (kdrv_led.c); ISET in 0.125 mA, 5 mA full scale. */
    unsigned g = ((en >> 0) & 3U) ? r[0x06] : 0U;
    unsigned b = ((en >> 2) & 3U) ? r[0x07] : 0U;
    unsigned rr = ((en >> 4) & 3U) ? r[0x08] : 0U;
    if ( 0x08U == (r[0x00] & 0x18U) )
    {
        g = b = rr = 0U;   /* asleep */
    }
#define LV(c) ((uint8_t)(((c) * 255U / 40U) > 255U ? 255U : ((c) * 255U / 40U)))
    uint32_t const rgb = ((uint32_t)LV(rr) << 16) | ((uint32_t)LV(g) << 8) | LV(b);
#undef LV
    static uint32_t last = 0xFFFFFFFFU;
    if ( rgb != last )
    {
        last = rgb;
        kdotsim_led((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
    }
}

static chip_t *find(uint16_t addr)
{
    for ( unsigned i = 0U; i < C_COUNT; ++i )
    {
        if ( s_chip[i].present && (s_chip[i].addr == addr) )
        {
            return &s_chip[i];
        }
    }
    return NULL;
}

kode_status_t khal_i2c_init(void)
{
    pthread_mutex_lock(&s_lock);
    if ( !s_init )
    {
        s_chip[C_IOEXP] = (chip_t){ .addr = KBSP_I2C_ADDR_IOEXP, .present = true };
        s_chip[C_IOEXP].reg[0x03] = 0xFFU;
        s_chip[C_TOUCH] = (chip_t){ .addr = KBSP_I2C_ADDR_TOUCH, .present = true };
        s_chip[C_TOUCH].reg[0xA7] = 0xB7U;   /* chip_id: CST820 */
        s_chip[C_TOUCH].reg[0xA8] = 0x01U;
        s_chip[C_TOUCH].reg[0xA9] = 0x02U;
        s_chip[C_LED] = (chip_t){ .addr = KBSP_I2C_ADDR_LED, .present = true };
        s_chip[C_HAPTIC] = (chip_t){ .addr = KBSP_I2C_ADDR_HAPTIC, .present = (getenv("KODE_SIM_HAPTIC") != NULL) };
        s_chip[C_HAPTIC].reg[0x57] = 0x23U;
        s_chip[C_HAPTIC].reg[0x69] = 0x30U;
        s_tca_last = 0xFFU;
        s_init = true;
        tca_int_update();
    }
    pthread_mutex_unlock(&s_lock);
    return KODE_OK;
}

struct i2c_master_bus_t *khal_i2c_bus(void)
{
    return NULL;
}

bool khal_i2c_probe(uint16_t addr, uint32_t timeout_ms)
{
    (void)timeout_ms;
    (void)khal_i2c_init();
    pthread_mutex_lock(&s_lock);
    bool const ok = (find(addr) != NULL);
    pthread_mutex_unlock(&s_lock);
    return ok;
}

kode_status_t khal_i2c_add(uint16_t addr, khal_i2c_dev_t *out)
{
    if ( NULL == out )
    {
        return KODE_DENIED;
    }
    struct i2c_master_dev_t *d = calloc(1, sizeof(*d));
    if ( NULL == d )
    {
        return KODE_NO_RESOURCES;
    }
    d->addr = addr;
    *out = d;
    return KODE_OK;
}

kode_status_t khal_i2c_remove(khal_i2c_dev_t dev)
{
    free(dev);
    return KODE_OK;
}

static void touch_frame(chip_t *c)
{
    uint16_t x = 0U, y = 0U;
    bool const down = kdotsim_touch(&x, &y);
    c->reg[0x01] = 0U;                       /* gesture */
    c->reg[0x02] = down ? 1U : 0U;           /* fingers */
    c->reg[0x03] = (uint8_t)((x >> 8) & 0x0FU);
    c->reg[0x04] = (uint8_t)(x & 0xFFU);
    c->reg[0x05] = (uint8_t)((y >> 8) & 0x0FU);
    c->reg[0x06] = (uint8_t)(y & 0xFFU);
}

kode_status_t khal_i2c_read_reg(khal_i2c_dev_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    if ( (NULL == dev) || (NULL == buf) )
    {
        return KODE_DENIED;
    }
    pthread_mutex_lock(&s_lock);
    chip_t *const c = find(dev->addr);
    if ( NULL == c )
    {
        pthread_mutex_unlock(&s_lock);
        return KODE_ERROR;
    }
    if ( c == &s_chip[C_TOUCH] )
    {
        touch_frame(c);
    }
    if ( c == &s_chip[C_IOEXP] )
    {
        c->reg[0] = tca_inputs();
    }
    for ( size_t i = 0U; i < len; ++i )
    {
        buf[i] = c->reg[(uint8_t)(reg + i)];
    }
    if ( (c == &s_chip[C_IOEXP]) && (0U == reg) )
    {
        /* Reading the input port clears the expander's interrupt. */
        s_tca_last = c->reg[0];
        tca_int_update();
    }
    pthread_mutex_unlock(&s_lock);
    return KODE_OK;
}

kode_status_t khal_i2c_write_regs(khal_i2c_dev_t dev, uint8_t reg, const uint8_t *buf, size_t len)
{
    if ( (NULL == dev) || ((NULL == buf) && (0U != len)) )
    {
        return KODE_DENIED;
    }
    pthread_mutex_lock(&s_lock);
    chip_t *const c = find(dev->addr);
    if ( NULL == c )
    {
        pthread_mutex_unlock(&s_lock);
        return KODE_ERROR;
    }
    for ( size_t i = 0U; i < len; ++i )
    {
        uint8_t const r = (uint8_t)(reg + i);
        if ( (c == &s_chip[C_IOEXP]) && (0U == r) )
        {
            continue;   /* read-only input port */
        }
        c->reg[r] = buf[i];
    }
    bool const led = (c == &s_chip[C_LED]);
    if ( led && (0U == reg) && (len > 0U) && (0x07U == (buf[0] & 0x07U)) )
    {
        memset(c->reg, 0, sizeof(c->reg));   /* full reset */
    }
    pthread_mutex_unlock(&s_lock);
    if ( led )
    {
        led_push();
    }
    return KODE_OK;
}

kode_status_t khal_i2c_write_reg(khal_i2c_dev_t dev, uint8_t reg, uint8_t val)
{
    return khal_i2c_write_regs(dev, reg, &val, 1U);
}
