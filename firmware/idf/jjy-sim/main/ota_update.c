/*
 * JJY-SIM - HTTPS OTA Update
 *
 * Checks firmware releases and installs updates over HTTPS.
 * - Provides the Japanese update page and progress/status endpoints
 * - Requires user action before writing an OTA image
 * - Maintains the persistent update notification
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 *
 * Developed by Shachi-lab
 * https://shachi-lab.com
 *   ____  _                _     _     _       _
 *  / ___)| |__   __ _  ___| |__ |_|   | | __ _| |__
 *  \___ \| '_ \ / _` |/ __) '_ \ _  _ | |/ _` | '_ \
 *   ___) | | | | (_| | (__| | | | ||_|| | (_| | |_) |
 *  (____/|_| |_|\__,_|\___)_| |_|_|   |_|\__,_|_.__/
 */
/* HTTPS OTA: checking never writes Flash; installation requires a separate POST. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"
#include "ota_update.h"
#include "wifi_mng.h"
#include "wifi_mng_def.h"
#include "settings.h"
#include "jjy-sim.h"

typedef struct {
    char version[32];
    char url[512];
    unsigned char sha256[32];
    size_t size;
    bool withdrawn;
} release_t;
static release_t release;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static bool busy, available;
static bool notice;
static char notified_version[32];
static int64_t next_check_us;
#define UPDATE_CHECK_INTERVAL_US (24LL * 60 * 60 * 1000000)
static char state[24] = "idle", message[160] = "「最新版を確認」を押してください。";
static unsigned progress;

static void publish(const char *s, const char *m, unsigned p)
{
    portENTER_CRITICAL(&lock);
    strlcpy(state, s, sizeof(state));
    strlcpy(message, m, sizeof(message));
    progress = p;
    portEXIT_CRITICAL(&lock);
}

bool ota_update_busy(void)
{
    portENTER_CRITICAL(&lock);
    bool result = busy;
    portEXIT_CRITICAL(&lock);
    return result;
}

/* Only plain numeric major.minor.patch releases are accepted, including local version. */
static bool version_parse(const char *s, unsigned v[3])
{
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s)) return false;
        unsigned n = 0;
        do {
            n = n * 10 + (*s++ - '0');
            if (n > 65535) return false;
        } while (isdigit((unsigned char)*s));
        v[i] = n;
        if (i < 2 && *s++ != '.') return false;
    }
    return *s == 0;
}

static bool newer(const char *candidate)
{
    unsigned a[3], b[3];
    if (!version_parse(candidate, a) || !version_parse(esp_app_get_description()->version, b)) return false;
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

bool ota_update_notice(void)
{
    portENTER_CRITICAL(&lock);
    bool result = notice;
    portEXIT_CRITICAL(&lock);
    return result;
}

void ota_update_init(void)
{
    nvs_handle_t handle;
    if (nvs_open("ota_update", NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(notified_version);
        if (nvs_get_str(handle, "notice", notified_version, &len) != ESP_OK)
            notified_version[0] = 0;
        nvs_close(handle);
    }
    // A notification about firmware already installed has been fulfilled.
    notice = newer(notified_version);
}

/* Call only after a valid manifest. Network/parse failures never revoke a notice. */
static void update_notice(const release_t *candidate, bool found)
{
    // A successfully validated manifest can explicitly confirm that no update exists.
    // A cancellation only applies to the exact release which raised the notice.
    if (candidate->withdrawn && strcmp(candidate->version, notified_version)) return;
    const char *version = found ? candidate->version : "";
    portENTER_CRITICAL(&lock);
    bool changed = strcmp(notified_version, version) != 0;
    notice = *version != 0;
    portEXIT_CRITICAL(&lock);
    if (!changed) return;
    nvs_handle_t handle;
    if (nvs_open("ota_update", NVS_READWRITE, &handle) != ESP_OK) return;
    esp_err_t err = nvs_set_str(handle, "notice", version);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&lock);
        strlcpy(notified_version, version, sizeof(notified_version));
        portEXIT_CRITICAL(&lock);
    }
}

