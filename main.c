/*
 * SeeTrue Task 1: "Eye Ring" NIR illuminator controller
 * Target: Raspberry Pi Pico (RP2040), Pico C SDK
 *
 * Two-board design. This firmware runs on the controller board, which
 * carries the Pico (U1) and a TLC5947 24-channel constant-current driver
 * (U$1). The dumb ring board holds the six 850 nm NIR LEDs and connects
 * back over an 8-pin 0.1" header cable: +5V to the LED anodes, six cathode
 * lines to TLC5947 outputs OUT0..OUT5, and ground.
 *
 * Pin map (matches the final schematic, do not change without changing the
 * board):
 *   GP2  SCLK   -> TLC5947 SCLK   (SPI0 SCK)
 *   GP3  SIN    -> TLC5947 SIN    (SPI0 TX)
 *   GP4  XLAT   -> TLC5947 XLAT   (latch, plain GPIO)
 *   GP5  BLANK  -> TLC5947 BLANK  (high = all outputs off, plain GPIO)
 *   GP0  SYNC   -> JP1 sync header (PWM square wave, 10..100 Hz)
 *
 * The TLC5947 has an internal grayscale oscillator, so there is no external
 * GSCLK pin. We shift 288 bits (24 channels x 12 bit) over SPI, pulse XLAT
 * to latch, and the chip handles the PWM dimming itself.
 *
 * USB CDC serial, line-based ASCII protocol (1 command per line):
 *   IDN?              -> prints "SEETRUE-RING v1.0"
 *   LED <1-6> <0-100> -> set one LED intensity in percent
 *   LED ALL <0-100>   -> set all six LEDs
 *   ON <1-6>          -> enable one LED at its stored intensity
 *   OFF <1-6>         -> disable one LED (intensity held, output off)
 *   SYNC <10-100>     -> enable sync output at the given frequency in Hz
 *   SYNC OFF          -> disable sync output
 *   GET               -> print current parameters
 *   SAVE              -> persist current parameters to flash
 * Every command replies with "OK" or "ERR <reason>".
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/pwm.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

/* ---------------- pin map ---------------- */
#define PIN_SCLK   2   /* SPI0 SCK  -> TLC5947 SCLK */
#define PIN_SIN    3   /* SPI0 TX   -> TLC5947 SIN  */
#define PIN_XLAT   4   /* TLC5947 latch */
#define PIN_BLANK  5   /* TLC5947 blank, high = all off */
#define PIN_SYNC   0   /* sync output to JP1 */

#define NUM_LEDS      6
#define TLC_CHANNELS  24
#define SPI_HZ        (4 * 1000 * 1000)   /* 4 MHz, well within TLC5947 limits */

/* last 4 kB sector of a 2 MB flash */
#define PARAM_FLASH_OFFSET  (2 * 1024 * 1024 - FLASH_SECTOR_SIZE)
#define PARAM_MAGIC 0x53545231u   /* "STR1" */

/* ---------------- persistent parameters ---------------- */
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  intensity[NUM_LEDS]; /* 0..100 */
    uint8_t  enabled[NUM_LEDS];   /* 0/1 */
    uint8_t  sync_on;             /* 0/1 */
    uint8_t  sync_hz;             /* 10..100 */
    uint32_t crc;
} params_t;

static params_t params;

