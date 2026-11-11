/*
 * LoRa radio (SX1262) and GNSS (ATGM336H) of the M5Stack Cap LoRa-1262, for apps.
 *
 * Wiring on the Cardputer ADV EXT header: SCK 40, MISO 39, MOSI 14 (SPI3, shared with the
 * microSD card), NSS 5, RESET 3, BUSY 6, DIO1 4. DIO2 drives the RF switch, DIO3 powers a 1.8 V
 * TCXO (tried first; a crystal is used if the TCXO does not start). GNSS on UART1: TX 13, RX 15.
 * The cap also has a PI4IOE5V6408 I/O expander (I2C 0x43 on the internal bus, 8/9): its P0 output
 * enables the radio's antenna path and must be driven high before the SX1262 is used (as the
 * Meshtastic firmware does in its Cardputer ADV variant).
 *
 * The radio is started when an app with the "lora" permission asks for it and stopped (radio put
 * to sleep, pins held high again so the SD bus stays clean) when the app ends. One OS task owns the
 * chip: it reads packets into a queue, sends queued packets, and keeps the receiver on otherwise.
 */
#include "app_ctx.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hal.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "lora";

#define PIN_LORA_NSS  5
#define PIN_LORA_RST  3
#define PIN_LORA_BUSY 6
#define PIN_LORA_DIO1 4
#define PIN_GNSS_TX   13
#define PIN_GNSS_RX   15
#define GNSS_UART     UART_NUM_1

/* SX126x commands */
#define CMD_SET_SLEEP           0x84
#define CMD_SET_STANDBY         0x80
#define CMD_SET_TX              0x83
#define CMD_SET_RX              0x82
#define CMD_SET_REGULATOR_MODE  0x96
#define CMD_CALIBRATE           0x89
#define CMD_CALIBRATE_IMAGE     0x98
#define CMD_SET_PA_CONFIG       0x95
#define CMD_SET_DIO_IRQ_PARAMS  0x08
#define CMD_GET_IRQ_STATUS      0x12
#define CMD_CLEAR_IRQ_STATUS    0x02
#define CMD_SET_DIO2_RF_SWITCH  0x9D
#define CMD_SET_DIO3_TCXO       0x97
#define CMD_SET_RF_FREQUENCY    0x86
#define CMD_SET_PACKET_TYPE     0x8A
#define CMD_SET_TX_PARAMS       0x8E
#define CMD_SET_MODULATION      0x8B
#define CMD_SET_PACKET_PARAMS   0x8C
#define CMD_GET_RX_BUFFER_STATUS 0x13
#define CMD_GET_PACKET_STATUS   0x14
#define CMD_GET_DEVICE_ERRORS   0x17
#define CMD_CLEAR_DEVICE_ERRORS 0x07
#define CMD_SET_BUFFER_BASE     0x8F
#define CMD_WRITE_REGISTER      0x0D
#define CMD_READ_REGISTER       0x1D
#define CMD_WRITE_BUFFER        0x0E
#define CMD_READ_BUFFER         0x1E

#define REG_SYNC_WORD     0x0740
#define REG_IQ_POLARITY   0x0736
#define REG_TX_MODULATION 0x0889
#define REG_OCP           0x08E7
#define REG_RX_GAIN       0x08AC

#define IRQ_TX_DONE      0x0001
#define IRQ_RX_DONE      0x0002
#define IRQ_PREAMBLE     0x0004
#define IRQ_HEADER_VALID 0x0010
#define IRQ_HEADER_ERR   0x0020
#define IRQ_CRC_ERR      0x0040
#define IRQ_TIMEOUT      0x0200

#define RX_Q 8
#define TX_Q 8
#define TX_TIMEOUT_MS 6000
#define BUSY_MAX_MS 3000 /* a header seen this long ago without a packet is stale */

typedef struct { uint8_t len; uint8_t data[255]; } txpkt_t;