#if defined(CONFIG_IDF_TARGET_ESP32C2)
#define OTA_HTTP_BUFFER_SIZE 1024
#define OTA_DOWNLOAD_BUFFER_SIZE 1024
#else
#define OTA_HTTP_BUFFER_SIZE 2048
#define OTA_DOWNLOAD_BUFFER_SIZE 4096
#endif
#define OTA_RX_IDLE_TIMEOUT_US (60LL * 1000000)

static const char *TAG = "ota_update";

static void ota_log_memory(const char *stage)
{
    ESP_LOGI(TAG, "%s: heap=%u largest=%u minimum=%u stack_free=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void ota_log_http_error(esp_http_client_handle_t h, int result, size_t offset, size_t expected)
{
    int tls_code = 0, tls_flags = 0;
    int socket_errno = esp_http_client_get_errno(h);
    esp_err_t tls_error = esp_http_client_get_and_clear_last_tls_error(h, &tls_code, &tls_flags);
    ESP_LOGE(TAG, "HTTP read failed: result=%d errno=%d tls=%d code=%d flags=%d at %u/%u bytes",
             result, socket_errno, (int)tls_error, tls_code, tls_flags,
             (unsigned)offset, (unsigned)expected);
    ota_log_memory("HTTP read failure");
}

static int ota_read_retry(esp_http_client_handle_t h, unsigned char *buffer, size_t length,
                          int64_t *last_data_us, size_t offset, size_t expected)
{
    for (;;) {
        int n = esp_http_client_read(h, (char *)buffer, length);
        if (n != -ESP_ERR_HTTP_EAGAIN) {
            if (n > 0) *last_data_us = esp_timer_get_time();
            return n;
        }
        if (esp_timer_get_time() - *last_data_us >= OTA_RX_IDLE_TIMEOUT_US) {
            ESP_LOGE(TAG, "Receive idle limit reached at %u/%u bytes", (unsigned)offset, (unsigned)expected);
            return n;
        }
        ESP_LOGW(TAG, "Receive timeout; waiting again at %u/%u bytes",
                 (unsigned)offset, (unsigned)expected);
        ota_log_memory("HTTP receive wait");
        if (expected) {
            publish("downloading", "通信の応答を待っています。電源を切らずにお待ちください。",
                    offset * 100 / expected);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static bool https_url(const char *s)
{
    if (strncmp(s, "https://", 8) || !s[8] || s[8] == '/') return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p <= 32 || *p == 127 || *p == '@' || *p == '#' || *p == '\\') return false;
    return true;
}

static esp_http_client_handle_t open_https(const char *url)
{
    if (!https_url(url)) return NULL;
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 15000, .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true, .buffer_size = OTA_HTTP_BUFFER_SIZE,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { ota_log_memory("HTTP client allocation failed"); return NULL; }
    if (esp_http_client_open(h, 0) != ESP_OK || esp_http_client_fetch_headers(h) < 0 ||
        esp_http_client_get_status_code(h) != 200) {
        ESP_LOGE(TAG, "HTTP connection/header failed: status=%d", esp_http_client_get_status_code(h));
        ota_log_http_error(h, ESP_FAIL, 0, 0);
        esp_http_client_cleanup(h);
        return NULL;
    }
    return h;
}

static bool json_text(cJSON *root, const char *name, char *out, size_t cap)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring) >= cap) return false;
    strlcpy(out, v->valuestring, cap);
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *fetch_release(release_t *out)
{
    char url[640];
    const char *base = CONFIG_JJYSIM_UPDATE_BASE_URL;
    int n = snprintf(url, sizeof(url), "%s%smanifest-latest.json", base,
                     *base && base[strlen(base)-1] == '/' ? "" : "/");
    if (n < 0 || n >= sizeof(url)) return "更新先URLが長すぎます。";
    esp_http_client_handle_t h = open_https(url);
    if (!h) return "更新情報を取得できません。接続先URLや通信状態を確認してください。";
    int64_t content_length = esp_http_client_get_content_length(h);
    size_t capacity = !esp_http_client_is_chunked_response(h) && content_length > 0 && content_length < 4096
                      ? (size_t)content_length : 4096;
    char *body = calloc(1, capacity + 1);
    if (!body) {
        ota_log_memory("Manifest buffer allocation failed");
        esp_http_client_cleanup(h);
        return "メモリが不足しています（更新情報の受信）。";
    }
    size_t used = 0;
    int got = 0;
    int64_t last_data_us = esp_timer_get_time();
    while (used < capacity && (got = ota_read_retry(h, (unsigned char *)body + used,
                                                   capacity - used, &last_data_us, used, 0)) > 0) used += got;
    bool complete = used < 4096 && esp_http_client_is_complete_data_received(h);
    if (!complete) ota_log_http_error(h, got, used, capacity);
    esp_http_client_cleanup(h);
    cJSON *root = complete ? cJSON_ParseWithOpts(body, NULL, true) : NULL;
    free(body);
    char project[32], digest[65];
    cJSON *builds = cJSON_GetObjectItemCaseSensitive(root, "builds");
    cJSON *build = cJSON_GetObjectItemCaseSensitive(builds, CONFIG_IDF_TARGET);
    cJSON *files = cJSON_GetObjectItemCaseSensitive(build, "files");
    // Common manifests use files.app. Flat app fields remain readable for older data.
    cJSON *app = files ? cJSON_GetObjectItemCaseSensitive(files, "app") : build;
    cJSON *size = cJSON_GetObjectItemCaseSensitive(app, "size");
    cJSON *withdrawn = cJSON_GetObjectItemCaseSensitive(build, "withdrawn");
    unsigned parts[3];
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    // Missing or invalid chip entries are errors, never an authoritative "no update".
    bool valid = cJSON_IsObject(root) && cJSON_IsObject(builds) && cJSON_IsObject(build) && cJSON_IsObject(app) &&
        json_text(root, "project", project, sizeof(project)) &&
        !strcmp(project, esp_app_get_description()->project_name) &&
        json_text(build, "version", out->version, sizeof(out->version)) && version_parse(out->version, parts) &&
        json_text(app, "url", out->url, sizeof(out->url)) && https_url(out->url) &&
        json_text(app, "sha256", digest, sizeof(digest)) && strlen(digest) == 64 &&
        (!withdrawn || cJSON_IsBool(withdrawn)) &&
        cJSON_IsNumber(size) && size->valuedouble >= 256 && slot &&
        size->valuedouble <= slot->size && size->valuedouble == (size_t)size->valuedouble;
    if (valid) {
        out->size = (size_t)size->valuedouble;
        out->withdrawn = cJSON_IsTrue(withdrawn);
        for (int i = 0; i < 32; i++) {
            int a = hex_digit(digest[i*2]), b = hex_digit(digest[i*2+1]);
            if (a < 0 || b < 0) { valid = false; break; }
            out->sha256[i] = (a << 4) | b;
        }
    }
    cJSON_Delete(root);
    return valid ? NULL : "更新情報が不正か、対象機種が異なるか、ファイルが大きすぎます。";
}

static const char *install_release(const release_t *r)
{
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!slot || slot == esp_ota_get_running_partition() || r->size > slot->size)
        return "OTA用の領域がありません。先にUSBで移行用ファームウェアを書き込んでください。";
    ota_log_memory("Download start");
    publish("downloading", "ファームウェアをダウンロード中です。電源を切らないでください。", 0);
    esp_http_client_handle_t h = open_https(r->url);
    if (!h) return "ファームウェアを取得できません。接続先URLや通信状態を確認してください。";
    int64_t length = esp_http_client_get_content_length(h);
    if (!esp_http_client_is_chunked_response(h) && length >= 0 && length != r->size) {
        esp_http_client_cleanup(h); return "ファームウェアのサイズが更新情報と一致しません。";
    }
    unsigned char *buf = malloc(OTA_DOWNLOAD_BUFFER_SIZE);
    if (!buf) {
        ota_log_memory("Download buffer allocation failed");
        esp_http_client_cleanup(h);
        return "メモリが不足しています（FWの受信）。";
    }
    ota_log_memory("Download connected");
    esp_ota_handle_t handle = 0;
    bool active = false;
    const char *error = "ファームウェアの取得または検証に失敗しました。現在のファームウェアを維持します。";
    size_t total = 0;
    esp_err_t err = ESP_OK;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0) goto done;
    publish("preparing", "更新領域を準備中です。電源を切らないでください。", 0);
    err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        ota_log_memory("OTA begin failure");
        error = "更新領域の準備に失敗しました。現在のファームウェアを維持します。";
        goto done;
    }
    active = true;
    publish("downloading", "ファームウェアをダウンロード中です。電源を切らないでください。", 0);
    int64_t last_data_us = esp_timer_get_time();
    while (total < r->size) {
        size_t count = r->size - total;
        if (count > OTA_DOWNLOAD_BUFFER_SIZE) count = OTA_DOWNLOAD_BUFFER_SIZE;
        int n = ota_read_retry(h, buf, count, &last_data_us, total, r->size);
        if (n <= 0) {
            ota_log_http_error(h, n, total, r->size);
            error = n == -ESP_ERR_HTTP_EAGAIN ?
                "受信がタイムアウトしました。Wi-Fiの状態を確認して再試行してください。" :
                "ファームウェアの受信が中断されました。現在のファームウェアを維持します。";
            goto done;
        }
        if (mbedtls_sha256_update(&sha, buf, n) != 0) {
            ESP_LOGE(TAG, "SHA-256 update failed at %u bytes", (unsigned)total);
            goto done;
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %u bytes: %s", (unsigned)total, esp_err_to_name(err));
            ota_log_memory("OTA write failure");
            error = "更新領域への書き込みに失敗しました。現在のファームウェアを維持します。";
            goto done;
        }
        total += n;
        publish("downloading", "ファームウェアをダウンロード中です。電源を切らないでください。", total * 100 / r->size);
    }
    /* Read to the real HTTP end, rejecting trailing bytes and truncated chunked transfers. */
    int tail = ota_read_retry(h, buf, 1, &last_data_us, total, r->size);
    if (tail != 0 || !esp_http_client_is_complete_data_received(h)) {
        ota_log_http_error(h, tail, total, r->size);
        goto done;
    }
    publish("verifying", "ファームウェアを検証中です。電源を切らないでください。", 100);
    unsigned char digest[32];
    if (mbedtls_sha256_finish(&sha, digest) != 0 || memcmp(digest, r->sha256, 32)) {
        ESP_LOGE(TAG, "Downloaded SHA-256 does not match manifest");
        goto done;
    }
    err = esp_ota_end(handle);
    active = false; /* end frees the handle on both success and failure. */
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err)); goto done; }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(slot, &desc) != ESP_OK ||
        strncmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name)) ||
        strncmp(desc.version, r->version, sizeof(desc.version))) goto done;
    /* esp_ota_end validates the image, including chip compatibility, before selecting it. */
    err = esp_ota_set_boot_partition(slot);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err)); goto done; }
    error = NULL;
