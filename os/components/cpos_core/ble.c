/*
 * Bluetooth LE for apps: NimBLE as observer + central, one connection.
 * The stack is started when an app with the "ble" permission asks for it and torn down
 * when the app ends, so its ~60 KB of RAM is only taken while a BLE app runs.
 * NimBLE events are queued here; the app polls them from its own task.
 */
#include "app_ctx.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "mbedtls/aes.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include <string.h>

static const char *TAG = "ble";

#define ADV_Q 48
#define NTF_Q 8
#define NTF_MAX 64
#define MAX_SVC 16
#define MAX_CHR 32
#define NO_CONN 0xFFFF

static volatile int s_up, s_synced, s_scanning;
static uint8_t s_own_addr_type;
static volatile uint16_t s_conn = NO_CONN;
static SemaphoreHandle_t s_done;  /* given by the host task when a blocking procedure ends */
static volatile int s_status;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static cpos_ble_adv_t s_adv[ADV_Q];
static int s_adv_head, s_adv_n;

typedef struct { uint16_t handle; uint8_t len; uint8_t data[NTF_MAX]; } ntf_t;
static ntf_t s_ntf[NTF_Q];
static int s_ntf_head, s_ntf_n;

static cpos_ble_attr_t s_svc[MAX_SVC];
static int s_nsvc, s_svc_done;
static cpos_ble_attr_t s_chr[MAX_CHR];
static uint16_t s_chr_def[MAX_CHR];
static int s_nchr;
static uint16_t s_cccd;
static uint8_t *s_rbuf;
static size_t s_rmax;
static int s_rlen;

/* wait for the host task; an exit request (Fn+`) leaves the app right away */
static int wait_done(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 50) {
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(50)) == pdTRUE) return 0;
        cpos_check_exit();
    }
    return -1;
}

static void uuid_out(const ble_uuid_any_t *u, cpos_ble_attr_t *a) {
    static const uint8_t base[16] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0, 0, 0, 0};
    memcpy(a->uuid, base, 16);
    if (u->u.type == BLE_UUID_TYPE_16) {
        a->uuid[12] = u->u16.value & 0xFF;
        a->uuid[13] = u->u16.value >> 8;
        a->uuid_bits = 16;
    } else if (u->u.type == BLE_UUID_TYPE_32) {
        for (int i = 0; i < 4; i++) a->uuid[12 + i] = (u->u32.value >> (8 * i)) & 0xFF;
        a->uuid_bits = 32;
    } else {
        memcpy(a->uuid, u->u128.value, 16);
        a->uuid_bits = 128;
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg) {
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        cpos_ble_adv_t r = {0};
        memcpy(r.addr, ev->disc.addr.val, 6);
        r.addr_type = ev->disc.addr.type;
        r.event = ev->disc.event_type;
        r.rssi = ev->disc.rssi;
        r.len = ev->disc.length_data > 31 ? 31 : ev->disc.length_data;
        memcpy(r.data, ev->disc.data, r.len);
        taskENTER_CRITICAL(&s_mux);
        if (s_adv_n < ADV_Q) {
            s_adv[(s_adv_head + s_adv_n) % ADV_Q] = r;
            s_adv_n++;
        }
        taskEXIT_CRITICAL(&s_mux);
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = 0;
        break;
    case BLE_GAP_EVENT_CONNECT:
        s_status = ev->connect.status;
        s_conn = ev->connect.status == 0 ? ev->connect.conn_handle : NO_CONN;
        xSemaphoreGive(s_done);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = NO_CONN;
        s_svc_done = 0;
        xSemaphoreGive(s_done);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        ntf_t n = {.handle = ev->notify_rx.attr_handle};
        uint16_t len = 0;
        ble_hs_mbuf_to_flat(ev->notify_rx.om, n.data, NTF_MAX, &len);
        n.len = len;
        taskENTER_CRITICAL(&s_mux);
        if (s_ntf_n < NTF_Q) {
            s_ntf[(s_ntf_head + s_ntf_n) % NTF_Q] = n;
            s_ntf_n++;
        }
        taskEXIT_CRITICAL(&s_mux);
        break;
    }
    default:
        break;
    }
    return 0;
}

