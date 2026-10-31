/*
 * iPhone по Bluetooth LE: NimBLE-периферия + GATT-клиент к службам Apple.
 *
 * Порядок после подключения:
 *   шифрование (код на экране при первом разе) → поиск служб CTS/AMS/ANCS →
 *   поиск характеристик → по очереди «работы»: прочитать часовой пояс и время,
 *   подписаться на время, на ANCS (сперва Data Source, потом Notification
 *   Source — так требует Apple), на AMS и выбрать, о чём AMS будет сообщать.
 *
 * Колбэки NimBLE идут в его собственном потоке; наружу — только очередь событий.
 */
#include "ilf_ble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

void ble_store_config_init(void);

/* ---------- UUID служб Apple (байты задом наперёд, как хранит NimBLE) ---------- */

static const ble_uuid128_t ANCS_SVC = BLE_UUID128_INIT(
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4, 0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79);
static const ble_uuid128_t ANCS_NS = BLE_UUID128_INIT(
    0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C, 0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F);
static const ble_uuid128_t ANCS_CP = BLE_UUID128_INIT(
    0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98, 0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69);
static const ble_uuid128_t ANCS_DS = BLE_UUID128_INIT(
    0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE, 0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22);
static const ble_uuid128_t AMS_SVC = BLE_UUID128_INIT(
    0xDC, 0xF8, 0x55, 0xAD, 0x02, 0xC5, 0xF4, 0x8E, 0x3A, 0x43, 0x36, 0x0F, 0x2B, 0x50, 0xD3, 0x89);
static const ble_uuid128_t AMS_RC = BLE_UUID128_INIT(
    0xC2, 0x51, 0xCA, 0xF7, 0x56, 0x0E, 0xDF, 0xB8, 0x8A, 0x4A, 0xB1, 0x57, 0xD8, 0x81, 0x3C, 0x9B);
static const ble_uuid128_t AMS_EU = BLE_UUID128_INIT(
    0x02, 0xC1, 0x96, 0xBA, 0x92, 0xBB, 0x0C, 0x9A, 0x1F, 0x41, 0x8D, 0x80, 0xCE, 0xAB, 0x7C, 0x2F);
static const ble_uuid16_t CTS_SVC = BLE_UUID16_INIT(0x1805);
static const ble_uuid16_t CTS_TIME = BLE_UUID16_INIT(0x2A2B);
static const ble_uuid16_t CTS_ZONE = BLE_UUID16_INIT(0x2A0F);
static const ble_uuid16_t CCCD = BLE_UUID16_INIT(0x2902);

/* ---------- состояние ---------- */

typedef struct { uint16_t start, end; } range_t;
typedef struct { uint16_t val, end; } chr_t;

enum { C_ANCS_NS, C_ANCS_CP, C_ANCS_DS, C_AMS_RC, C_AMS_EU, C_CTS_TIME, C_CTS_ZONE, C_COUNT };
enum { S_ANCS, S_AMS, S_CTS, S_COUNT };

/* работы после поиска — по одной, каждая зовёт следующую из своего колбэка */
typedef enum {
    J_READ_ZONE, J_READ_TIME, J_SUB_TIME, J_SUB_ANCS_DS, J_SUB_ANCS_NS,
    J_SUB_AMS, J_AMS_TRACK, J_AMS_PLAYER, J_PARAMS, J_DONE,
} job_t;

#define FAST_ADV_MS 30000
#define UID_QUEUE   8

static struct {
    QueueHandle_t q;
    char name[32];
    uint8_t own_addr_type;
    uint16_t conn;
    bool connected, encrypted;
    ilf_ble_state_t state;
    range_t svc[S_COUNT];
    chr_t chr[C_COUNT];
    int svc_i;             /* какую службу сейчас разбираем на характеристики */
    chr_t *last_chr;       /* у последней найденной характеристики ещё не известен конец */
    job_t job;
    char tz[32];
    ilf_ams_state_t music;
    ilf_ancs_rx_t rx;
    struct { uint32_t uid; uint8_t cat; } pend[UID_QUEUE];
    int pend_n;
    bool ancs_busy;
    uint8_t busy_cat;
} b = { .conn = BLE_HS_CONN_HANDLE_NONE };