done:
    if (active) esp_ota_abort(handle);
    mbedtls_sha256_free(&sha);
    free(buf);
    esp_http_client_cleanup(h);
    ota_log_memory(error ? "Download failed" : "Download complete");
    return error;
}

static void update_task(void *arg)
{
    bool background = (uintptr_t)arg == 2;
    bool install = (uintptr_t)arg == 1;
    const char *error = NULL;
    bool network = false;
    ota_log_memory("Update worker start");
    release_t *candidate = calloc(1, sizeof(*candidate));
    if (!candidate) { error = "メモリが不足しています（更新処理の準備）。"; goto done; }
    if (!esp_ota_get_next_update_partition(NULL)) {
        error = "OTA用の領域がありません。先にUSBで移行用ファームウェアを書き込んでください。"; goto done;
    }
    if (!background) {
        publish("connecting", "保存済みWi-Fiに接続中です（最大30秒）...", 0);
        network = true;
        if (wifi_update_connect() != ESP_OK) { error = "Wi-Fiに接続できません。接続可能なWi-Fi設定を先に保存してください。"; goto done; }
    }
    /* Setup mode stops the normal SNTP client. TLS verification still needs valid UTC. */
    if (!background && time(NULL) < 1704067200) {
        publish("syncing", "安全なダウンロードのため時刻を同期中です（最大20秒）...", 0);
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, s_settings.ntp_mode && s_settings.ntp_url[0] ? s_settings.ntp_url : NTP_URL);
        esp_sntp_init();
        for (int i = 0; i < 20 && time(NULL) < 1704067200; i++) vTaskDelay(pdMS_TO_TICKS(1000));
        esp_sntp_stop();
        if (time(NULL) < 1704067200) { error = "時刻同期に失敗したため、HTTPS証明書を確認できません。"; goto done; }
    }
    if (!install) {
        publish("checking", "サーバーの更新情報を確認中です...", 0);
        error = fetch_release(candidate);
        if (!error) {
            bool found = !candidate->withdrawn && newer(candidate->version);
            portENTER_CRITICAL(&lock);
            release = *candidate;
            available = found;
            portEXIT_CRITICAL(&lock);
            update_notice(candidate, found);
            publish(found ? "available" : "current", found ? "新しいファームウェアがあります。「最新版に更新」で更新を開始します。" :
                    candidate->withdrawn ? "このアップデートは配信元によって取り消されました。" : "新しいファームウェアはありません。", 0);
        }
    } else {
        portENTER_CRITICAL(&lock);
        *candidate = release; /* Install exactly the release the user checked. */
        portEXIT_CRITICAL(&lock);
        error = install_release(candidate);
        if (!error) {
            publish("rebooting", "更新内容を確認しました。再起動します...", 100);
            free(candidate);
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_wifi_stop();
            esp_restart();
        }
    }
