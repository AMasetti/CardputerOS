/* WiFi + sockets. The OS owns the radio: connection state survives app changes. */
#include "app_ctx.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "net";
static bool s_inited;
static EventGroupHandle_t s_ev;
#define EV_GOT_IP BIT0
#define EV_FAIL BIT1
#define EV_SCAN BIT2
static volatile int s_status = 0; /* Arduino wl_status_t: 0 idle, 1 no ssid, 3 connected, 4 failed, 6 disconnected */
static int s_retry;
static int s_want;                /* a connect attempt is in progress */
static char s_cur_ssid[33];
static uint32_t s_ip;
static uint32_t s_mask;
static wifi_ap_record_t s_aps[24];
static int s_ap_count = -1;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_DISCONNECTED:
                s_ip = 0;
                s_status = 6;
                if (s_want && s_retry < 3) {
                    s_retry++;
                    esp_wifi_connect();
                } else if (s_want) {
                    s_want = 0;
                    s_status = 4;
                    xEventGroupSetBits(s_ev, EV_FAIL);
                }
                break;
            case WIFI_EVENT_SCAN_DONE: {
                uint16_t n = 24;
                if (esp_wifi_scan_get_ap_records(&n, s_aps) != ESP_OK) n = 0;
                s_ap_count = n;
                xEventGroupSetBits(s_ev, EV_SCAN);
                break;
            }
            default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_ip = e->ip_info.ip.addr;
        s_mask = e->ip_info.netmask.addr;
        s_status = 3;
        s_want = 0;
        xEventGroupSetBits(s_ev, EV_GOT_IP);
    }
}

void cpos_wifi_ensure(void) {
    if (s_inited) return;
#ifdef CONFIG_CPOS_EMULATOR
    /* no radio in the emulator: pretend it is up and let scans return two fake networks */
    s_ev = xEventGroupCreate();
    s_inited = true;
    return;
#endif
    s_ev = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_ERROR_CHECK(esp_wifi_start());
    s_inited = true;
}

void cpos_wifi_stop(void) {
    if (!s_inited) return;
    esp_wifi_stop();
    esp_wifi_deinit();
    s_inited = false;
    s_status = 0;
    s_ip = 0;
}

int cpos_wifi_scan_blocking(cpos_scan_t *out, int max) {
    cpos_wifi_ensure();
    xEventGroupClearBits(s_ev, EV_SCAN);
    s_ap_count = -1;
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) return 0;
    xEventGroupWaitBits(s_ev, EV_SCAN, pdTRUE, pdFALSE, pdMS_TO_TICKS(8000));
    int n = 0;
    for (int i = 0; i < s_ap_count && n < max; i++) {
        const char *ss = (const char *)s_aps[i].ssid;
        if (!ss[0]) continue;
        bool dup = false;
        for (int j = 0; j < n; j++) if (strcmp(out[j].ssid, ss) == 0) dup = true;
        if (dup) continue;
        strlcpy(out[n].ssid, ss, 33);
        out[n].rssi = s_aps[i].rssi;
        out[n].open = s_aps[i].authmode == WIFI_AUTH_OPEN;
        out[n].channel = s_aps[i].primary;
        n++;
    }
    return n;
}

int cpos_wifi_connect(const char *ssid, const char *pass, int timeout_ms) {
    cpos_wifi_ensure();
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password));
    wc.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    s_want = 0;
    esp_wifi_disconnect();
    xEventGroupClearBits(s_ev, EV_GOT_IP | EV_FAIL);
    s_retry = 0;
    s_want = 1;
    strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    if (timeout_ms <= 0) return 0;
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (b & EV_GOT_IP) return 0;
    s_want = 0;
    return -1;
}

void cpos_wifi_save(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
}

int cpos_wifi_load(char *ssid, char *pass) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return -1;
    size_t a = 33, b = 65;
    int rc = 0;
    if (nvs_get_str(h, "ssid", ssid, &a) != ESP_OK || !ssid[0]) rc = -1;
    if (nvs_get_str(h, "pass", pass, &b) != ESP_OK) pass[0] = 0;
    nvs_close(h);
    return rc;
}

int cpos_wifi_autoconnect(int timeout_ms) {
    char ssid[33], pass[65];
    if (cpos_wifi_is_connected()) return 0;
    if (cpos_wifi_load(ssid, pass)) return -1;
    return cpos_wifi_connect(ssid, pass, timeout_ms);
}

int cpos_wifi_is_connected(void) { return s_inited && s_status == 3 && s_ip != 0; }

/* ---- app-facing API ----------------------------------------------------------------- */
#define NEED_WIFI(ret) do { if (!cpos_has_perm(CPOS_PERM_WIFI)) return ret; cpos_check_exit(); } while (0)