/* ---- stack up / down ---------------------------------------------------------------- */
static void on_sync(void) {
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = 1;
}

static void on_reset(int reason) { s_synced = 0; }

static void host_task(void *arg) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void b_end(void);

static int b_begin(void) {
    if (!cpos_has_perm(CPOS_PERM_BLE)) return 0;
#ifdef CONFIG_CPOS_EMULATOR
    return 1; /* no radio in the emulator: scans return a few fake robots */
#endif
    if (s_up) return 1;
    if (!s_done) s_done = xSemaphoreCreateBinary();
    xSemaphoreTake(s_done, 0);
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed (free heap %u)", (unsigned)esp_get_free_heap_size());
        return 0;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    s_up = 1;
    nimble_port_freertos_init(host_task);
    for (int i = 0; i < 60 && !s_synced; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (!s_synced) {
        ESP_LOGE(TAG, "host did not sync");
        b_end();
        return 0;
    }
    ESP_LOGI(TAG, "up (free heap %u)", (unsigned)esp_get_free_heap_size());
    return 1;
}

static void b_end(void) {
    if (!s_up) return;
    if (s_scanning) ble_gap_disc_cancel();
    s_scanning = 0;
    if (s_conn != NO_CONN) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        xSemaphoreTake(s_done, pdMS_TO_TICKS(1000));
    }
    if (nimble_port_stop() == 0) nimble_port_deinit();
    s_up = s_synced = 0;
    s_conn = NO_CONN;
    s_adv_n = s_ntf_n = 0;
    s_svc_done = 0;
    ESP_LOGI(TAG, "down (free heap %u)", (unsigned)esp_get_free_heap_size());
}

/* ---- scanning ---------------------------------------------------------------------- */
static int b_scan_start(uint32_t ms, int active, uint16_t itvl_ms, uint16_t win_ms, int dup) {
#ifdef CONFIG_CPOS_EMULATOR
    static const char *names[] = {"BB-4D2A", "D2-7F31", "GB-A011", "SB-9C4E", "RS_W120114"};
    s_adv_head = s_adv_n = 0;
    for (int i = 0; i < 5; i++) {
        cpos_ble_adv_t *a = &s_adv[s_adv_n++];
        memset(a, 0, sizeof(*a));
        a->addr[0] = i + 1; a->addr_type = 1; a->rssi = -45 - 7 * i;
        a->data[0] = 1 + strlen(names[i]); a->data[1] = 0x09;
        memcpy(&a->data[2], names[i], strlen(names[i]));
        a->len = 2 + strlen(names[i]);
    }
    return 1;
#endif
    if (!s_up || s_conn != NO_CONN) return 0;
    if (s_scanning) ble_gap_disc_cancel();
    struct ble_gap_disc_params p = {0};
    if (!itvl_ms) itvl_ms = 100;
    if (!win_ms || win_ms > itvl_ms) win_ms = itvl_ms;
    p.itvl = itvl_ms * 8 / 5; /* 0.625 ms units */
    p.window = win_ms * 8 / 5;
    p.passive = !active;
    p.filter_duplicates = dup ? 1 : 0;
    int rc = ble_gap_disc(s_own_addr_type, ms ? (int32_t)ms : BLE_HS_FOREVER, &p, gap_event, NULL);
    s_scanning = rc == 0;
    if (rc) ESP_LOGW(TAG, "scan start rc=%d", rc);
    return rc == 0;
}

static void b_scan_stop(void) {
    if (s_up && s_scanning) ble_gap_disc_cancel();
    s_scanning = 0;
}

static int b_scanning(void) { return s_scanning; }

static int b_scan_read(cpos_ble_adv_t *out) {
    int got = 0;
    taskENTER_CRITICAL(&s_mux);
    if (s_adv_n) {
        *out = s_adv[s_adv_head];
        s_adv_head = (s_adv_head + 1) % ADV_Q;
        s_adv_n--;
        got = 1;
    }
    taskEXIT_CRITICAL(&s_mux);
    return got;
}