done:
    if (network) wifi_update_disconnect();
    free(candidate);
    if (error) publish("error", error, 0);
    portENTER_CRITICAL(&lock);
    busy = false;
    portEXIT_CRITICAL(&lock);
    vTaskDelete(NULL);
}

void ota_update_poll(void)
{
    // The caller has completed normal startup/time sync and keeps the saved STA running.
    if (wifi_get_status() != WIFI_STATUS_CONNECTED) return;
    int64_t now = esp_timer_get_time();
    if (now < next_check_us) return;
    portENTER_CRITICAL(&lock);
    bool accepted = !busy;
    if (accepted) busy = true;
    portEXIT_CRITICAL(&lock);
    if (!accepted) return;
    // Even failed checks wait a day; do not hammer an unavailable update service.
    next_check_us = now + UPDATE_CHECK_INTERVAL_US;
    publish("checking", "サーバーの更新情報を確認中です...", 0);
    if (xTaskCreate(update_task, "ota_check", 8192, (void *)2, 1, NULL) != pdPASS) {
        publish("error", "更新の確認を開始できません。", 0);
        portENTER_CRITICAL(&lock); busy = false; portEXIT_CRITICAL(&lock);
    }
}

static esp_err_t launch(httpd_req_t *req, bool install)
{
    /* Custom header requires same-origin JS; cross-origin forms cannot initiate Flash writes. */
    char header[8];
    if (httpd_req_get_hdr_value_str(req, "X-JJY-Update", header, sizeof(header)) != ESP_OK || strcmp(header, "1"))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "アップデート画面から操作してください。");
    portENTER_CRITICAL(&lock);
    bool accepted = !busy && (!install || available);
    if (accepted) {
        busy = true;
        if (!install) available = false;
    }
    portEXIT_CRITICAL(&lock);
    if (!accepted) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "処理中か、更新の確認がまだ完了していません。");
    }
    publish(install ? "installing" : "checking", install ? "アップデートを開始しました。接続を準備しています..." : "最新版の確認を開始しました...", 0);
    ota_log_memory("Update launch");
    if (xTaskCreate(update_task, "ota_update", 8192, install ? (void *)1 : NULL, 4, NULL) != pdPASS) {
        ota_log_memory("Update task allocation failed");
        publish("error", "アップデート処理を開始できません。", 0);
        portENTER_CRITICAL(&lock); busy = false; portEXIT_CRITICAL(&lock);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "メモリが不足しています。");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