static int n_mode(int sta) {
    NEED_WIFI(-1);
    if (sta) cpos_wifi_ensure();
    return 0;
}
static int n_scan_start(int async) {
    NEED_WIFI(-1);
    cpos_wifi_ensure();
#ifdef CONFIG_CPOS_EMULATOR
    memset(s_aps, 0, sizeof(s_aps));
    strcpy((char *)s_aps[0].ssid, "JumpingSumo_a1b2c3"); s_aps[0].rssi = -48; s_aps[0].primary = 6;
    strcpy((char *)s_aps[1].ssid, "HomeWifi"); s_aps[1].rssi = -60; s_aps[1].authmode = WIFI_AUTH_WPA2_PSK;
    strcpy((char *)s_aps[2].ssid, "JumpingSumo_9f8e7d"); s_aps[2].rssi = -71; s_aps[2].primary = 11;
    s_ap_count = 3;
    return async ? 0 : 3;
#endif
    xEventGroupClearBits(s_ev, EV_SCAN);
    s_ap_count = -1;
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) return -1;
    if (async) return 0;
    xEventGroupWaitBits(s_ev, EV_SCAN, pdTRUE, pdFALSE, pdMS_TO_TICKS(8000));
    return s_ap_count < 0 ? 0 : s_ap_count;
}
static int n_scan_count(void) { return cpos_has_perm(CPOS_PERM_WIFI) ? s_ap_count : -1; }
static int n_scan_get(int i, cpos_ap_t *o) {
    if (!cpos_has_perm(CPOS_PERM_WIFI) || i < 0 || i >= s_ap_count) return -1;
    strlcpy(o->ssid, (const char *)s_aps[i].ssid, 33);
    o->rssi = s_aps[i].rssi;
    o->auth_open = s_aps[i].authmode == WIFI_AUTH_OPEN;
    o->channel = s_aps[i].primary;
    return 0;
}
static void n_scan_clear(void) { s_ap_count = -1; }
static int n_connect(const char *ssid, const char *pass) {
    NEED_WIFI(-1);
    return cpos_wifi_connect(ssid, pass, 0);
}
static void n_disconnect(void) {
    if (!cpos_has_perm(CPOS_PERM_WIFI) || !s_inited) return;
    s_want = 0;
    esp_wifi_disconnect();
}
static int n_status(void) { return cpos_has_perm(CPOS_PERM_WIFI) ? s_status : 0; }
static uint32_t n_ip(void) { return cpos_has_perm(CPOS_PERM_WIFI) ? s_ip : 0; }
static uint32_t n_bcast(void) { return cpos_has_perm(CPOS_PERM_WIFI) ? (s_ip | ~s_mask) : 0; }
static int n_rssi(void) {
    wifi_ap_record_t r;
    if (!cpos_has_perm(CPOS_PERM_WIFI) || !s_inited || esp_wifi_sta_get_ap_info(&r) != ESP_OK) return 0;
    return r.rssi;
}
static const char *n_ssid(void) { return s_cur_ssid; }
static void n_sleep(int on) {
    if (cpos_has_perm(CPOS_PERM_WIFI) && s_inited) esp_wifi_set_ps(on ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

static int n_socket(int d, int t, int p) {
    NEED_WIFI(-1);
    int fd = lwip_socket(d, t, p);
    if (fd >= 0 && g_app) {
        int placed = 0;
        for (int i = 0; i < APP_MAX_FDS; i++)
            if (g_app->fds[i] < 0) { g_app->fds[i] = fd; placed = 1; break; }
        if (!placed) { lwip_close(fd); return -1; }
    }
    return fd;
}
static int n_close(int fd) {
    if (g_app)
        for (int i = 0; i < APP_MAX_FDS; i++)
            if (g_app->fds[i] == fd) g_app->fds[i] = -1;
    return lwip_close(fd);
}
static int n_bind(int fd, const void *a, uint32_t l) { return cpos_has_perm(CPOS_PERM_WIFI) ? lwip_bind(fd, a, l) : -1; }
static int n_sendto(int fd, const void *b, size_t n, int f, const void *a, uint32_t al) {
    return cpos_has_perm(CPOS_PERM_WIFI) ? lwip_sendto(fd, b, n, f, a, al) : -1;
}
static int n_recvfrom(int fd, void *b, size_t n, int f, void *a, uint32_t *al) {
    return cpos_has_perm(CPOS_PERM_WIFI) ? lwip_recvfrom(fd, b, n, f, a, (socklen_t *)al) : -1;
}
static int n_setsockopt(int fd, int lv, int o, const void *v, uint32_t l) {
    return cpos_has_perm(CPOS_PERM_WIFI) ? lwip_setsockopt(fd, lv, o, v, l) : -1;
}
static int n_fcntl(int fd, int c, int a) { return cpos_has_perm(CPOS_PERM_WIFI) ? lwip_fcntl(fd, c, a) : -1; }

static int n_sock_connect(int fd, const void *a, uint32_t l) { return cpos_has_perm(CPOS_PERM_WIFI) ? lwip_connect(fd, a, l) : -1; }
static int n_sock_send(int fd, const void *b, size_t n, int f) { return cpos_has_perm(CPOS_PERM_WIFI) ? lwip_send(fd, b, n, f) : -1; }
static int n_sock_recv(int fd, void *b, size_t n, int f) { return cpos_has_perm(CPOS_PERM_WIFI) ? lwip_recv(fd, b, n, f) : -1; }

void cpos_net_fill(cpos_net_api_t *n) {
    n->sock_connect = n_sock_connect; n->sock_send = n_sock_send; n->sock_recv = n_sock_recv;
    n->mode = n_mode; n->scan_start = n_scan_start; n->scan_count = n_scan_count; n->scan_get = n_scan_get;
    n->scan_clear = n_scan_clear; n->connect = n_connect; n->disconnect = n_disconnect; n->status = n_status;
    n->ip = n_ip; n->broadcast_ip = n_bcast; n->rssi = n_rssi; n->ssid = n_ssid; n->set_sleep = n_sleep;
    n->socket = n_socket; n->bind = n_bind; n->sendto = n_sendto; n->recvfrom = n_recvfrom;
    n->setsockopt = n_setsockopt; n->fcntl = n_fcntl; n->close = n_close;
}

void cpos_app_cleanup_net(app_ctx_t *a) {
    for (int i = 0; i < APP_MAX_FDS; i++)
        if (a->fds[i] >= 0) { lwip_close(a->fds[i]); a->fds[i] = -1; }
}