/* ---- connection ---------------------------------------------------------------------- */
static int b_connect(const uint8_t addr[6], uint8_t type, uint32_t timeout_ms) {
    if (!s_up) return -1;
    if (s_conn != NO_CONN) return 0;
    b_scan_stop();
    ble_addr_t peer = {.type = type};
    memcpy(peer.val, addr, 6);
    if (!timeout_ms || timeout_ms > 15000) timeout_ms = 15000;
    xSemaphoreTake(s_done, 0);
    s_svc_done = 0;
    s_ntf_n = 0;
    int rc = ble_gap_connect(s_own_addr_type, &peer, timeout_ms, NULL, gap_event, NULL);
    if (rc) {
        ESP_LOGW(TAG, "connect rc=%d", rc);
        return -1;
    }
    if (wait_done(timeout_ms + 1000)) {
        ble_gap_conn_cancel();
        return -1;
    }
    return s_conn != NO_CONN ? 0 : -1;
}

static void b_disconnect(void) {
    if (!s_up || s_conn == NO_CONN) return;
    xSemaphoreTake(s_done, 0);
    if (ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM) == 0) wait_done(2000);
    s_conn = NO_CONN;
}

static int b_connected(void) { return s_up && s_conn != NO_CONN; }

/* ---- discovery ----------------------------------------------------------------------- */
static int svc_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg) {
    if (err->status == 0 && svc && s_nsvc < MAX_SVC) {
        cpos_ble_attr_t *a = &s_svc[s_nsvc++];
        memset(a, 0, sizeof(*a));
        uuid_out(&svc->uuid, a);
        a->handle = svc->start_handle;
        a->end_handle = svc->end_handle;
    }
    if (err->status) {
        s_status = err->status == BLE_HS_EDONE ? 0 : err->status;
        xSemaphoreGive(s_done);
    }
    return 0;
}

static int b_services(cpos_ble_attr_t *out, int max) {
    if (!b_connected()) return 0;
    if (!s_svc_done) {
        s_nsvc = 0;
        xSemaphoreTake(s_done, 0);
        if (ble_gattc_disc_all_svcs(s_conn, svc_cb, NULL) || wait_done(5000) || s_status) return 0;
        s_svc_done = 1;
    }
    int n = s_nsvc < max ? s_nsvc : max;
    memcpy(out, s_svc, n * sizeof(*out));
    return n;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *c, void *arg) {
    if (err->status == 0 && c && s_nchr < MAX_CHR) {
        cpos_ble_attr_t *a = &s_chr[s_nchr];
        memset(a, 0, sizeof(*a));
        uuid_out(&c->uuid, a);
        a->props = c->properties;
        a->handle = c->val_handle;
        s_chr_def[s_nchr++] = c->def_handle;
    }
    if (err->status) {
        s_status = err->status == BLE_HS_EDONE ? 0 : err->status;
        xSemaphoreGive(s_done);
    }
    return 0;
}

static int b_characteristics(int service, cpos_ble_attr_t *out, int max) {
    if (!b_connected() || !s_svc_done || service < 0 || service >= s_nsvc) return 0;
    s_nchr = 0;
    xSemaphoreTake(s_done, 0);
    const cpos_ble_attr_t *sv = &s_svc[service];
    if (ble_gattc_disc_all_chrs(s_conn, sv->handle, sv->end_handle, chr_cb, NULL) || wait_done(5000) || s_status) return 0;
    for (int i = 0; i < s_nchr; i++) /* a characteristic ends where the next one is declared */
        s_chr[i].end_handle = i + 1 < s_nchr ? s_chr_def[i + 1] - 1 : sv->end_handle;
    int n = s_nchr < max ? s_nchr : max;
    memcpy(out, s_chr, n * sizeof(*out));
    return n;
}

/* ---- read / write / notify ---------------------------------------------------------- */
static int read_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg) {
    s_status = err->status;
    if (err->status == 0 && attr) {
        uint16_t len = 0;
        ble_hs_mbuf_to_flat(attr->om, s_rbuf, s_rmax, &len);
        s_rlen = len;
    }
    xSemaphoreGive(s_done);
    return 0;
}