static void post(const ilf_ble_event_t *ev)
{
    if (b.q) xQueueSend(b.q, ev, 0);
}

static void set_state(ilf_ble_state_t s)
{
    if (b.state == s) return;
    b.state = s;
    post(&(ilf_ble_event_t){ .type = ILF_BLE_EV_STATE, .state = s });
}

static size_t mbuf_flat(const struct os_mbuf *om, uint8_t *buf, size_t cap)
{
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(om, buf, (uint16_t)cap, &len) != 0 && len == 0) return 0;
    return len;
}

/* ---------- реклама ---------- */

static int gap_event(struct ble_gap_event *ev, void *arg);

static void advertise(bool fast)
{
    if (b.connected) return;
    ble_gap_adv_stop();

    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    /* «ищу службу уведомлений Apple» — так iPhone показывает барашка в настройках Bluetooth */
    f.sol_uuids128 = &ANCS_SVC;
    f.sol_num_uuids128 = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) ESP_LOGW(TAG, "реклама: %d", rc);

    struct ble_hs_adv_fields r = { 0 };
    r.name = (const uint8_t *)b.name;
    r.name_len = (uint8_t)strlen(b.name);
    r.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);

    /* Apple советует: первые 30 с — 152.5 мс, потом 1022.5 мс */
    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = p.itvl_max = fast ? 244 : 1636;   /* в единицах 0.625 мс */
    rc = ble_gap_adv_start(b.own_addr_type, NULL, fast ? FAST_ADV_MS : BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "не начать рекламу: %d", rc);
    if (b.state != ILF_BLE_PAIRING) set_state(ILF_BLE_WAITING);
}

/* ---------- время ---------- */

static void apply_time(const uint8_t *d, size_t len)
{
    struct tm local;
    if (!ilf_cts_parse_time(d, len, &local)) return;
    time_t epoch = mktime(&local);   /* TZ уже выставлен из Local Time Information */
    struct timeval tv = { .tv_sec = epoch };
    settimeofday(&tv, NULL);
    ilf_ble_event_t ev = { .type = ILF_BLE_EV_TIME };
    snprintf(ev.tz, sizeof ev.tz, "%s", b.tz);
    post(&ev);
    ESP_LOGI(TAG, "время с iPhone: %02d:%02d, пояс %s", local.tm_hour, local.tm_min, b.tz[0] ? b.tz : "прежний");
}

static void apply_zone(const uint8_t *d, size_t len)
{
    char tz[32];
    if (!ilf_cts_parse_zone(d, len, tz, sizeof tz, NULL)) return;
    snprintf(b.tz, sizeof b.tz, "%s", tz);
    setenv("TZ", tz, 1);
    tzset();
}

/* ---------- ANCS: по одному запросу подробностей за раз ---------- */

static int ancs_cp_done(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg);

static void ancs_next(void)
{
    if (b.ancs_busy || !b.pend_n || !b.encrypted) return;
    uint32_t uid = b.pend[0].uid;
    b.busy_cat = b.pend[0].cat;
    memmove(b.pend, b.pend + 1, sizeof b.pend[0] * (size_t)(--b.pend_n));
    uint8_t cmd[16];
    size_t n = ilf_ancs_get_attributes(uid, cmd, sizeof cmd);
    ilf_ancs_rx_reset(&b.rx);
    if (ble_gattc_write_flat(b.conn, b.chr[C_ANCS_CP].val, cmd, (uint16_t)n, ancs_cp_done, NULL) == 0) {
        b.ancs_busy = true;
    }
}

static int ancs_cp_done(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err->status != 0) {   /* уведомление уже исчезло — берём следующее */
        b.ancs_busy = false;
        ancs_next();
    }
    return 0;
}

