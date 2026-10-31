#include "ilf_net.h"

#include <string.h>

#include "esp_app_format.h"
#include "esp_event.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "ilf_version.h"

static const char *TAG = "ilf_net";

#define CHANNEL 1
/* общий ключ PMK — им ESP-NOW шифрует ключи друзей; сами ключи у каждой пары свои */
static const uint8_t PMK[16] = { 'I', 'L', 'F', '-', 'H', 'E', 'R', 'D', '-', 'P', 'M', 'K', '-', 'v', '0', '2' };

typedef struct {
    bool sent;           /* это отчёт об отправке, а не пакет */
    bool ok;
    uint8_t mac[6];
    uint8_t len;
    uint8_t data[ILF_HERD_MAX_FRAME];
} net_msg_t;

static QueueHandle_t s_q;
static ilf_herd_t *s_herd;
static ilf_net_event_fn s_fn;
static void *s_user;
static char s_name[ILF_HERD_NAME_LEN];
static uint32_t s_now;
static bool s_listen_wanted = true, s_listening = true;
static uint32_t s_awake_until;
static volatile bool s_got_packet;

/* обновление от друзей */
static ilf_ota_t s_ota;
static ilf_net_ota_fn s_ota_fn;
static void *s_ota_user;
static const esp_partition_t *s_run_part, *s_new_part;
static esp_ota_handle_t s_ota_h;

/* Отчёты ESP-NOW об отправке приходят по порядку отправки, но не говорят, чья
   это была отправка. Стаду важны только его собственные, поэтому помечаем. */
#define TAGS 64
static uint8_t s_tags[TAGS];
static volatile unsigned s_tag_head, s_tag_tail;
static portMUX_TYPE s_tag_mux = portMUX_INITIALIZER_UNLOCKED;
enum { TAG_HERD = 0, TAG_OTA = 1 };

static void tag_push(uint8_t t)
{
    portENTER_CRITICAL(&s_tag_mux);
    if (s_tag_head - s_tag_tail < TAGS) s_tags[s_tag_head++ % TAGS] = t;
    portEXIT_CRITICAL(&s_tag_mux);
}

static uint8_t tag_pop(void)
{
    uint8_t t = TAG_HERD;
    portENTER_CRITICAL_ISR(&s_tag_mux);
    if (s_tag_tail != s_tag_head) t = s_tags[s_tag_tail++ % TAGS];
    portEXIT_CRITICAL_ISR(&s_tag_mux);
    return t;
}

/* ---------- радио (колбэки из задачи Wi-Fi → очередь) ---------- */

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len <= 0 || len > ILF_HERD_MAX_FRAME) return;
    s_got_packet = true;
    net_msg_t m = { .sent = false, .len = (uint8_t)len };
    memcpy(m.mac, info->src_addr, 6);
    memcpy(m.data, data, (size_t)len);
    xQueueSend(s_q, &m, 0);
}

static void sent_cb(const uint8_t *mac, esp_now_send_status_t st)
{
    (void)mac;
    if (tag_pop() == TAG_OTA) return;   /* куски прошивки: отчёт не нужен, получатель переспросит сам */
    net_msg_t m = { .sent = true, .ok = st == ESP_NOW_SEND_SUCCESS };
    xQueueSend(s_q, &m, 0);
}

static void io_send(void *ctx, const uint8_t *mac, const uint8_t *data, size_t len)
{
    (void)ctx;
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    tag_push(TAG_HERD);
    if (esp_now_send(mac ? mac : bcast, data, len) != ESP_OK) {
        portENTER_CRITICAL(&s_tag_mux);
        if (s_tag_head != s_tag_tail) s_tag_head--;
        portEXIT_CRITICAL(&s_tag_mux);
        net_msg_t m = { .sent = true, .ok = false };
        xQueueSend(s_q, &m, 0);
    }
}

static void io_add_peer(void *ctx, const uint8_t mac[6], const uint8_t key[16])
{
    (void)ctx;
    esp_now_peer_info_t p = { .channel = CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = true };
    memcpy(p.peer_addr, mac, 6);
    memcpy(p.lmk, key, 16);
    if (esp_now_is_peer_exist(mac)) esp_now_mod_peer(&p);
    else if (esp_now_add_peer(&p) != ESP_OK) ESP_LOGW(TAG, "не удалось добавить друга в радио");
}

static void io_remove_peer(void *ctx, const uint8_t mac[6])
{
    (void)ctx;
    esp_now_del_peer(mac);
}

/* ---------- криптография ---------- */