static spi_device_handle_t s_spi;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;     /* protects the queues and the config */
static volatile int s_run, s_up, s_task_done;
static volatile int s_rx_header;     /* a header was seen, packet not finished yet */
static volatile uint32_t s_header_ms;
static int s_tcxo;
static cpos_lora_cfg_t s_cfg;
static volatile int s_reconfig;
static cpos_lora_pkt_t *s_rxq;       /* RX_Q entries, allocated while running */
static int s_rx_head, s_rx_n;
static txpkt_t *s_txq;               /* TX_Q entries */
static int s_tx_head, s_tx_n;
static volatile int s_txing;
static uint32_t s_tx_start, s_tx_air;
static cpos_lora_stats_t s_stats;
static int s_gnss_up;

static char s_diag[200];             /* what the last begin() saw, for the app to show */
static int s_gnss_swap, s_gnss_seen;
static uint32_t s_gnss_try_at;

static uint8_t s_tx[264] __attribute__((aligned(4)));
static uint8_t s_rx[264] __attribute__((aligned(4)));

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- low level ------------------------------------------------------------------------- */
static int wait_busy(uint32_t ms) {
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (gpio_get_level(PIN_LORA_BUSY)) {
        if (esp_timer_get_time() > end) return -1;
        vTaskDelay(1);
    }
    return 0;
}

/* one SPI transaction: n bytes out of s_tx, n bytes back into s_rx */
static int xfer(size_t n) {
    if (wait_busy(200)) return -1;
    spi_transaction_t t = {.length = n * 8, .tx_buffer = s_tx, .rx_buffer = s_rx};
    return spi_device_polling_transmit(s_spi, &t) == ESP_OK ? 0 : -1;
}

static int cmd(uint8_t op, const uint8_t *p, size_t n) {
    s_tx[0] = op;
    if (n) memcpy(s_tx + 1, p, n);
    return xfer(1 + n);
}

/* read commands: opcode, status byte, then n data bytes */
static int cmd_read(uint8_t op, uint8_t *out, size_t n) {
    memset(s_tx, 0, 2 + n);
    s_tx[0] = op;
    if (xfer(2 + n)) return -1;
    memcpy(out, s_rx + 2, n);
    return 0;
}

static int reg_write(uint16_t addr, const uint8_t *v, size_t n) {
    s_tx[0] = CMD_WRITE_REGISTER;
    s_tx[1] = addr >> 8;
    s_tx[2] = addr & 0xFF;
    memcpy(s_tx + 3, v, n);
    return xfer(3 + n);
}

static int reg_read(uint16_t addr, uint8_t *v, size_t n) {
    memset(s_tx, 0, 4 + n);
    s_tx[0] = CMD_READ_REGISTER;
    s_tx[1] = addr >> 8;
    s_tx[2] = addr & 0xFF;
    if (xfer(4 + n)) return -1;
    memcpy(v, s_rx + 4, n);
    return 0;
}

static void reg_update(uint16_t addr, uint8_t clear, uint8_t set) {
    uint8_t v = 0;
    if (reg_read(addr, &v, 1) == 0) {
        v = (v & ~clear) | set;
        reg_write(addr, &v, 1);
    }
}

static uint16_t irq_status(void) {
    uint8_t b[2] = {0};
    if (cmd_read(CMD_GET_IRQ_STATUS, b, 2)) return 0;
    return (uint16_t)(b[0] << 8 | b[1]);
}

static void irq_clear(uint16_t m) {
    uint8_t p[2] = {m >> 8, m & 0xFF};
    cmd(CMD_CLEAR_IRQ_STATUS, p, 2);
}

