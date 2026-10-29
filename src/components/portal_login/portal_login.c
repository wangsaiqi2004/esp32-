/* Uplink captive-portal auto-login for ePortal (Dr.COM / city hotspot).
 *
 * See portal_login.h for the motivation. Protocol was derived from the campus
 * portal's own front-end script (eportal/interface/.../AuthInterFace.js):
 *
 *   POST <base>login      Content-Type: application/x-www-form-urlencoded
 *        userId=&password=&service=&queryString=&operatorPwd=
 *        &operatorUserId=&validcode=&passwordEncrypt=false
 *   POST <base>keepalive  userIndex=<from login response>
 *
 * where <base> defaults to http://172.16.29.11:8080/eportal/InterFace.do?method=
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "portal_login.h"

static const char *TAG = "portal";

#define NVS_NS            "portal"
#define DEF_URL           "http://172.16.29.11:8080/eportal/InterFace.do?method="
#define DEF_INTERVAL      240       /* keepalive period, seconds */
#define LOGIN_RETRY_MS    45000     /* base gap between login attempts */
#define LOGIN_RETRY_MAX_MS 600000   /* backoff ceiling (10 min) */
#define CHECK_OK_MS       30000     /* reachability poll when online */
#define CHECK_FAIL_MS     10000     /* reachability poll when offline */

/* Two independent 204 endpoints; campus portals usually cannot fake both. */
static const char *PROBE_URLS[] = {
    "http://connect.rom.miui.com/generate_204",
    "http://wifi.vivo.com.cn/generate_204",
};

static bool      s_enabled;
static char      s_url[192]  = DEF_URL;
static char      s_user[64];
static char      s_pass[64];
static int       s_interval  = DEF_INTERVAL;
static char      s_userindex[192];
static int64_t   s_last_login_ms;      /* esp_timer, ms */
static int       s_fail_streak;        /* consecutive failed logins */
static int64_t   s_last_keepalive_ms;
static bool      s_last_ok;
static char      s_last_msg[96] = "idle";

/* ---------------------------------------------------------------- NVS --- */

static void load_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;                     /* first boot: defaults apply */
    }
    size_t len;
    uint8_t en = 0;

    if (nvs_get_u8(h, "en", &en) == ESP_OK) s_enabled = en != 0;

    len = sizeof(s_url);
    if (nvs_get_str(h, "url", s_url, &len) != ESP_OK) strcpy(s_url, DEF_URL);

    len = sizeof(s_user);
    if (nvs_get_str(h, "user", s_user, &len) != ESP_OK) s_user[0] = '\0';

    len = sizeof(s_pass);
    if (nvs_get_str(h, "pass", s_pass, &len) != ESP_OK) s_pass[0] = '\0';

    uint16_t iv = 0;
    if (nvs_get_u16(h, "iv", &iv) == ESP_OK && iv >= 60) s_interval = iv;

    nvs_close(h);
}

static void save_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_u8(h, "en", s_enabled ? 1 : 0);
    nvs_set_str(h, "url", s_url);
    nvs_set_str(h, "user", s_user);
    nvs_set_str(h, "pass", s_pass);
    nvs_set_u16(h, "iv", (uint16_t)s_interval);
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------ helpers --- */

static void url_encode(const char *src, char *dst, size_t dst_len)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 4 < dst_len; p++) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= 'a' && *p <= 'z') || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            dst[o++] = (char)*p;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[*p >> 4];
            dst[o++] = hex[*p & 0x0F];
        }
    }
    dst[o] = '\0';
}

static bool sta_ip(char *buf, size_t len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return false;
    if (info.ip.addr == 0) return false;
    snprintf(buf, len, IPSTR, IP2STR(&info.ip));
    return true;
}

/* Minimal HTTP call. Returns status code, or -1 on transport error.
 * Body bytes received are copied into out (NUL terminated) when out != NULL. */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} body_ctx_t;