static int b_read(uint16_t handle, uint8_t *buf, size_t max) {
    if (!b_connected()) return -1;
    s_rbuf = buf;
    s_rmax = max;
    s_rlen = 0;
    xSemaphoreTake(s_done, 0);
    if (ble_gattc_read(s_conn, handle, read_cb, NULL) || wait_done(5000) || s_status) return -1;
    return s_rlen;
}

static int write_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg) {
    s_status = err->status;
    xSemaphoreGive(s_done);
    return 0;
}

static int b_write(uint16_t handle, const uint8_t *data, size_t len, int rsp) {
    if (!b_connected()) return -1;
    if (!rsp) return ble_gattc_write_no_rsp_flat(s_conn, handle, data, len) ? -1 : 0;
    xSemaphoreTake(s_done, 0);
    if (ble_gattc_write_flat(s_conn, handle, data, len, write_cb, NULL) || wait_done(5000)) return -1;
    return s_status ? -1 : 0;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr, const struct ble_gatt_dsc *d, void *arg) {
    if (err->status == 0 && d && ble_uuid_u16(&d->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) s_cccd = d->handle;
    if (err->status) {
        s_status = err->status == BLE_HS_EDONE ? 0 : err->status;
        xSemaphoreGive(s_done);
    }
    return 0;
}

static int b_subscribe(uint16_t val, uint16_t end, int mode) {
    if (!b_connected()) return -1;
    s_cccd = 0;
    if (end > val) {
        xSemaphoreTake(s_done, 0);
        if (ble_gattc_disc_all_dscs(s_conn, val, end, dsc_cb, NULL) == 0) wait_done(5000);
    }
    if (!s_cccd) s_cccd = val + 1; /* the usual place when the peer does not list it */
    uint8_t v[2] = {mode == 1 ? 1 : mode == 2 ? 2 : 0, 0};
    return b_write(s_cccd, v, 2, 1);
}

static int b_notify_read(uint16_t *handle, uint8_t *buf, size_t max) {
    int len = -1;
    taskENTER_CRITICAL(&s_mux);
    if (s_ntf_n) {
        ntf_t *n = &s_ntf[s_ntf_head];
        len = n->len < max ? n->len : max;
        memcpy(buf, n->data, len);
        *handle = n->handle;
        s_ntf_head = (s_ntf_head + 1) % NTF_Q;
        s_ntf_n--;
    }
    taskEXIT_CRITICAL(&s_mux);
    return len;
}

static void b_set_power(int level) {
    if (s_up) esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, (esp_power_level_t)level);
}

/* ---- crypto ---------------------------------------------------------------------------- */
static int c_aes_ecb(const uint8_t *key, int bits, int enc, const uint8_t in[16], uint8_t out[16]) {
    mbedtls_aes_context c;
    mbedtls_aes_init(&c);
    int rc = enc ? mbedtls_aes_setkey_enc(&c, key, bits) : mbedtls_aes_setkey_dec(&c, key, bits);
    if (!rc) rc = mbedtls_aes_crypt_ecb(&c, enc ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, in, out);
    mbedtls_aes_free(&c);
    return rc;
}

void cpos_ble_fill(cpos_ble_api_t *a, cpos_crypto_api_t *c) {
    a->begin = b_begin;
    a->end = b_end;
    a->scan_start = b_scan_start;
    a->scan_stop = b_scan_stop;
    a->scanning = b_scanning;
    a->scan_read = b_scan_read;
    a->connect = b_connect;
    a->disconnect = b_disconnect;
    a->connected = b_connected;
    a->services = b_services;
    a->characteristics = b_characteristics;
    a->read = b_read;
    a->write = b_write;
    a->subscribe = b_subscribe;
    a->notify_read = b_notify_read;
    a->set_power = b_set_power;
    c->aes_ecb = c_aes_ecb;
}

void cpos_app_cleanup_ble(app_ctx_t *a) { b_end(); }