static void hw_reset(void) {
    gpio_set_level(PIN_LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    wait_busy(200);
}

static void standby(void) {
    uint8_t p = 0x00; /* STDBY_RC */
    cmd(CMD_SET_STANDBY, &p, 1);
}

/* ---- configuration --------------------------------------------------------------------- */
static int bw_code(uint32_t bw) {
    static const struct { uint32_t hz; uint8_t code; } t[] = {
        {7800, 0x00}, {10400, 0x08}, {15600, 0x01}, {20800, 0x09}, {31250, 0x02},
        {41700, 0x0A}, {62500, 0x03}, {125000, 0x04}, {250000, 0x05}, {500000, 0x06}};
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (bw >= t[i].hz - t[i].hz / 50 && bw <= t[i].hz + t[i].hz / 50) return t[i].code;
    return -1;
}

static int cfg_ok(const cpos_lora_cfg_t *c) {
    return c && c->freq_hz >= 150000000u && c->freq_hz <= 960000000u && bw_code(c->bw_hz) >= 0 && c->sf >= 5 &&
           c->sf <= 12 && c->cr >= 5 && c->cr <= 8 && c->power_dbm >= -9 && c->power_dbm <= 22 && c->preamble >= 1;
}

static void packet_params(uint8_t len) {
    uint8_t p[6] = {s_cfg.preamble >> 8, s_cfg.preamble & 0xFF, 0x00 /* explicit header */, len,
                    s_cfg.crc ? 1 : 0, s_cfg.iq_invert ? 1 : 0};
    cmd(CMD_SET_PACKET_PARAMS, p, 6);
}

static void apply_cfg(void) {
    standby();
    /* image calibration for the band in use */
    uint8_t cal[2];
    uint32_t f = s_cfg.freq_hz / 1000000u;
    if (f >= 902) { cal[0] = 0xE1; cal[1] = 0xE9; }
    else if (f >= 863) { cal[0] = 0xD7; cal[1] = 0xDB; }
    else if (f >= 779) { cal[0] = 0xC1; cal[1] = 0xC5; }
    else if (f >= 470) { cal[0] = 0x75; cal[1] = 0x81; }
    else { cal[0] = 0x6B; cal[1] = 0x6F; }
    cmd(CMD_CALIBRATE_IMAGE, cal, 2);
    wait_busy(100);

    uint32_t frf = (uint32_t)(((uint64_t)s_cfg.freq_hz << 25) / 32000000u);
    uint8_t fp[4] = {frf >> 24, frf >> 16, frf >> 8, frf};
    cmd(CMD_SET_RF_FREQUENCY, fp, 4);

    uint8_t pa[4] = {0x04, 0x07, 0x00, 0x01}; /* SX1262, up to +22 dBm */
    cmd(CMD_SET_PA_CONFIG, pa, 4);
    uint8_t tp[2] = {(uint8_t)s_cfg.power_dbm, 0x04 /* 200 us ramp */};
    cmd(CMD_SET_TX_PARAMS, tp, 2);
    uint8_t ocp = 0x38; /* 140 mA */
    reg_write(REG_OCP, &ocp, 1);

    /* low data rate optimisation when a symbol lasts 16 ms or more */
    uint32_t sym_us = (uint32_t)(((uint64_t)1000000u << s_cfg.sf) / s_cfg.bw_hz);
    uint8_t mp[4] = {s_cfg.sf, (uint8_t)bw_code(s_cfg.bw_hz), (uint8_t)(s_cfg.cr - 4), sym_us >= 16000 ? 1 : 0};
    cmd(CMD_SET_MODULATION, mp, 4);
    packet_params(255);

    uint8_t sw[2] = {(uint8_t)((s_cfg.sync_word & 0xF0) | 0x04), (uint8_t)(((s_cfg.sync_word & 0x0F) << 4) | 0x04)};
    reg_write(REG_SYNC_WORD, sw, 2);
    /* datasheet errata 15.1 (500 kHz modulation quality) and 15.4 (IQ polarity) */
    if (bw_code(s_cfg.bw_hz) == 0x06) reg_update(REG_TX_MODULATION, 0x04, 0);
    else reg_update(REG_TX_MODULATION, 0, 0x04);
    if (s_cfg.iq_invert) reg_update(REG_IQ_POLARITY, 0x04, 0);
    else reg_update(REG_IQ_POLARITY, 0, 0x04);
    uint8_t gain = 0x96; /* boosted receiver gain */
    reg_write(REG_RX_GAIN, &gain, 1);
}

static void start_rx(void) {
    packet_params(255);
    irq_clear(0xFFFF);
    uint8_t t[3] = {0xFF, 0xFF, 0xFF}; /* continuous */
    cmd(CMD_SET_RX, t, 3);
    s_rx_header = 0;
}

/* bring the chip up; tcxo: try with the 1.8 V TCXO on DIO3 */
/* ---- the cap's I/O expander ----------------------------------------------------------- */
#define PI4IO_ADDR 0x43
#define PI4IO_REG_IO_DIR 0x03
#define PI4IO_REG_OUT_SET 0x05
#define PI4IO_REG_OUT_H_IM 0x07
static i2c_master_dev_handle_t s_pi4io;

static int pi4io_write(uint8_t reg, uint8_t v) {
    uint8_t b[2] = {reg, v};
    return i2c_master_transmit(s_pi4io, b, 2, 50) == ESP_OK ? 0 : -1;
}

/* P0 high: radio on. Returns 0 when the expander answered. */
static int cap_enable(void) {
    if (!s_pi4io) {
        if (i2c_master_probe(hal_i2c(), PI4IO_ADDR, 50) != ESP_OK) {
            ESP_LOGW(TAG, "no I/O expander at 0x%02x: is the Cap LoRa-1262 attached?", PI4IO_ADDR);
            return -1;
        }
        i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = PI4IO_ADDR, .scl_speed_hz = 400000};
        if (i2c_master_bus_add_device(hal_i2c(), &dc, &s_pi4io) != ESP_OK) { s_pi4io = NULL; return -1; }
    }
    int rc = pi4io_write(PI4IO_REG_IO_DIR, 0x01);       /* P0 output */
    rc |= pi4io_write(PI4IO_REG_OUT_H_IM, 0x00);        /* outputs driven, not high impedance */
    rc |= pi4io_write(PI4IO_REG_OUT_SET, 0x01);         /* P0 high */
    vTaskDelay(pdMS_TO_TICKS(10));
    return rc ? -1 : 0;
}

