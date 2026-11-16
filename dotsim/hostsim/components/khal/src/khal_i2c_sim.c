/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_i2c_sim.c
 * @brief The board's I2C bus, simulated: a register file per chip, with the
 *        few registers that move (buttons, touch, LED, battery, USB, motion) modelled.
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

enum { C_IOEXP, C_TOUCH, C_LED, C_HAPTIC, C_CHARGER, C_GAUGE, C_IMU, C_MAG, C_COUNT };

static chip_t s_chip[C_COUNT];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_init;
static uint8_t s_tca_last;     /* input port as last read: INT is low while it differs */

/* What the page says about the battery, the USB port and the motion sensors
 * (under s_lock). The charger, gauge, IMU and magnetometer registers are
 * made from this when they are read, in the chips' own formats. */
static uint8_t s_bat_pct = 80U;
static bool s_bat_usb;
static int32_t s_motion[KDOTSIM_MOTION_N] = { 0, 0, 1000, 0, 0, 0, 200, 0, -400, 250 };

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

static int32_t clamp_i(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/** @brief value * num / den, rounded to the nearest. */
static int32_t scale_i(int32_t v, int32_t num, int32_t den)
{
    int64_t const p = (int64_t)v * num;
    int64_t const half = den / 2;
    return (int32_t)((p >= 0) ? ((p + half) / den) : ((p - half) / den));
}

/* The cell's voltage and current from the charge: 3.30 V empty to 4.20 V full,
 * a little higher while charging; 0.5 A in while charging, 0.12 A out otherwise. */
static bool bat_charging(void)
{
    return s_bat_usb && (s_bat_pct < 100U);
}

static int32_t bat_mv(void)
{
    int32_t const mv = 3300 + ((int32_t)s_bat_pct * 9) + (bat_charging() ? 60 : 0);
    return clamp_i(mv, 3000, 4200);
}

static void i16_be(uint8_t *p, int32_t v)
{
    uint16_t const u = (uint16_t)(int16_t)clamp_i(v, -32768, 32767);
    p[0] = (uint8_t)(u >> 8);
    p[1] = (uint8_t)(u & 0xFFU);
}

static void i16_le(uint8_t *p, int32_t v)
{
    uint16_t const u = (uint16_t)(int16_t)clamp_i(v, -32768, 32767);
    p[0] = (uint8_t)(u & 0xFFU);
    p[1] = (uint8_t)(u >> 8);
}

/* BQ25896 (kdrv_bq25896.c's table): REG0B status, REG0E..REG12 ADC, REG14 id. */
static void charger_regs(chip_t *c)
{
    uint8_t *r = c->reg;
    int32_t const mv = bat_mv();
    uint8_t const chrg = bat_charging() ? 2U : (s_bat_usb ? 3U : 0U);   /* fast / done / none */
    r[0x0B] = (uint8_t)((s_bat_usb ? (1U << 5) : 0U) | (chrg << 3) | (s_bat_usb ? (1U << 2) : 0U));
    r[0x0C] = 0U;
    r[0x0E] = (uint8_t)clamp_i((mv - 2304) / 20, 0, 127);               /* 20 mV a step from 2.304 V */
    r[0x0F] = (uint8_t)clamp_i((mv + 50 - 2304) / 20, 0, 127);
    r[0x10] = 0x20U;
    r[0x11] = s_bat_usb ? (uint8_t)(0x80U | (uint8_t)((5000 - 2600) / 100)) : 0U;   /* VBUS_GD, 100 mV a step from 2.6 V */
    r[0x12] = bat_charging() ? (uint8_t)(512 / 50) : 0U;                /* 50 mA a step */
    r[0x13] = 0U;
    r[0x14] = 0x02U;                                                    /* PN 0: bq25896 */
}

/* CW2217 (kdrv_cw2217.c): SOC in 1/256 %, VCELL in 312.5 uV, TEMP -40 C + raw/2, CURRENT through the shunt. */
static void gauge_regs(chip_t *c)
{
    uint8_t *r = c->reg;
    r[0x00] = 0xA0U;                                  /* CW221x */
    r[0xA7] = 0x0CU;                                  /* ready */
    i16_be(&r[0x04], (int32_t)s_bat_pct * 256);
    i16_be(&r[0x02], scale_i(bat_mv(), 16, 5));
    r[0x06] = (uint8_t)(25 * 2 + 80);                 /* 25 C */
    int32_t const ma = bat_charging() ? 500 : -120;
    int32_t const raw = scale_i(ma * 1000, 1, 160);   /* ~160 uA a step at 10 mOhm */
    i16_be(&r[0x0E], raw);
    i16_be(&r[0xA8], raw);
}

/* QMI8658 (kdrv_qmi8658.c): WHO_AM_I, then TEMP and the six words at 0x33; +-8 g is 4096 LSB/g,
 * +-512 dps is 64 LSB/dps, the temperature 256 LSB per degree. */
static void imu_regs(chip_t *c)
{
    uint8_t *r = c->reg;
    r[0x00] = 0x05U;
    r[0x01] = 0x7CU;
    i16_le(&r[0x33], scale_i(s_motion[9], 256, 10));
    for ( unsigned i = 0U; i < 3U; ++i )
    {
        i16_le(&r[0x35U + (2U * i)], scale_i(s_motion[i], 4096, 1000));
        i16_le(&r[0x3BU + (2U * i)], scale_i(s_motion[3U + i], 64, 1000));
    }
}

/* QMC5883P (kdrv_qmc5883p.c): chip id 0x80, status DRDY, X Y Z little endian at 0x01, 1 LSB = 1 mG. */
static void mag_regs(chip_t *c)
{
    uint8_t *r = c->reg;
    r[0x00] = 0x80U;
    r[0x09] = 0x01U;
    for ( unsigned i = 0U; i < 3U; ++i )
    {
        i16_le(&r[0x01U + (2U * i)], s_motion[6U + i]);
    }
}

void kdotsim_battery_set(uint8_t pct, bool usb)
{
    pthread_mutex_lock(&s_lock);
    s_bat_pct = (pct > 100U) ? 100U : pct;
    s_bat_usb = usb;
    pthread_mutex_unlock(&s_lock);
}

void kdotsim_battery_get(uint8_t *pct, bool *usb)
{
    pthread_mutex_lock(&s_lock);
    *pct = s_bat_pct;
    *usb = s_bat_usb;
    pthread_mutex_unlock(&s_lock);
}

void kdotsim_motion_set(const int32_t v[KDOTSIM_MOTION_N])
{
    pthread_mutex_lock(&s_lock);
    memcpy(s_motion, v, sizeof(s_motion));
    pthread_mutex_unlock(&s_lock);
}

void kdotsim_motion_get(int32_t v[KDOTSIM_MOTION_N])
{
    pthread_mutex_lock(&s_lock);
    memcpy(v, s_motion, sizeof(s_motion));
    pthread_mutex_unlock(&s_lock);
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
        s_chip[C_CHARGER] = (chip_t){ .addr = KBSP_I2C_ADDR_CHARGER, .present = true };
        s_chip[C_GAUGE] = (chip_t){ .addr = KBSP_I2C_ADDR_GAUGE, .present = true };
        s_chip[C_IMU] = (chip_t){ .addr = KBSP_I2C_ADDR_IMU, .present = true };
        s_chip[C_MAG] = (chip_t){ .addr = KBSP_I2C_ADDR_MAG, .present = true };
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
    else if ( c == &s_chip[C_CHARGER] ) { charger_regs(c); }
    else if ( c == &s_chip[C_GAUGE] )   { gauge_regs(c); }
    else if ( c == &s_chip[C_IMU] )     { imu_regs(c); }
    else if ( c == &s_chip[C_MAG] )     { mag_regs(c); }
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
