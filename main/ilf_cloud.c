#include "ilf_cloud.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "ilf_voice.h"

static const char *TAG = "ilf_cloud";

#define HERD_CHANNEL   1
#define CONNECT_MS     15000
#define MAX_PER_SYNC   40
#define OAUTH          "https://oauth.yandex.ru"
#define DISK_API       "https://cloud-api.yandex.net/v1/disk"
#define BIT_IP         BIT0
#define BIT_FAIL       BIT1

typedef enum { C_SCAN, C_JOIN, C_FORGET, C_DISK_TAP, C_SYNC } ccmd_t;
typedef struct { ccmd_t type; char ssid[33]; char pass[65]; } cmd_t;

static QueueHandle_t s_cmd, s_ev;
static EventGroupHandle_t s_bits;
static volatile bool s_busy, s_session;
static char s_dev_name[40], s_dev_id[24];
static char s_ssid[33], s_pass[65];
static char s_tok[96], s_rtok[96];
static uint32_t s_tok_exp;
static int s_retries;

static const char *client_id(void) { return CONFIG_ILF_YANDEX_CLIENT_ID; }
static const char *client_secret(void) { return CONFIG_ILF_YANDEX_CLIENT_SECRET; }

/* ---------- события наружу ---------- */

static void emit_text(ilf_cloud_ev_type_t type, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void emit_text(ilf_cloud_ev_type_t type, const char *fmt, ...)
{
    ilf_cloud_event_t *e = calloc(1, sizeof *e);
    if (!e) return;
    e->type = type;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof e->text, fmt, ap);
    va_end(ap);
    if (xQueueSend(s_ev, &e, 0) != pdTRUE) free(e);
}

static void emit(ilf_cloud_event_t *e)
{
    ilf_cloud_event_t *c = malloc(sizeof *c);
    if (!c) return;
    *c = *e;
    if (xQueueSend(s_ev, &c, 0) != pdTRUE) free(c);
}

bool ilf_cloud_poll(ilf_cloud_event_t *ev)
{
    ilf_cloud_event_t *e;
    if (!s_ev || xQueueReceive(s_ev, &e, 0) != pdTRUE) return false;
    *ev = *e;
    free(e);
    return true;
}

/* ---------- NVS ---------- */

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open("cloud", NVS_READONLY, &h) != ESP_OK) return;
    size_t n;
    n = sizeof s_ssid; nvs_get_str(h, "ssid", s_ssid, &n);
    n = sizeof s_pass; nvs_get_str(h, "pass", s_pass, &n);
    n = sizeof s_tok; nvs_get_str(h, "tok", s_tok, &n);
    n = sizeof s_rtok; nvs_get_str(h, "rtok", s_rtok, &n);
    nvs_get_u32(h, "exp", &s_tok_exp);
    nvs_close(h);
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("cloud", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", s_ssid);
    nvs_set_str(h, "pass", s_pass);
    nvs_set_str(h, "tok", s_tok);
    nvs_set_str(h, "rtok", s_rtok);
    nvs_set_u32(h, "exp", s_tok_exp);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------- Wi-Fi ---------- */

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (!s_session) return;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_bits, BIT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries-- > 0) esp_wifi_connect();
        else xEventGroupSetBits(s_bits, BIT_FAIL);
    }
}

static void back_to_herd(void)
{
    /* стадо слушает на канале 1 */
    esp_wifi_set_channel(HERD_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

static bool wifi_up(const char *ssid, const char *pass)
{
    if (!ssid[0]) return false;
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", pass);
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    xEventGroupClearBits(s_bits, BIT_IP | BIT_FAIL);
    s_retries = 2;
    s_session = true;
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_bits, BIT_IP | BIT_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_MS));
    if (b & BIT_IP) {
        ESP_LOGI(TAG, "Wi-Fi %s: есть интернет", ssid);
        return true;
    }
    ESP_LOGW(TAG, "Wi-Fi %s: не подключились", ssid);
    s_session = false;
    esp_wifi_disconnect();
    back_to_herd();
    return false;
}

static void wifi_down(void)
{
    s_session = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    back_to_herd();
}

static void do_scan(void)
{
    ilf_cloud_event_t *e = calloc(1, sizeof *e);
    if (!e) return;
    e->type = ILF_CLOUD_EV_SCAN;
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 20;
        wifi_ap_record_t *ap = calloc(n, sizeof *ap);
        if (ap && esp_wifi_scan_get_ap_records(&n, ap) == ESP_OK) {
            /* уже по силе сигнала; одинаковые имена (2.4 и 5 ГГц, репитеры) — один раз */
            for (int i = 0; i < n && e->n < ILF_CLOUD_SSIDS; i++) {
                const char *s = (const char *)ap[i].ssid;
                if (!s[0]) continue;
                bool dup = false;
                for (int j = 0; j < e->n; j++) dup |= !strcmp(e->ssids[j], s);
                if (!dup) snprintf(e->ssids[e->n++], sizeof e->ssids[0], "%s", s);
            }
        }
        free(ap);
    }
    back_to_herd();
    emit(e);
    free(e);
}