static void cap_disable(void) {
    if (!s_pi4io) return;
    pi4io_write(PI4IO_REG_OUT_SET, 0x00);
    i2c_master_bus_rm_device(s_pi4io);
    s_pi4io = NULL;
}

/* tcxo: power the 1.8 V TCXO from DIO3. force: carry on even if the oscillator reports a start error. */
static int chip_init(int tcxo, int force) {
    hw_reset();
    uint8_t sw[2] = {0};
    int busy = gpio_get_level(PIN_LORA_BUSY);
    int rr = reg_read(REG_SYNC_WORD, sw, 2);
    size_t dl = strlen(s_diag);
    snprintf(s_diag + dl, sizeof(s_diag) - dl, " reg:%02x%02x%s busy:%d", sw[0], sw[1], rr ? "(timeout)" : "", busy);
    if (rr || !((sw[0] == 0x14 && sw[1] == 0x24) || (sw[0] == 0x34 && sw[1] == 0x44))) {
        ESP_LOGW(TAG, "SX1262 sync word reads %02x %02x (busy %d)", sw[0], sw[1], busy);
        return -2; /* nothing (or not an SX126x) on the bus */
    }
    standby();
    /* XOSC_START_ERR is set at power-up on boards with a TCXO (it was not powered yet): clear it before
       configuring the TCXO, as RadioLib does, or the check below sees a stale error */
    uint8_t z[2] = {0, 0};
    cmd(CMD_CLEAR_DEVICE_ERRORS, z, 2);
    if (tcxo) {
        uint8_t p[4] = {0x02 /* 1.8 V */, 0x00, 0x01, 0x40 /* 5 ms */};
        cmd(CMD_SET_DIO3_TCXO, p, 4);
    }
    uint8_t all = 0x7F;
    cmd(CMD_CALIBRATE, &all, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (wait_busy(100)) return -1;
    uint8_t err[2] = {0};
    cmd_read(CMD_GET_DEVICE_ERRORS, err, 2);
    if (err[0] || err[1]) {
        size_t l = strlen(s_diag);
        snprintf(s_diag + l, sizeof(s_diag) - l, " err:%02x%02x", err[0], err[1]);
        cmd(CMD_CLEAR_DEVICE_ERRORS, z, 2);
    }
    if ((err[1] & 0x20) && !force) return -1; /* XOSC_START_ERR: try the other oscillator */
    standby(); /* back to a clean STDBY_RC after the error clear */
    uint8_t lora = 0x01;
    cmd(CMD_SET_PACKET_TYPE, &lora, 1);
    uint8_t dcdc = 0x01;
    cmd(CMD_SET_REGULATOR_MODE, &dcdc, 1);
    uint8_t one = 0x01;
    cmd(CMD_SET_DIO2_RF_SWITCH, &one, 1);
    uint8_t base[2] = {0, 0};
    cmd(CMD_SET_BUFFER_BASE, base, 2);
    uint16_t mask = IRQ_TX_DONE | IRQ_RX_DONE | IRQ_PREAMBLE | IRQ_HEADER_VALID | IRQ_HEADER_ERR | IRQ_CRC_ERR | IRQ_TIMEOUT;
    uint16_t dio1 = IRQ_TX_DONE | IRQ_RX_DONE | IRQ_HEADER_VALID | IRQ_HEADER_ERR | IRQ_CRC_ERR | IRQ_TIMEOUT;
    uint8_t ip[8] = {mask >> 8, mask & 0xFF, dio1 >> 8, dio1 & 0xFF, 0, 0, 0, 0};
    cmd(CMD_SET_DIO_IRQ_PARAMS, ip, 8);
    apply_cfg();
    return 0;
}

/* time on air in ms (Semtech AN1200.13), explicit header */
static uint32_t airtime_ms(size_t len) {
    uint32_t sf = s_cfg.sf;
    int ldro = (((uint64_t)1000000u << sf) / s_cfg.bw_hz) >= 16000;
    int32_t num = 8 * (int32_t)len - 4 * (int32_t)sf + 28 + (s_cfg.crc ? 16 : 0);
    int32_t den = 4 * ((int32_t)sf - (ldro ? 2 : 0));
    int32_t n = num > 0 ? (num + den - 1) / den : 0;
    uint32_t symbols = s_cfg.preamble + 5 + 8 + n * (s_cfg.cr);  /* 4.25 + 8 rounded up */
    return (uint32_t)(((uint64_t)symbols << sf) * 1000u / s_cfg.bw_hz);
}

/* ---- the radio task -------------------------------------------------------------------- */
static void IRAM_ATTR dio1_isr(void *arg) {
    BaseType_t woken = pdFALSE;
    if (s_task) vTaskNotifyGiveFromISR(s_task, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void read_packet(void) {
    uint8_t bs[2] = {0}, ps[3] = {0};
    if (cmd_read(CMD_GET_RX_BUFFER_STATUS, bs, 2) || cmd_read(CMD_GET_PACKET_STATUS, ps, 3)) return;
    uint8_t len = bs[0];
    cpos_lora_pkt_t *p = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_rx_n < RX_Q) p = &s_rxq[(s_rx_head + s_rx_n) % RX_Q];
    xSemaphoreGive(s_lock);
    if (!p) { s_stats.rx_bad++; return; } /* app not keeping up: drop */
    memset(s_tx, 0, 3 + len);
    s_tx[0] = CMD_READ_BUFFER;
    s_tx[1] = bs[1];
    if (xfer(3 + len)) return;
    p->len = len;
    memcpy(p->data, s_rx + 3, len);
    p->rssi = -(int16_t)ps[0] / 2;
    p->snr4 = (int8_t)ps[1];
    p->ms = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_rx_n++;
    xSemaphoreGive(s_lock);
    s_stats.rx_ok++;
    s_stats.rx_air_ms += airtime_ms(len);
}

static void start_tx(void) {
    txpkt_t t;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    t = s_txq[s_tx_head];
    xSemaphoreGive(s_lock);
    standby();
    packet_params(t.len);
    s_tx[0] = CMD_WRITE_BUFFER;
    s_tx[1] = 0;
    memcpy(s_tx + 2, t.data, t.len);
    xfer(2 + t.len);
    irq_clear(0xFFFF);
    uint32_t to = TX_TIMEOUT_MS * 64; /* 15.625 us steps */
    uint8_t p[3] = {to >> 16, to >> 8, to};
    cmd(CMD_SET_TX, p, 3);
    s_txing = 1;
    s_tx_start = now_ms();
    s_tx_air = airtime_ms(t.len);
}

static void tx_finished(int ok) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tx_head = (s_tx_head + 1) % TX_Q;
    s_tx_n--;
    xSemaphoreGive(s_lock);
    s_txing = 0;
    if (ok) {
        s_stats.tx_ok++;
        s_stats.tx_air_ms += s_tx_air;
    } else {
        s_stats.tx_fail++;
    }
    start_rx();
}

static void radio_task(void *arg) {
    start_rx();
    uint32_t last_poll = now_ms();
    while (s_run) {
        uint32_t woke = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        if (!s_run) break;
        if (s_reconfig) {
            s_reconfig = 0;
            if (!s_txing) { apply_cfg(); start_rx(); }
            else s_reconfig = 1;
        }
        /* the chip is only asked when DIO1 says something happened (or once a second, in case an edge was missed),
           so the SD card shares the bus with as little radio traffic as possible */
        uint16_t irq = 0;
        if (woke || gpio_get_level(PIN_LORA_DIO1) || now_ms() - last_poll > 1000) {
            irq = irq_status();
            last_poll = now_ms();
        }
        if (irq) {
            irq_clear(irq);
            if (irq & IRQ_HEADER_VALID) { s_rx_header = 1; s_header_ms = now_ms(); }
            if (s_txing && (irq & (IRQ_TX_DONE | IRQ_TIMEOUT))) {
                tx_finished((irq & IRQ_TX_DONE) != 0);
            } else if (irq & IRQ_RX_DONE) {
                s_rx_header = 0;
                if (irq & (IRQ_CRC_ERR | IRQ_HEADER_ERR)) s_stats.rx_bad++;
                else read_packet();
            } else if (irq & (IRQ_CRC_ERR | IRQ_HEADER_ERR)) {
                s_rx_header = 0;
                s_stats.rx_bad++;
            }
        }
        if (s_txing && now_ms() - s_tx_start > TX_TIMEOUT_MS + 500) tx_finished(0); /* never heard back */
        if (s_rx_header && now_ms() - s_header_ms > BUSY_MAX_MS) s_rx_header = 0;
        /* send when nothing is being received; the app does the random back-off */
        if (!s_txing && s_tx_n && !s_rx_header) start_tx();
    }
    standby();
    uint8_t sleep = 0x04; /* warm start, config kept */
    cmd(CMD_SET_SLEEP, &sleep, 1);
    s_task_done = 1;
    vTaskDelete(NULL);
}

/* ---- API --------------------------------------------------------------------------------- */
static void pins_idle(void) {
    /* back to how the HAL leaves them: outputs held high so the SD bus stays clean */
    static const int pins[] = {PIN_LORA_RST, PIN_LORA_DIO1, PIN_LORA_NSS, PIN_LORA_BUSY};
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(pins[i], 1);
    }
}