esp_err_t ota_update_check(httpd_req_t *req) { return launch(req, false); }
esp_err_t ota_update_start(httpd_req_t *req) { return launch(req, true); }

esp_err_t ota_update_status(httpd_req_t *req)
{
    char s[24], m[160], v[32];
    bool b, a;
    unsigned p;
    portENTER_CRITICAL(&lock);
    strlcpy(s, state, sizeof(s)); strlcpy(m, message, sizeof(m));
    strlcpy(v, release.version, sizeof(v)); b = busy; a = available; p = progress;
    portEXIT_CRITICAL(&lock);
    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j, "state", s); cJSON_AddStringToObject(j, "message", m);
    cJSON_AddStringToObject(j, "current", esp_app_get_description()->version);
    cJSON_AddStringToObject(j, "latest", v);
    cJSON_AddBoolToObject(j, "busy", b); cJSON_AddBoolToObject(j, "available", a);
    cJSON_AddNumberToObject(j, "progress", p);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

esp_err_t ota_update_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req,
        "<!doctype html><html lang=\"ja\"><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
        "<title>JJY-SIM アップデート</title><style>\n"
        "body{font:16px sans-serif;max-width:520px;margin:24px auto;padding:16px}#versions{white-space:pre-line}\n"
        WIFI_AP_BUTTON_CSS_STYLE
        "button:disabled{opacity:.6}progress{width:100%}\n"
        ".spinner{display:inline-block;width:16px;height:16px;border:3px solid #ddd;border-top-color:#246;border-radius:50%;animation:spin 1s linear infinite;vertical-align:middle;margin-right:8px}\n"
        "@keyframes spin{to{transform:rotate(360deg)}}@media(prefers-reduced-motion:reduce){.spinner{animation:none}}\n"
        "</style><h1>JJY-SIM アップデート</h1>\n"
        "<p>保存済みのWi-Fi設定で接続します。設定を変更した場合は、先に保存してください。</p>\n"
        "<p>まず「最新版を確認」を押してください。新しい版が見つかったら、更新ボタンでダウンロードと書き込みを開始できます。</p>\n"
        "<p>JJY-SIMの設定用Wi-Fiに接続したまま操作してください。更新後は自動で再起動します。アップデート中は電源を切らないでください。</p>\n"
        "<p id=\"versions\"></p><p id=\"status\" role=\"status\" aria-live=\"polite\">読み込み中...</p>\n"
        "<p id=\"activity\" hidden><span class=\"spinner\" aria-hidden=\"true\"></span><span id=\"activityText\">処理中...</span></p>\n"
        "<progress id=\"progress\" aria-label=\"ファームウェアのダウンロード進捗\" max=\"100\" value=\"0\" hidden></progress><p id=\"percent\" hidden></p>\n"
        "<button id=\"check\" type=\"button\">最新版を確認</button>\n"
        "<button id=\"install\" type=\"button\" hidden>最新版に更新</button>\n"
        "<p><a href=\"/\">設定画面に戻る</a></p><script>\n"
        "let pending=false,polling=false,requestError='',requestMessage='';\n"
        "const status=document.getElementById('status'),check=document.getElementById('check'),install=document.getElementById('install'),\n"
        "bar=document.getElementById('progress'),activity=document.getElementById('activity'),activityText=document.getElementById('activityText'),percent=document.getElementById('percent');\n"
        "const phases={connecting:'Wi-Fi接続中...',syncing:'時刻同期中...',checking:'最新版の確認中...',installing:'更新の準備中...',downloading:'ダウンロード中...',verifying:'更新内容の検証中...',rebooting:'再起動中...'};\n"
        "async function poll(){\n"
        " if(polling)return;polling=true;const controller=new AbortController();const timeout=setTimeout(()=>controller.abort(),8000);\n"
        " try{\n"
        "  const r=await fetch('/update/status',{cache:'no-store',signal:controller.signal});if(!r.ok)throw Error();const s=await r.json();\n"
        "  if(s.busy)requestError='';\n"
        "  status.textContent=pending&&!s.busy?requestMessage:requestError||s.message;\n"
        "  document.getElementById('versions').textContent='現在のバージョン：'+s.current+(s.latest?'\\n配信版バージョン：'+s.latest:'');\n"
        "  const working=pending||s.busy;activity.hidden=!working;activityText.textContent=s.busy?(phases[s.state]||'処理中...'):'本体に要求を送信中...';bar.hidden=!working;\n"
        "  percent.hidden=!s.busy||s.state!=='downloading';\n"
        "  if(s.busy&&s.state==='downloading'){bar.value=s.progress;percent.textContent=s.progress+'% ダウンロード済み';}else{bar.removeAttribute('value');}\n"
        "  check.disabled=working;install.disabled=working||!!requestError;install.hidden=!s.available;\n"
        " }catch(e){\n"
        "  status.textContent='接続が途切れました。端末の応答を待っています。設定用Wi-Fiへの接続を保ち、再起動後は設定画面を開き直してください。';\n"
        "  activity.hidden=false;activityText.textContent='端末の応答を待っています...';bar.hidden=true;percent.hidden=true;check.disabled=true;install.disabled=true;\n"
        " }finally{clearTimeout(timeout);polling=false;}\n"
        "}\n"
        "async function run(action){\n"
        " if(pending)return;pending=true;requestError='';\n"
        " requestMessage=action==='start'?'アップデートの開始を本体に要求しています...':'最新版の確認を本体に要求しています...';\n"
        " check.disabled=true;install.disabled=true;status.textContent=requestMessage;\n"
        " activity.hidden=false;activityText.textContent='本体に要求を送信中...';bar.hidden=false;bar.removeAttribute('value');percent.hidden=true;\n"
        " const controller=new AbortController();const timeout=setTimeout(()=>controller.abort(),8000);\n"
        " try{\n"
        "  const r=await fetch('/update/'+action,{method:'POST',headers:{'X-JJY-Update':'1'},signal:controller.signal});\n"
        "  if(!r.ok){const reason=await r.text();requestError=(action==='start'?'更新を開始できませんでした：':'最新版の確認を開始できませんでした：')+reason;}\n"
        "  else{const result=await r.json();if(result.accepted!==true)throw Error();}\n"
        " }catch(e){requestError='本体が要求を受け付けたか確認できません。状態を確認しています。開始していない場合は「最新版を確認」からやり直してください。';}\n"
        " finally{clearTimeout(timeout);pending=false;}\n"
        " await poll();\n"
        "}\n"
        "check.addEventListener('click',()=>run('check'));\n"
        "install.addEventListener('click',()=>run('start'));\n"
        "poll();setInterval(poll,2000);\n"
        "</script></html>\n");
}

void ota_update_confirm_boot(void)
{
    esp_ota_img_states_t s;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &s) == ESP_OK && s == ESP_OTA_IMG_PENDING_VERIFY)
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
}