static void on_ancs_source(const uint8_t *d, size_t len)
{
    ilf_ancs_source_t s;
    if (!ilf_ancs_parse_source(d, len, &s)) return;
    if (s.event == ILF_ANCS_REMOVED) {
        post(&(ilf_ble_event_t){ .type = ILF_BLE_EV_NOTE_GONE, .uid = s.uid });
        return;
    }
    /* старые (висели до подключения), беззвучные и изменения — не показываем */
    if (s.event != ILF_ANCS_ADDED || s.preexisting || s.silent) return;
    if (b.pend_n == UID_QUEUE) memmove(b.pend, b.pend + 1, sizeof b.pend[0] * (size_t)(--b.pend_n));
    b.pend[b.pend_n].uid = s.uid;
    b.pend[b.pend_n].cat = s.category;
    b.pend_n++;
    ancs_next();
}

static void on_ancs_data(const uint8_t *d, size_t len)
{
    ilf_ble_event_t ev = { .type = ILF_BLE_EV_NOTE };
    bool got = ilf_ancs_rx_feed(&b.rx, d, len, &ev.note);
    while (got) {
        ev.category = b.busy_cat;
        post(&ev);
        b.ancs_busy = false;
        got = b.rx.len && ilf_ancs_rx_feed(&b.rx, d, 0, &ev.note);
    }
    ancs_next();
}

/* ---------- работы после поиска ---------- */

static void run_job(void);

static void next_job(void)
{
    b.job++;
    run_job();
}

static int on_read(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)arg;
    if (err->status == 0 && attr) {
        uint8_t buf[16];
        size_t n = mbuf_flat(attr->om, buf, sizeof buf);
        if (b.job == J_READ_ZONE) apply_zone(buf, n);
        else if (b.job == J_READ_TIME) apply_time(buf, n);
    }
    next_job();
    return 0;
}

static int on_write(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err->status != 0) ESP_LOGW(TAG, "работа %d: запись не прошла (%d)", b.job, err->status);
    next_job();
    return 0;
}

/* подписка: найти дескриптор 0x2902 у характеристики и записать туда 1 */
static uint16_t s_cccd;

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val; (void)arg;
    if (err->status == 0 && dsc) {
        if (ble_uuid_cmp(&dsc->uuid.u, &CCCD.u) == 0) s_cccd = dsc->handle;
        return 0;
    }
    if (s_cccd) {
        static const uint8_t on[2] = { 1, 0 };
        if (ble_gattc_write_flat(conn, s_cccd, on, 2, on_write, NULL) == 0) return 0;
    }
    next_job();
    return 0;
}

static bool subscribe(int c)
{
    chr_t *ch = &b.chr[c];
    if (!ch->val) return false;
    s_cccd = 0;
    if (ch->end <= ch->val) {   /* дескрипторов нет — попробуем стандартное место */
        s_cccd = ch->val + 1;
        static const uint8_t on[2] = { 1, 0 };
        return ble_gattc_write_flat(b.conn, s_cccd, on, 2, on_write, NULL) == 0;
    }
    return ble_gattc_disc_all_dscs(b.conn, ch->val, ch->end, on_dsc, NULL) == 0;
}

static void run_job(void)
{
    for (;;) {
        bool started = false;
        uint8_t cmd[4];
        switch (b.job) {
        case J_READ_ZONE:
            started = b.chr[C_CTS_ZONE].val && ble_gattc_read(b.conn, b.chr[C_CTS_ZONE].val, on_read, NULL) == 0;
            break;
        case J_READ_TIME:
            started = b.chr[C_CTS_TIME].val && ble_gattc_read(b.conn, b.chr[C_CTS_TIME].val, on_read, NULL) == 0;
            break;
        case J_SUB_TIME:    started = subscribe(C_CTS_TIME); break;
        case J_SUB_ANCS_DS: started = subscribe(C_ANCS_DS); break;
        case J_SUB_ANCS_NS: started = subscribe(C_ANCS_NS); break;
        case J_SUB_AMS:     started = subscribe(C_AMS_EU); break;
        case J_AMS_TRACK:
            started = b.chr[C_AMS_EU].val &&
                      ble_gattc_write_flat(b.conn, b.chr[C_AMS_EU].val, cmd, (uint16_t)ilf_ams_subscribe_track(cmd), on_write, NULL) == 0;
            break;
        case J_AMS_PLAYER:
            started = b.chr[C_AMS_EU].val &&
                      ble_gattc_write_flat(b.conn, b.chr[C_AMS_EU].val, cmd, (uint16_t)ilf_ams_subscribe_player(cmd), on_write, NULL) == 0;
            break;
        case J_PARAMS: {
            /* бережём батарею: интервал 30–50 мс и пропуск до 4 событий (правила Apple соблюдены) */
            struct ble_gap_upd_params p = { .itvl_min = 24, .itvl_max = 40, .latency = 4,
                                            .supervision_timeout = 500, .min_ce_len = 0, .max_ce_len = 0 };
            ble_gap_update_params(b.conn, &p);
            break;
        }
        case J_DONE:
            ESP_LOGI(TAG, "iPhone готов: ANCS %s, AMS %s, CTS %s",
                     b.chr[C_ANCS_NS].val ? "да" : "нет", b.chr[C_AMS_EU].val ? "да" : "нет",
                     b.chr[C_CTS_TIME].val ? "да" : "нет");
            set_state(ILF_BLE_CONNECTED);
            return;
        }
        if (started) return;
        b.job++;
    }
}