static void l_end(void);

static int l_begin(const cpos_lora_cfg_t *cfg) {
    if (!cpos_has_perm(CPOS_PERM_LORA)) return -1;
    if (!cfg_ok(cfg)) return -3;
#ifdef CONFIG_CPOS_EMULATOR
    return -2; /* no radio in the emulator */
#endif
    if (s_up) l_end();
    s_cfg = *cfg;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    s_rxq = heap_caps_malloc(sizeof(cpos_lora_pkt_t) * RX_Q, MALLOC_CAP_INTERNAL);
    s_txq = heap_caps_malloc(sizeof(txpkt_t) * TX_Q, MALLOC_CAP_INTERNAL);
    if (!s_rxq || !s_txq) { l_end(); return -2; }
    s_rx_head = s_rx_n = s_tx_head = s_tx_n = 0;
    s_txing = 0;
    memset(&s_stats, 0, sizeof(s_stats));

    gpio_reset_pin(PIN_LORA_RST);
    gpio_set_direction(PIN_LORA_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LORA_RST, 1);
    gpio_reset_pin(PIN_LORA_BUSY);
    gpio_set_direction(PIN_LORA_BUSY, GPIO_MODE_INPUT);
    gpio_reset_pin(PIN_LORA_DIO1);
    gpio_set_direction(PIN_LORA_DIO1, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_LORA_DIO1, GPIO_PULLDOWN_ONLY);
    gpio_reset_pin(PIN_LORA_NSS);

    spi_bus_config_t bus = {.mosi_io_num = 14, .miso_io_num = 39, .sclk_io_num = 40, .quadwp_io_num = -1,
                            .quadhd_io_num = -1, .max_transfer_sz = 4096};
    esp_err_t e = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO); /* normally already up for the SD card */
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { l_end(); return -2; }
    /* 2 MHz like RadioLib's default: long traces through the cap connector, shared with the card */
    spi_device_interface_config_t dev = {.mode = 0, .clock_speed_hz = 2 * 1000 * 1000, .spics_io_num = PIN_LORA_NSS,
                                         .queue_size = 1};
    if (spi_bus_add_device(SPI3_HOST, &dev, &s_spi) != ESP_OK) { s_spi = NULL; l_end(); return -2; }
    s_up = 1;

    int cap = cap_enable(); /* a missing expander is logged; the chip is still tried */
    snprintf(s_diag, sizeof(s_diag), "exp:%s busy0:%d", cap ? "no" : "ok", gpio_get_level(PIN_LORA_BUSY));
    int rc = chip_init(1, 0);
    s_tcxo = rc == 0;
    if (rc == -1) rc = chip_init(0, 0); /* no TCXO on this board: crystal */
    if (rc == -1) { rc = chip_init(1, 1); s_tcxo = 1; } /* both flagged: the Cap LoRa-1262 has a TCXO, use it anyway */
    {
        size_t l = strlen(s_diag);
        snprintf(s_diag + l, sizeof(s_diag) - l, rc ? " -> sin radio" : (s_tcxo ? " -> ok TCXO" : " -> ok XTAL"));
    }
    if (rc) {
        ESP_LOGW(TAG, "no SX1262 answered (%d, expander %s)", rc, cap ? "missing" : "ok");
        l_end();
        return -2;
    }
    e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { l_end(); return -2; }
    gpio_set_intr_type(PIN_LORA_DIO1, GPIO_INTR_POSEDGE);
    gpio_isr_handler_add(PIN_LORA_DIO1, dio1_isr, NULL);
    s_run = 1;
    s_task_done = 0;
    if (xTaskCreatePinnedToCore(radio_task, "lora", 4096, NULL, 6, &s_task, 1) != pdPASS) {
        s_task = NULL;
        s_run = 0;
        l_end();
        return -2;
    }
    ESP_LOGI(TAG, "up: %lu Hz, bw %lu, sf %u, cr 4/%u, %d dBm, %s", (unsigned long)s_cfg.freq_hz,
             (unsigned long)s_cfg.bw_hz, s_cfg.sf, s_cfg.cr, s_cfg.power_dbm, s_tcxo ? "TCXO" : "XTAL");
    return 0;
}