static esp_err_t http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data != NULL) {
        body_ctx_t *ctx = (body_ctx_t *)e->user_data;
        if (ctx->len + e->data_len < ctx->cap) {
            memcpy(ctx->buf + ctx->len, e->data, e->data_len);
            ctx->len += e->data_len;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

static int http_call(const char *url, const char *body, char *out, size_t out_len, int timeout_ms)
{
    body_ctx_t ctx = { .buf = out, .cap = out_len, .len = 0 };
    if (out && out_len) out[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .event_handler = http_event,
        .user_data = out ? &ctx : NULL,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return -1;

    if (body != NULL) {
        esp_http_client_set_method(c, HTTP_METHOD_POST);
        esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded; charset=UTF-8");
        esp_http_client_set_post_field(c, body, (int)strlen(body));
    }

    int status = -1;
    esp_err_t err = esp_http_client_perform(c);
    if (err == ESP_OK) status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return status;
}

static bool internet_ok(void)
{
    for (size_t i = 0; i < sizeof(PROBE_URLS) / sizeof(PROBE_URLS[0]); i++) {
        int st = http_call(PROBE_URLS[i], NULL, NULL, 0, 4000);
        if (st == 204) return true;
        /* Some portals answer 200 with a login page - that is NOT internet. */
    }
    return false;
}

/* ------------------------------------------------------------- login --- */

static bool do_login(void)
{
    char ip[20];
    if (!sta_ip(ip, sizeof(ip))) {
        snprintf(s_last_msg, sizeof(s_last_msg), "no uplink IP yet");
        return false;
    }
    if (s_user[0] == '\0') {
        snprintf(s_last_msg, sizeof(s_last_msg), "username not set");
        return false;
    }

    char euser[192], epass[192];
    url_encode(s_user, euser, sizeof(euser));
    url_encode(s_pass, epass, sizeof(epass));

    char url[256];
    snprintf(url, sizeof(url), "%s%s", s_url, "login");

    /* queryString value is itself url-encoded: wlanuserip=<ip>&wlanacname=&nasip=&ssid= */
    char body[640];
    snprintf(body, sizeof(body),
             "userId=%s&password=%s&service=&queryString=wlanuserip%%3D%s"
             "%%26wlanacname%%3D%%26nasip%%3D%%26ssid%%3D"
             "&operatorPwd=&operatorUserId=&validcode=&passwordEncrypt=false",
             euser, epass, ip);

    char resp[512];
    int st = http_call(url, body, resp, sizeof(resp), 8000);

    if (st != 200) {
        snprintf(s_last_msg, sizeof(s_last_msg), "login HTTP %d", st);
        ESP_LOGW(TAG, "%s", s_last_msg);
        return false;
    }

    if (strstr(resp, "\"result\":\"success\"") == NULL &&
        strstr(resp, "\"result\":\"ok\"") == NULL) {
        /* Keep the portal's own message short for the UI. */
        snprintf(s_last_msg, sizeof(s_last_msg), "login rejected: %.60s", resp);
        ESP_LOGW(TAG, "%s", s_last_msg);
        return false;
    }

    /* Pull userIndex out for keepalive. */
    s_userindex[0] = '\0';
    const char *k = strstr(resp, "\"userIndex\":\"");
    if (k != NULL) {
        k += strlen("\"userIndex\":\"");
        size_t n = 0;
        while (k[n] && k[n] != '"' && n + 1 < sizeof(s_userindex)) {
            s_userindex[n] = k[n];
            n++;
        }
        s_userindex[n] = '\0';
    }

    s_last_keepalive_ms = esp_timer_get_time() / 1000;
    s_fail_streak = 0;
    snprintf(s_last_msg, sizeof(s_last_msg), "login OK");
    ESP_LOGI(TAG, "portal login OK, userIndex=%s", s_userindex[0] ? "got" : "missing");
    return true;
}

static void do_keepalive(void)
{
    if (s_userindex[0] == '\0') return;

    char url[256];
    snprintf(url, sizeof(url), "%s%s", s_url, "keepalive");

    char eidx[256];
    url_encode(s_userindex, eidx, sizeof(eidx));

    char body[320];
    snprintf(body, sizeof(body), "userIndex=%s", eidx);

    int st = http_call(url, body, NULL, 0, 6000);
    s_last_keepalive_ms = esp_timer_get_time() / 1000;

    if (st != 200) {
        snprintf(s_last_msg, sizeof(s_last_msg), "keepalive HTTP %d", st);
        ESP_LOGW(TAG, "%s", s_last_msg);
    }
}

/* -------------------------------------------------------------- task --- */

static void portal_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(8000));      /* let the uplink settle first */

    for (;;) {
        int delay_ms = CHECK_OK_MS;

        if (s_enabled) {
            bool ok = internet_ok();
            s_last_ok = ok;

            if (!ok) {
                delay_ms = CHECK_FAIL_MS;
                /* Back off on repeated failures: wrong credentials must not
                 * hammer the portal, campus systems commonly lock the account
                 * after a handful of bad attempts. */
                int shift = s_fail_streak > 5 ? 5 : s_fail_streak;
                int64_t gap = (int64_t)LOGIN_RETRY_MS << shift;
                if (gap > LOGIN_RETRY_MAX_MS) gap = LOGIN_RETRY_MAX_MS;

                int64_t now = esp_timer_get_time() / 1000;
                if (now - s_last_login_ms > gap) {
                    s_last_login_ms = now;
                    ESP_LOGI(TAG, "internet unreachable, trying portal login (attempt %d)",
                             s_fail_streak + 1);
                    if (do_login()) {
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        s_last_ok = internet_ok();
                    } else if (s_fail_streak < 10) {
                        s_fail_streak++;
                        ESP_LOGW(TAG, "login failed %d time(s), next try in %llds",
                                 s_fail_streak, (long long)(gap / 1000));
                    }
                }
            } else {
                int64_t now = esp_timer_get_time() / 1000;
                if (s_userindex[0] && now - s_last_keepalive_ms > (int64_t)s_interval * 1000) {
                    do_keepalive();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/* ------------------------------------------------------------- public --- */

void portal_init(void)
{
    load_config();
    ESP_LOGI(TAG, "init: enabled=%d url=%s user=%s interval=%ds",
             s_enabled, s_url, s_user[0] ? s_user : "(unset)", s_interval);
    xTaskCreate(portal_task, "portal_login", 5120, NULL, 4, NULL);
}

void portal_config_get(char *url, size_t url_len,
                       char *user, size_t user_len,
                       char *pass, size_t pass_len,
                       int *enabled, int *interval)
{
    if (url && url_len)     snprintf(url, url_len, "%s", s_url);
    if (user && user_len)   snprintf(user, user_len, "%s", s_user);
    if (pass && pass_len)   snprintf(pass, pass_len, "%s", s_pass);
    if (enabled)            *enabled = s_enabled ? 1 : 0;
    if (interval)           *interval = s_interval;
}

void portal_config_set(const char *url, const char *user, const char *pass,
                       int enabled, int interval)
{
    if (url)  snprintf(s_url, sizeof(s_url), "%s", url);
    if (user) snprintf(s_user, sizeof(s_user), "%s", user);
    if (pass) snprintf(s_pass, sizeof(s_pass), "%s", pass);
    s_enabled = enabled != 0;
    if (interval >= 60) s_interval = interval;
    save_config();
    /* Re-login on next round with the new credentials. */
    s_userindex[0] = '\0';
    s_last_login_ms = 0;
    snprintf(s_last_msg, sizeof(s_last_msg), "settings saved");
}

void portal_status_text(char *buf, size_t len)
{
    snprintf(buf, len,
             "enabled=%d  online=%d  keepalive=%s  fails=%d  %s",
             s_enabled ? 1 : 0,
             s_last_ok ? 1 : 0,
             s_userindex[0] ? "armed" : "not logged in",
             s_fail_streak,
             s_last_msg);
}