/* ---------------- CRC32 (for flash validity check) ---------------- */
static uint32_t crc32_calc(const uint8_t *d, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static void params_defaults(void)
{
    memset(&params, 0, sizeof params);
    params.magic   = PARAM_MAGIC;
    params.version = 1;
    params.sync_hz = 30;   /* sensible default inside 10..100 */
    /* all LEDs off, sync off */
}

static void params_load(void)
{
    const params_t *p = (const params_t *)(XIP_BASE + PARAM_FLASH_OFFSET);
    uint32_t crc = crc32_calc((const uint8_t *)p, offsetof(params_t, crc));
    if (p->magic == PARAM_MAGIC && p->crc == crc)
        params = *p;
    else
        params_defaults();
}

static void params_save(void)
{
    static uint8_t page[FLASH_PAGE_SIZE];
    params.crc = crc32_calc((const uint8_t *)&params, offsetof(params_t, crc));
    memset(page, 0xFF, sizeof page);
    memcpy(page, &params, sizeof params);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(PARAM_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(PARAM_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
}

/* ---------------- TLC5947 ---------------- */
/*
 * 24 channels x 12 bit = 288 bits = 36 bytes, shifted MSB first with
 * channel 23 first and channel 0 last. gs[] is indexed by channel number.
 */
static void tlc_write(const uint16_t gs[TLC_CHANNELS])
{
    uint8_t buf[36];
    memset(buf, 0, sizeof buf);

    int idx = 0;
    for (int ch = TLC_CHANNELS - 1; ch >= 0; ch--) {
        uint16_t v = gs[ch] & 0x0FFF;
        for (int b = 11; b >= 0; b--) {
            if (v & (1u << b))
                buf[idx >> 3] |= (uint8_t)(0x80 >> (idx & 7));
            idx++;
        }
    }

    spi_write_blocking(spi0, buf, sizeof buf);

    gpio_put(PIN_XLAT, 1);
    sleep_us(1);
    gpio_put(PIN_XLAT, 0);
}

/* Map percent (0..100) to 12-bit grayscale (0..4095). */
static inline uint16_t pct_to_gs(uint8_t pct)
{
    if (pct > 100) pct = 100;
    return (uint16_t)((pct * 4095u) / 100u);
}

/* Push the current params to the driver. LEDs 1..6 -> OUT0..OUT5. */
static void leds_apply(void)
{
    uint16_t gs[TLC_CHANNELS];
    memset(gs, 0, sizeof gs);
    for (int i = 0; i < NUM_LEDS; i++)
        gs[i] = params.enabled[i] ? pct_to_gs(params.intensity[i]) : 0;
    tlc_write(gs);
}

/* ---------------- sync output (PWM on GP0) ---------------- */
/*
 * Fix the wrap at 62500 counts. Then clkdiv = f_sys / (hz * 62500)
 * = 125e6 / (hz * 62500) = 2000 / hz, which stays in [20, 200] for
 * hz in [10, 100], comfortably inside the valid clkdiv range.
 * 50% duty is level = wrap/2.
 */
static void sync_apply(void)
{
    uint slice = pwm_gpio_to_slice_num(PIN_SYNC);
    uint chan  = pwm_gpio_to_channel(PIN_SYNC);

    if (!params.sync_on) {
        pwm_set_enabled(slice, false);
        gpio_set_function(PIN_SYNC, GPIO_FUNC_SIO);
        gpio_set_dir(PIN_SYNC, GPIO_OUT);
        gpio_put(PIN_SYNC, 0);
        return;
    }

    uint8_t hz = params.sync_hz;
    if (hz < 10)  hz = 10;
    if (hz > 100) hz = 100;

    gpio_set_function(PIN_SYNC, GPIO_FUNC_PWM);
    pwm_set_clkdiv(slice, 2000.0f / (float)hz);
    pwm_set_wrap(slice, 62499);
    pwm_set_chan_level(slice, chan, 31250);
    pwm_set_enabled(slice, true);
}

/* ---------------- hardware init ---------------- */
static void hw_init(void)
{
    /* SPI0 for the TLC5947 */
    spi_init(spi0, SPI_HZ);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_SCLK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SIN,  GPIO_FUNC_SPI);

    /* XLAT low idle */
    gpio_init(PIN_XLAT);
    gpio_set_dir(PIN_XLAT, GPIO_OUT);
    gpio_put(PIN_XLAT, 0);

    /* BLANK high at boot so outputs stay dark until we are ready.
     * There is also a 10k pull-up (R1) on the board for the window
     * before this line runs. */
    gpio_init(PIN_BLANK);
    gpio_set_dir(PIN_BLANK, GPIO_OUT);
    gpio_put(PIN_BLANK, 1);

    /* SYNC starts as a driven-low GPIO; sync_apply() reconfigures it. */
    gpio_init(PIN_SYNC);
    gpio_set_dir(PIN_SYNC, GPIO_OUT);
    gpio_put(PIN_SYNC, 0);
}

/* ---------------- command parsing ---------------- */
static void print_params(void)
{
    printf("intensity:");
    for (int i = 0; i < NUM_LEDS; i++) printf(" %d", params.intensity[i]);
    printf("\nenabled:");
    for (int i = 0; i < NUM_LEDS; i++) printf(" %d", params.enabled[i]);
    printf("\nsync: %s %d Hz\n",
           params.sync_on ? "on" : "off", params.sync_hz);
}

/* returns 1 if the LED index token (1..6) is valid, storing 0-based idx */
static int parse_led_index(const char *tok, int *idx)
{
    char *end;
    long n = strtol(tok, &end, 10);
    if (*end != '\0' || n < 1 || n > NUM_LEDS) return 0;
    *idx = (int)(n - 1);
    return 1;
}

static void handle_line(char *line)
{
    /* uppercase the verb region; tokenize on spaces */
    char *tok = strtok(line, " \t");
    if (!tok) return;

    for (char *p = tok; *p; p++) *p = (char)toupper((unsigned char)*p);

    if (strcmp(tok, "IDN?") == 0) {
        printf("SEETRUE-RING v1.0\n");
        return;
    }

    if (strcmp(tok, "LED") == 0) {
        char *a = strtok(NULL, " \t");
        char *b = strtok(NULL, " \t");
        if (!a || !b) { printf("ERR usage: LED <1-6|ALL> <0-100>\n"); return; }

        char au[8]; strncpy(au, a, sizeof au - 1); au[sizeof au - 1] = 0;
        for (char *p = au; *p; p++) *p = (char)toupper((unsigned char)*p);

        char *end;
        long val = strtol(b, &end, 10);
        if (*end != '\0' || val < 0 || val > 100) {
            printf("ERR intensity 0..100\n"); return;
        }

        if (strcmp(au, "ALL") == 0) {
            for (int i = 0; i < NUM_LEDS; i++) {
                params.intensity[i] = (uint8_t)val;
                params.enabled[i]   = (val > 0);
            }
        } else {
            int idx;
            if (!parse_led_index(a, &idx)) { printf("ERR led 1..6\n"); return; }
            params.intensity[idx] = (uint8_t)val;
            params.enabled[idx]   = (val > 0);
        }
        leds_apply();
        printf("OK\n");
        return;
    }

    if (strcmp(tok, "ON") == 0 || strcmp(tok, "OFF") == 0) {
        int on = (tok[1] == 'N');
        char *a = strtok(NULL, " \t");
        int idx;
        if (!a || !parse_led_index(a, &idx)) { printf("ERR led 1..6\n"); return; }
        params.enabled[idx] = on ? 1 : 0;
        leds_apply();
        printf("OK\n");
        return;
    }

    if (strcmp(tok, "SYNC") == 0) {
        char *a = strtok(NULL, " \t");
        if (!a) { printf("ERR usage: SYNC <10-100|OFF>\n"); return; }
        char au[8]; strncpy(au, a, sizeof au - 1); au[sizeof au - 1] = 0;
        for (char *p = au; *p; p++) *p = (char)toupper((unsigned char)*p);

        if (strcmp(au, "OFF") == 0) {
            params.sync_on = 0;
        } else {
            char *end;
            long hz = strtol(a, &end, 10);
            if (*end != '\0' || hz < 10 || hz > 100) {
                printf("ERR sync 10..100 Hz or OFF\n"); return;
            }
            params.sync_hz = (uint8_t)hz;
            params.sync_on = 1;
        }
        sync_apply();
        printf("OK\n");
        return;
    }

    if (strcmp(tok, "GET") == 0) { print_params(); printf("OK\n"); return; }

    if (strcmp(tok, "SAVE") == 0) { params_save(); printf("OK\n"); return; }

    printf("ERR unknown command\n");
}

/* ---------------- main ---------------- */
int main(void)
{
    stdio_init_all();
    hw_init();
    params_load();

    /* apply stored state, then release BLANK so enabled LEDs light */
    leds_apply();
    sync_apply();
    gpio_put(PIN_BLANK, 0);

    char line[128];
    int len = 0;

    while (true) {
        int c = getchar_timeout_us(1000);
        if (c == PICO_ERROR_TIMEOUT) continue;

        if (c == '\r' || c == '\n') {
            if (len > 0) {
                line[len] = '\0';
                handle_line(line);
                len = 0;
            }
        } else if (len < (int)sizeof line - 1) {
            line[len++] = (char)c;
        } else {
            /* overrun: reset the buffer and report */
            len = 0;
            printf("ERR line too long\n");
        }
    }
    return 0;
}