static void l_end(void) {
    if (s_task) {
        s_run = 0;
        xTaskNotifyGive(s_task);
        for (int i = 0; i < 100 && !s_task_done; i++) vTaskDelay(pdMS_TO_TICKS(10));
        s_task = NULL;
    }
    if (s_up) {
        gpio_isr_handler_remove(PIN_LORA_DIO1);
        gpio_set_intr_type(PIN_LORA_DIO1, GPIO_INTR_DISABLE);
    }
    if (s_spi) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    if (s_up) pins_idle();
    cap_disable();
    s_up = 0;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    heap_caps_free(s_rxq);
    heap_caps_free(s_txq);
    s_rxq = NULL;
    s_txq = NULL;
    s_rx_n = s_tx_n = 0;
    if (s_lock) xSemaphoreGive(s_lock);
}

static int l_configure(const cpos_lora_cfg_t *cfg) {
    if (!s_run) return -1;
    if (!cfg_ok(cfg)) return -3;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    s_reconfig = 1;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return 0;
}

static int l_send(const uint8_t *data, size_t len) {
    if (!s_run || !data || !len || len > 255) return -1;
    int ok = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_tx_n < TX_Q) {
        txpkt_t *t = &s_txq[(s_tx_head + s_tx_n) % TX_Q];
        t->len = len;
        memcpy(t->data, data, len);
        s_tx_n++;
        ok = 0;
    }
    xSemaphoreGive(s_lock);
    if (ok == 0) xTaskNotifyGive(s_task);
    return ok;
}