/* ---------- HTTP ---------- */

/* Запрос целиком в памяти: ответ до cap байт. Возвращает код HTTP или -1. */
static int http(esp_http_client_method_t method, const char *url, const char *auth, const char *ctype,
                const char *body, char *out, int cap)
{
    esp_http_client_config_t cfg = {
        .url = url, .method = method, .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    if (auth) esp_http_client_set_header(c, "Authorization", auth);
    if (ctype) esp_http_client_set_header(c, "Content-Type", ctype);
    int blen = body ? (int)strlen(body) : 0;
    int status = -1;
    if (esp_http_client_open(c, blen) == ESP_OK) {
        if (!blen || esp_http_client_write(c, body, blen) == blen) {
            esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
            int got = 0, r;
            while (out && got < cap - 1 && (r = esp_http_client_read(c, out + got, cap - 1 - got)) > 0) got += r;
            if (out) out[got] = 0;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

static void urlenc(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char ch = (unsigned char)*in;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || strchr("-_.~", ch)) {
            out[o++] = (char)ch;
        } else {
            out[o++] = '%';
            out[o++] = hex[ch >> 4];
            out[o++] = hex[ch & 15];
        }
    }
    out[o] = 0;
}

static const char *jstr(cJSON *j, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(j, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool take_tokens(cJSON *j)
{
    const char *at = jstr(j, "access_token"), *rt = jstr(j, "refresh_token");
    cJSON *exp = cJSON_GetObjectItem(j, "expires_in");
    if (!at) return false;
    snprintf(s_tok, sizeof s_tok, "%s", at);
    if (rt) snprintf(s_rtok, sizeof s_rtok, "%s", rt);
    s_tok_exp = (uint32_t)time(NULL) + (cJSON_IsNumber(exp) ? (uint32_t)exp->valuedouble : 3600u * 24 * 300);
    save();
    return true;
}

/* ---------- Яндекс: привязка по коду (OAuth device flow) ---------- */

static void disk_link(void)
{
    char *buf = malloc(1024);
    char body[256];
    if (!buf) return;
    snprintf(body, sizeof body, "client_id=%s&device_id=%s&device_name=", client_id(), s_dev_id);
    urlenc(s_dev_name, body + strlen(body), sizeof body - strlen(body));
    int st = http(HTTP_METHOD_POST, OAUTH "/device/code", NULL, "application/x-www-form-urlencoded", body, buf, 1024);
    cJSON *j = st == 200 ? cJSON_Parse(buf) : NULL;
    const char *dev = j ? jstr(j, "device_code") : NULL, *code = j ? jstr(j, "user_code") : NULL;
    const char *url = j ? jstr(j, "verification_url") : NULL;
    if (!dev || !code) {
        ESP_LOGW(TAG, "device/code: %d %s", st, buf);
        emit_text(ILF_CLOUD_EV_DISK, "Яндекс не ответил, попробуй ещё");
        cJSON_Delete(j);
        free(buf);
        return;
    }
    char device_code[80];
    snprintf(device_code, sizeof device_code, "%s", dev);
    cJSON *iv = cJSON_GetObjectItem(j, "interval"), *ex = cJSON_GetObjectItem(j, "expires_in");
    int interval = cJSON_IsNumber(iv) ? (int)iv->valuedouble : 5;
    int expires = cJSON_IsNumber(ex) ? (int)ex->valuedouble : 300;
    ilf_cloud_event_t e = { .type = ILF_CLOUD_EV_DISK_CODE };
    const char *u = url ? url : "ya.ru/device";
    if (!strncmp(u, "https://", 8)) u += 8;
    snprintf(e.url, sizeof e.url, "%s", u);
    snprintf(e.code, sizeof e.code, "%s", code);
    emit(&e);
    cJSON_Delete(j);

    bool linked = false;
    for (int waited = 0; waited < expires && !linked; waited += interval) {
        vTaskDelay(pdMS_TO_TICKS(interval * 1000));
        snprintf(body, sizeof body, "grant_type=device_code&code=%s&client_id=%s&client_secret=%s",
                 device_code, client_id(), client_secret());
        st = http(HTTP_METHOD_POST, OAUTH "/token", NULL, "application/x-www-form-urlencoded", body, buf, 1024);
        cJSON *r = cJSON_Parse(buf);
        if (st == 200 && r && take_tokens(r)) linked = true;
        const char *err = r ? jstr(r, "error") : NULL;
        bool pending = err && (!strcmp(err, "authorization_pending") || !strcmp(err, "slow_down"));
        cJSON_Delete(r);
        if (!linked && !pending) break;
    }
    ilf_cloud_event_t hide = { .type = ILF_CLOUD_EV_DISK_CODE };
    emit(&hide);
    if (linked) emit_text(ILF_CLOUD_EV_DISK, "привязан");
    else emit_text(ILF_CLOUD_EV_DISK, "не привязан · код не подтвердили");
    free(buf);
}

static bool refresh_token(void)
{
    if ((int32_t)(s_tok_exp - (uint32_t)time(NULL)) > 24 * 3600) return true;
    if (!s_rtok[0]) return false;
    char body[256], buf[768];
    snprintf(body, sizeof body, "grant_type=refresh_token&refresh_token=%s&client_id=%s&client_secret=%s",
             s_rtok, client_id(), client_secret());
    int st = http(HTTP_METHOD_POST, OAUTH "/token", NULL, "application/x-www-form-urlencoded", body, buf, sizeof buf);
    cJSON *r = st == 200 ? cJSON_Parse(buf) : NULL;
    bool ok = r && take_tokens(r);
    cJSON_Delete(r);
    return ok;
}

/* ---------- выгрузка ---------- */

typedef struct { esp_http_client_handle_t c; size_t sent; } put_t;

static bool put_sink(const void *d, size_t n, void *user)
{
    put_t *p = user;
    if (esp_http_client_write(p->c, d, (int)n) != (int)n) return false;
    p->sent += n;
    return true;
}

static bool upload_one(const ilf_upload_t *u, const char *auth)
{
    char path[200], url[320], *buf = malloc(1024);
    if (!buf) return false;
    char full[140];
    snprintf(full, sizeof full, "app:/%s/%s", u->voice ? "Голосовые" : "Рисунки", u->name);
    urlenc(full, path, sizeof path);
    snprintf(url, sizeof url, DISK_API "/resources/upload?overwrite=true&path=%s", path);
    int st = http(HTTP_METHOD_GET, url, auth, NULL, NULL, buf, 1024);
    cJSON *j = st == 200 ? cJSON_Parse(buf) : NULL;
    const char *href = j ? jstr(j, "href") : NULL;
    bool ok = false;
    if (href) {
        esp_http_client_config_t cfg = { .url = href, .method = HTTP_METHOD_PUT, .timeout_ms = 30000,
                                         .crt_bundle_attach = esp_crt_bundle_attach };
        put_t p = { .c = esp_http_client_init(&cfg) };
        if (p.c && esp_http_client_open(p.c, (int)u->size) == ESP_OK) {
            if (ilf_voice_read_upload(u, put_sink, &p) && p.sent == u->size) {
                esp_http_client_fetch_headers(p.c);
                int code = esp_http_client_get_status_code(p.c);
                ok = code == 201 || code == 202 || code == 200;
                if (!ok) ESP_LOGW(TAG, "PUT %s: %d", u->name, code);
            }
        }
        if (p.c) { esp_http_client_close(p.c); esp_http_client_cleanup(p.c); }
    } else {
        ESP_LOGW(TAG, "upload link %s: %d %s", u->name, st, buf);
    }
    cJSON_Delete(j);
    free(buf);
    return ok;
}

static void make_folders(const char *auth)
{
    static const char *dirs[] = { "app:/Голосовые", "app:/Рисунки" };
    for (int i = 0; i < 2; i++) {
        char p[80], url[200];
        urlenc(dirs[i], p, sizeof p);
        snprintf(url, sizeof url, DISK_API "/resources?path=%s", p);
        http(HTTP_METHOD_PUT, url, auth, NULL, NULL, NULL, 0);   /* 409 — уже есть, это нормально */
    }
}

static void do_sync(bool by_hand)
{
    int pending = ilf_voice_pending_uploads();
    if (!pending) {
        if (by_hand) emit_text(ILF_CLOUD_EV_DISK, "привязан · всё выгружено");
        return;
    }
    if (!wifi_up(s_ssid, s_pass)) {
        emit_text(ILF_CLOUD_EV_DISK, "привязан · %d ждут Wi-Fi", pending);
        return;
    }
    if (!refresh_token()) {
        wifi_down();
        s_tok[0] = 0;
        save();
        emit_text(ILF_CLOUD_EV_DISK, "не привязан · привяжи снова");
        return;
    }
    char auth[110];
    snprintf(auth, sizeof auth, "OAuth %s", s_tok);
    make_folders(auth);
    int done = 0;
    ilf_upload_t u;
    while (done < MAX_PER_SYNC && ilf_voice_next_upload(&u)) {
        emit_text(ILF_CLOUD_EV_DISK, "выгружаю %d из %d…", done + 1, pending);
        if (!upload_one(&u, auth)) break;
        ilf_voice_uploaded(&u);
        done++;
    }
    wifi_down();
    int left = ilf_voice_pending_uploads();
    ESP_LOGI(TAG, "на Диск ушло %d, осталось %d", done, left);
    if (left) emit_text(ILF_CLOUD_EV_DISK, "привязан · %d ждут", left);
    else emit_text(ILF_CLOUD_EV_DISK, "привязан · всё выгружено");
    ilf_cloud_event_t e = { .type = ILF_CLOUD_EV_SYNCED };
    emit(&e);
}

/* ---------- задача ---------- */

static void status_now(void)
{
    if (s_ssid[0]) emit_text(ILF_CLOUD_EV_WIFI, "%s", s_ssid);
    else emit_text(ILF_CLOUD_EV_WIFI, "не задан");
    int pending = ilf_voice_pending_uploads();
    if (!client_id()[0]) emit_text(ILF_CLOUD_EV_DISK, "нужен ключ приложения");
    else if (!s_tok[0]) emit_text(ILF_CLOUD_EV_DISK, "не привязан");
    else if (pending) emit_text(ILF_CLOUD_EV_DISK, "привязан · %d ждут", pending);
    else emit_text(ILF_CLOUD_EV_DISK, "привязан · всё выгружено");
}

static void cloud_task(void *arg)
{
    (void)arg;
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_cmd, &c, portMAX_DELAY) != pdTRUE) continue;
        s_busy = true;
        switch (c.type) {
        case C_SCAN:
            do_scan();
            break;
        case C_JOIN:
            emit_text(ILF_CLOUD_EV_WIFI, "%s · проверяю…", c.ssid);
            if (wifi_up(c.ssid, c.pass)) {
                wifi_down();
                snprintf(s_ssid, sizeof s_ssid, "%s", c.ssid);
                snprintf(s_pass, sizeof s_pass, "%s", c.pass);
                save();
                emit_text(ILF_CLOUD_EV_WIFI, "%s · ок", s_ssid);
            } else {
                emit_text(ILF_CLOUD_EV_WIFI, "%s · не подключилось", c.ssid);
            }
            break;
        case C_FORGET:
            s_ssid[0] = s_pass[0] = 0;
            save();
            emit_text(ILF_CLOUD_EV_WIFI, "не задан");
            break;
        case C_DISK_TAP:
            if (!client_id()[0]) { emit_text(ILF_CLOUD_EV_DISK, "нужен ключ приложения"); break; }
            if (!s_ssid[0]) { emit_text(ILF_CLOUD_EV_DISK, "сначала Wi-Fi"); break; }
            if (s_tok[0]) { do_sync(true); break; }
            if (!wifi_up(s_ssid, s_pass)) { emit_text(ILF_CLOUD_EV_DISK, "нет Wi-Fi"); break; }
            disk_link();
            wifi_down();
            if (s_tok[0]) do_sync(true);
            break;
        case C_SYNC:
            if (ilf_cloud_ready()) do_sync(false);
            break;
        }
        s_busy = false;
    }
}

static void send_cmd(ccmd_t type, const char *ssid, const char *pass)
{
    if (!s_cmd) return;
    cmd_t c = { .type = type };
    if (ssid) snprintf(c.ssid, sizeof c.ssid, "%s", ssid);
    if (pass) snprintf(c.pass, sizeof c.pass, "%s", pass);
    xQueueSend(s_cmd, &c, 0);
}

void ilf_cloud_scan(void) { send_cmd(C_SCAN, NULL, NULL); }
void ilf_cloud_join(const char *ssid, const char *pass) { send_cmd(C_JOIN, ssid, pass); }
void ilf_cloud_forget(void) { send_cmd(C_FORGET, NULL, NULL); }
void ilf_cloud_disk_tap(void) { send_cmd(C_DISK_TAP, NULL, NULL); }
void ilf_cloud_sync(void) { if (!s_busy && uxQueueMessagesWaiting(s_cmd) == 0) send_cmd(C_SYNC, NULL, NULL); }
bool ilf_cloud_ready(void) { return s_ssid[0] && s_tok[0] && client_id()[0]; }
bool ilf_cloud_busy(void) { return s_busy; }
void ilf_cloud_refresh_status(void) { if (s_ev) status_now(); }

void ilf_cloud_init(const char *device_name)
{
    snprintf(s_dev_name, sizeof s_dev_name, "ILF %s", device_name ? device_name : "");
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_dev_id, sizeof s_dev_id, "ilf-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    load();
    s_bits = xEventGroupCreate();
    s_cmd = xQueueCreate(4, sizeof(cmd_t));
    s_ev = xQueueCreate(12, sizeof(ilf_cloud_event_t *));
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    xTaskCreatePinnedToCore(cloud_task, "cloud", 12288, NULL, 3, NULL, 0);
    status_now();
}