/* ---------- поиск служб и характеристик ---------- */

static void disc_next_svc(void);

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;
    range_t *svc = &b.svc[b.svc_i];
    if (err->status == 0 && chr) {
        if (b.last_chr) { b.last_chr->end = chr->def_handle - 1; b.last_chr = NULL; }
        int c = -1;
        const ble_uuid_t *u = &chr->uuid.u;
        if (b.svc_i == S_ANCS) {
            if (!ble_uuid_cmp(u, &ANCS_NS.u)) c = C_ANCS_NS;
            else if (!ble_uuid_cmp(u, &ANCS_CP.u)) c = C_ANCS_CP;
            else if (!ble_uuid_cmp(u, &ANCS_DS.u)) c = C_ANCS_DS;
        } else if (b.svc_i == S_AMS) {
            if (!ble_uuid_cmp(u, &AMS_RC.u)) c = C_AMS_RC;
            else if (!ble_uuid_cmp(u, &AMS_EU.u)) c = C_AMS_EU;
        } else {
            if (!ble_uuid_cmp(u, &CTS_TIME.u)) c = C_CTS_TIME;
            else if (!ble_uuid_cmp(u, &CTS_ZONE.u)) c = C_CTS_ZONE;
        }
        if (c >= 0) {
            b.chr[c].val = chr->val_handle;
            b.chr[c].end = svc->end;
            b.last_chr = &b.chr[c];
        }
        return 0;
    }
    b.last_chr = NULL;
    b.svc_i++;
    disc_next_svc();
    return 0;
}

static void disc_next_svc(void)
{
    while (b.svc_i < S_COUNT && !b.svc[b.svc_i].start) b.svc_i++;
    if (b.svc_i >= S_COUNT) {
        b.job = J_READ_ZONE;
        run_job();
        return;
    }
    range_t *s = &b.svc[b.svc_i];
    if (ble_gattc_disc_all_chrs(b.conn, s->start, s->end, on_chr, NULL) != 0) {
        b.svc_i++;
        disc_next_svc();
    }
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn; (void)arg;
    if (err->status == 0 && svc) {
        int i = -1;
        if (!ble_uuid_cmp(&svc->uuid.u, &ANCS_SVC.u)) i = S_ANCS;
        else if (!ble_uuid_cmp(&svc->uuid.u, &AMS_SVC.u)) i = S_AMS;
        else if (!ble_uuid_cmp(&svc->uuid.u, &CTS_SVC.u)) i = S_CTS;
        if (i >= 0) { b.svc[i].start = svc->start_handle; b.svc[i].end = svc->end_handle; }
        return 0;
    }
    b.svc_i = 0;
    disc_next_svc();
    return 0;
}

static void start_discovery(void)
{
    memset(b.svc, 0, sizeof b.svc);
    memset(b.chr, 0, sizeof b.chr);
    b.pend_n = 0;
    b.ancs_busy = false;
    ilf_ancs_rx_reset(&b.rx);
    if (ble_gattc_disc_all_svcs(b.conn, on_svc, NULL) != 0) ESP_LOGW(TAG, "не начать поиск служб");
}

/* ---------- один iPhone ---------- */