static int l_pending(void) { return s_run ? s_tx_n : 0; }

static int l_recv(cpos_lora_pkt_t *out) {
    cpos_check_exit();
    if (!s_run || !out) return 0;
    int got = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_rx_n) {
        *out = s_rxq[s_rx_head];
        s_rx_head = (s_rx_head + 1) % RX_Q;
        s_rx_n--;
        got = 1;
    }
    xSemaphoreGive(s_lock);
    return got;
}

static int l_busy(void) { return s_run && (s_rx_header || s_txing); }

static void l_stats(cpos_lora_stats_t *out) {
    if (out) *out = s_stats;
}

/* ---- GNSS ------------------------------------------------------------------------------ */
static void l_gnss_end(void);

static int l_gnss_begin(uint32_t baud) {
    if (!cpos_has_perm(CPOS_PERM_GNSS)) return -1;
#ifdef CONFIG_CPOS_EMULATOR
    return -2;
#endif
    if (s_gnss_up) l_gnss_end();
    uart_config_t c = {.baud_rate = (int)(baud ? baud : 115200), .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                       .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT};
    gpio_reset_pin(PIN_GNSS_TX);
    gpio_reset_pin(PIN_GNSS_RX);
    if (uart_driver_install(GNSS_UART, 2048, 0, 0, NULL, 0) != ESP_OK) return -2;
    uart_param_config(GNSS_UART, &c);
    s_gnss_swap = 0;
    s_gnss_seen = 0;
    s_gnss_try_at = now_ms();
    uart_set_pin(GNSS_UART, PIN_GNSS_TX, PIN_GNSS_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    s_gnss_up = 1;
    return 0;
}

/* Boards and docs disagree on which of 13/15 is the receiver's TX: until NMEA shows up, swap them every 3 s. */
static void gnss_try_swap(void) {
    if (s_gnss_seen || now_ms() - s_gnss_try_at < 3000) return;
    s_gnss_swap = !s_gnss_swap;
    s_gnss_try_at = now_ms();
    uart_set_pin(GNSS_UART, s_gnss_swap ? PIN_GNSS_RX : PIN_GNSS_TX, s_gnss_swap ? PIN_GNSS_TX : PIN_GNSS_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_flush_input(GNSS_UART);
}

static void l_gnss_end(void) {
    if (!s_gnss_up) return;
    uart_driver_delete(GNSS_UART);
    s_gnss_up = 0;
    static const int pins[] = {PIN_GNSS_TX, PIN_GNSS_RX};
    for (unsigned i = 0; i < 2; i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(pins[i], 1);
    }
}

static int l_gnss_read(uint8_t *buf, size_t max) {
    cpos_check_exit();
    if (!s_gnss_up || !buf || !max) return 0;
    int n = uart_read_bytes(GNSS_UART, buf, max, 0);
    if (n > 0 && !s_gnss_seen)
        for (int i = 0; i < n; i++)
            if (buf[i] == '$') { s_gnss_seen = 1; ESP_LOGI(TAG, "GNSS data on RX %d", s_gnss_swap ? PIN_GNSS_TX : PIN_GNSS_RX); break; }
    gnss_try_swap();
    return n > 0 ? n : 0;
}

static int l_gnss_write(const uint8_t *buf, size_t len) {
    if (!s_gnss_up || !buf) return -1;
    return uart_write_bytes(GNSS_UART, buf, len);
}

static int l_diag(char *buf, size_t n) {
    if (!buf || !n) return 0;
    return snprintf(buf, n, "%s | gps:%s%s", s_diag[0] ? s_diag : "-", !s_gnss_up ? "off" : s_gnss_seen ? "datos" : "sin datos",
                    s_gnss_up ? (s_gnss_swap ? " rx13" : " rx15") : "");
}

void cpos_lora_fill(cpos_lora_api_t *a) {
    a->diag = l_diag;
    a->begin = l_begin;
    a->end = l_end;
    a->configure = l_configure;
    a->send = l_send;
    a->pending = l_pending;
    a->recv = l_recv;
    a->busy = l_busy;
    a->stats = l_stats;
    a->gnss_begin = l_gnss_begin;
    a->gnss_end = l_gnss_end;
    a->gnss_read = l_gnss_read;
    a->gnss_write = l_gnss_write;
}

void cpos_app_cleanup_lora(app_ctx_t *a) {
    l_end();
    l_gnss_end();
}