static int rng(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

static void io_keypair(void *ctx, uint8_t priv[32], uint8_t pub[32])
{
    (void)ctx;
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    mbedtls_ecdh_gen_public(&grp, &d, &q, rng, NULL);
    mbedtls_mpi_write_binary_le(&d, priv, 32);
    mbedtls_mpi_write_binary_le(&q.MBEDTLS_PRIVATE(X), pub, 32);
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
}

static bool io_shared(void *ctx, const uint8_t priv[32], const uint8_t pub[32], uint8_t out[32])
{
    (void)ctx;
    mbedtls_ecp_group grp;
    mbedtls_mpi d, z;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&q);
    int r = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (!r) r = mbedtls_mpi_read_binary_le(&d, priv, 32);
    if (!r) r = mbedtls_mpi_read_binary_le(&q.MBEDTLS_PRIVATE(X), pub, 32);
    if (!r) r = mbedtls_mpi_lset(&q.MBEDTLS_PRIVATE(Z), 1);
    if (!r) r = mbedtls_ecdh_compute_shared(&grp, &z, &q, &d, rng, NULL);
    if (!r) r = mbedtls_mpi_write_binary_le(&z, out, 32);
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return r == 0;
}

static void io_sha256(void *ctx, const uint8_t *data, size_t len, uint8_t out[32])
{
    (void)ctx;
    mbedtls_sha256(data, len, out, 0);
}

/* ---------- друзья в NVS ---------- */

static void save_friends(void)
{
    ilf_friend_t list[ILF_HERD_MAX_FRIENDS];
    int n = ilf_herd_friend_count(s_herd);
    for (int i = 0; i < n; i++) list[i] = *ilf_herd_friend(s_herd, i);
    nvs_handle_t h;
    if (nvs_open("herd", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "friends", list, sizeof(ilf_friend_t) * (size_t)n);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "друзей сохранено: %d", n);
}

static void load_friends(void)
{
    ilf_friend_t list[ILF_HERD_MAX_FRIENDS];
    size_t size = sizeof list;
    nvs_handle_t h;
    if (nvs_open("herd", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "friends", list, &size) == ESP_OK && size % sizeof(ilf_friend_t) == 0) {
        ilf_herd_load_friends(s_herd, list, (int)(size / sizeof(ilf_friend_t)));
    }
    nvs_close(h);
    ESP_LOGI(TAG, "друзей в стаде: %d", ilf_herd_friend_count(s_herd));
}

static void io_event(void *ctx, const ilf_herd_event_t *ev)
{
    (void)ctx;
    if (ev->type == ILF_HERD_EV_SAVE) save_friends();
    if (s_fn) s_fn(ev, s_user);
}

/* ---------- обновление прошивки от друга ---------- */

static bool ota_send(void *u, const uint8_t mac[6], const uint8_t *d, size_t n)
{
    (void)u;
    tag_push(TAG_OTA);
    if (esp_now_send(mac, d, n) == ESP_OK) return true;
    portENTER_CRITICAL(&s_tag_mux);
    if (s_tag_head != s_tag_tail) s_tag_head--;
    portEXIT_CRITICAL(&s_tag_mux);
    return false;
}

static bool ota_read(void *u, uint32_t off, uint8_t *buf, size_t n)
{
    (void)u;
    return esp_partition_read(s_run_part, off, buf, n) == ESP_OK;
}

static bool ota_begin(void *u, uint32_t size)
{
    (void)u;
    s_new_part = esp_ota_get_next_update_partition(NULL);
    if (!s_new_part || size > s_new_part->size) return false;
    ESP_LOGI(TAG, "принимаю прошивку %lu байт в %s", (unsigned long)size, s_new_part->label);
    return esp_ota_begin(s_new_part, size, &s_ota_h) == ESP_OK;
}

static bool ota_write(void *u, const uint8_t *d, size_t n)
{
    (void)u;
    return esp_ota_write(s_ota_h, d, n) == ESP_OK;
}

static bool ota_finish(void *u, const uint8_t sha[32])
{
    (void)u;
    /* esp_ota_end сам проверяет образ по встроенному хешу; дальше сверяем этот хеш с обещанным другом */
    if (esp_ota_end(s_ota_h) != ESP_OK) { ESP_LOGW(TAG, "образ от друга битый"); return false; }
    uint8_t got[32];
    if (esp_partition_get_sha256(s_new_part, got) != ESP_OK || memcmp(got, sha, 32) != 0) {
        ESP_LOGW(TAG, "хеш прошивки не совпал");
        return false;
    }
    return esp_ota_set_boot_partition(s_new_part) == ESP_OK;
}

static void ota_abort(void *u)
{
    (void)u;
    esp_ota_abort(s_ota_h);
}

static void ota_event(void *u, const ilf_ota_event_t *ev)
{
    (void)u;
    if (s_ota_fn) s_ota_fn(ev, s_ota_user);
}