static void forget_others(const ble_addr_t *keep)
{
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, MYNEWT_VAL(BLE_STORE_MAX_BONDS)) != 0) return;
    for (int i = 0; i < n; i++) {
        if (ble_addr_cmp(&peers[i], keep) != 0) ble_gap_unpair(&peers[i]);
    }
}

/* ---------- события GAP ---------- */

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) { advertise(true); break; }
        b.conn = ev->connect.conn_handle;
        b.connected = true;
        b.encrypted = false;
        /* службы Apple открываются только после шифрования — просим его сразу */
        ble_gap_security_initiate(b.conn);
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "iPhone отключился (%d)", ev->disconnect.reason);
        b.connected = b.encrypted = false;
        b.conn = BLE_HS_CONN_HANDLE_NONE;
        b.music = (ilf_ams_state_t){ 0 };
        b.state = ILF_BLE_OFF;   /* чтобы WAITING точно ушло наружу */
        advertise(true);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!b.connected) advertise(false);
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (ev->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000 };
            ble_sm_inject_io(ev->passkey.conn_handle, &io);
            set_state(ILF_BLE_PAIRING);
            post(&(ilf_ble_event_t){ .type = ILF_BLE_EV_PASSKEY, .passkey = io.passkey });
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ev->enc_change.status != 0) {
            ESP_LOGW(TAG, "шифрование не вышло (%d)", ev->enc_change.status);
            ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        b.encrypted = true;
        if (ble_gap_conn_find(ev->enc_change.conn_handle, &desc) == 0) forget_others(&desc.peer_id_addr);
        start_discovery();
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* iPhone забыл барашка и знакомится заново — стираем старый ключ */
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t buf[256];
        size_t n = mbuf_flat(ev->notify_rx.om, buf, sizeof buf);
        uint16_t h = ev->notify_rx.attr_handle;
        if (h == b.chr[C_ANCS_NS].val) on_ancs_source(buf, n);
        else if (h == b.chr[C_ANCS_DS].val) on_ancs_data(buf, n);
        else if (h == b.chr[C_AMS_EU].val) {
            if (ilf_ams_parse(&b.music, buf, n)) {
                post(&(ilf_ble_event_t){ .type = ILF_BLE_EV_MUSIC, .music = b.music });
            }
        } else if (h == b.chr[C_CTS_TIME].val) apply_time(buf, n);
        break;
    }
    default:
        break;
    }
    return 0;
}

/* ---------- запуск ---------- */

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &b.own_addr_type);
    advertise(true);
}

static void on_reset(int reason) { ESP_LOGW(TAG, "стек BLE перезапущен (%d)", reason); }

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ilf_ble_init(const char *name)
{
    b.q = xQueueCreate(8, sizeof(ilf_ble_event_t));
    snprintf(b.name, sizeof b.name, "%s", name && *name ? name : "ILF");
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "Bluetooth не поднялся"); return; }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    /* у барашка экран без клавиатуры: показываем код, вводят на iPhone */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(b.name);
    ble_svc_gap_device_appearance_set(0x00C0);   /* «часы» — iPhone рисует значок часов */
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}

void ilf_ble_set_name(const char *name)
{
    if (!name || !*name) return;
    snprintf(b.name, sizeof b.name, "%s", name);
    ble_svc_gap_device_name_set(b.name);
    if (!b.connected && ble_hs_synced()) advertise(true);
}

bool ilf_ble_poll(ilf_ble_event_t *ev)
{
    return b.q && xQueueReceive(b.q, ev, 0) == pdTRUE;
}

bool ilf_ble_connected(void) { return b.state == ILF_BLE_CONNECTED; }

void ilf_ble_music(ilf_ams_cmd_t cmd)
{
    if (!b.encrypted || !b.chr[C_AMS_RC].val) return;
    uint8_t c = (uint8_t)cmd;
    ble_gattc_write_flat(b.conn, b.chr[C_AMS_RC].val, &c, 1, NULL, NULL);
}

void ilf_ble_forget(void)
{
    if (b.connected) ble_gap_terminate(b.conn, BLE_ERR_REM_USER_CONN_TERM);
    ble_store_clear();
}