static void ota_init(void)
{
    ilf_ota_image_t mine = { .version = ILF_FW_VERSION };
    s_run_part = esp_ota_get_running_partition();
    esp_image_metadata_t md;
    const esp_partition_pos_t pos = { .offset = s_run_part->address, .size = s_run_part->size };
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) == ESP_OK &&
        esp_partition_get_sha256(s_run_part, mine.sha) == ESP_OK) {
        mine.size = md.image_len;
    } else {
        ESP_LOGW(TAG, "свою прошивку не прочитать — раздавать друзьям не будем");
    }
    ilf_ota_io_t io = { ota_send, ota_read, ota_begin, ota_write, ota_finish, ota_abort, ota_event, NULL };
    ilf_ota_init(&s_ota, &io, &mine);
    ESP_LOGI(TAG, "ILF OS v%d, образ %lu байт", ILF_FW_VERSION, (unsigned long)mine.size);
}

void ilf_net_set_ota_cb(ilf_net_ota_fn fn, void *user) { s_ota_fn = fn; s_ota_user = user; }
void ilf_net_ota_answer(int answer)
{
    if (answer > 0) ilf_ota_accept(&s_ota, s_now);
    else ilf_ota_decline(&s_ota, answer == 0);
}

/* ---------- запуск ---------- */

ilf_herd_t *ilf_net_init(ilf_net_event_fn fn, void *user)
{
    s_fn = fn;
    s_user = user;
    s_q = xQueueCreate(40, sizeof(net_msg_t));

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();   /* для домашнего Wi-Fi (ilf_cloud) */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
    ESP_ERROR_CHECK(esp_now_init());
    esp_now_set_pmk(PMK);
    esp_now_register_recv_cb(recv_cb);
    esp_now_register_send_cb(sent_cb);
    /* экономный режим радио: окно приёма раз в секунду (включается в ilf_net_poll) */
    esp_now_set_wake_window(ILF_NET_WAKE_INTERVAL);   /* сначала слушаем целиком */
    esp_wifi_connectionless_module_set_wake_interval(ILF_NET_WAKE_INTERVAL);
    /* рядом работает Bluetooth: Wi-Fi обязан быть в modem sleep, поэтому «слушать
       всё время» = окно приёма во весь интервал, а не выключенный сон */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_now_peer_info_t b = { .channel = CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = false };
    memset(b.peer_addr, 0xFF, 6);
    esp_now_add_peer(&b);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_name, sizeof s_name, "Барашек %02X%02X", mac[4], mac[5]);

    ilf_herd_io_t io = {
        .send = io_send, .add_peer = io_add_peer, .remove_peer = io_remove_peer,
        .keypair = io_keypair, .shared = io_shared, .sha256 = io_sha256, .event = io_event,
    };
    s_herd = ilf_herd_create(&io, mac, s_name);
    load_friends();
    ota_init();
    ESP_LOGI(TAG, "стадо готово: %s, канал %d", s_name, CHANNEL);
    return s_herd;
}

void ilf_net_poll(uint32_t now_ms)
{
    s_now = now_ms;
    net_msg_t m;
    while (xQueueReceive(s_q, &m, 0) == pdTRUE) {
        if (m.sent) ilf_herd_on_sent(s_herd, m.ok, now_ms);
        else if (ilf_herd_find_friend(s_herd, m.mac) >= 0) {
            /* друг рядом: сказать ему свою версию (раз в 10 минут) */
            ilf_ota_seen(&s_ota, m.mac, now_ms);
            if (m.data[0] == ILF_OTA_MAGIC) ilf_ota_on_recv(&s_ota, m.mac, m.data, m.len, now_ms);
            else ilf_herd_on_recv(s_herd, m.mac, m.data, m.len, now_ms);
        } else if (m.data[0] != ILF_OTA_MAGIC) {
            ilf_herd_on_recv(s_herd, m.mac, m.data, m.len, now_ms);
        }
    }
    ilf_herd_tick(s_herd, now_ms);
    ilf_ota_tick(&s_ota, now_ms);

    if (s_got_packet) {
        s_got_packet = false;
        s_awake_until = now_ms + ILF_NET_STAY_AWAKE_MS;
    }
    bool listen = s_listen_wanted || ilf_herd_sending(s_herd) || ilf_ota_busy(&s_ota) || ilf_herd_pairing(s_herd) ||
                  (int32_t)(s_awake_until - now_ms) > 0;
    if (listen != s_listening) {
        s_listening = listen;
        esp_now_set_wake_window(listen ? ILF_NET_WAKE_INTERVAL : ILF_NET_WAKE_WINDOW);
        ESP_LOGI(TAG, "%s", listen ? "радио слушает" : "радио дремлет");
    }
}

void ilf_net_keep_listening(bool on) { s_listen_wanted = on; }

const char *ilf_net_my_name(void) { return s_name; }
