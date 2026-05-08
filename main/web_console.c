#include "web_console.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* ------------------------------------------------------------------ */
#define AP_SSID        "MOCO Jib"
#define AP_PASS        "openjib1"
#define AP_MAX_CONN    4

#define EVENT_QUEUE_DEPTH  64
#define EVENT_MSG_LEN      160

static const char *TAG = "web_console";

/* Embedded landing page (added to CMakeLists EMBED_FILES) */
extern const uint8_t landing_html_start[] asm("_binary_landing_html_start");
extern const uint8_t landing_html_end[]   asm("_binary_landing_html_end");

/* ------------------------------------------------------------------ */
static httpd_handle_t  s_server   = NULL;
static int             s_ws_fd   = -1;   /* fd of the active WebSocket client */
static QueueHandle_t   s_eq      = NULL;
static SemaphoreHandle_t s_send_sem = NULL; /* serialises one in-flight WS frame */
static web_console_cmd_handler_t s_cmd_handler = NULL;

/* ------------------------------------------------------------------ */
/*  Async WebSocket send (runs inside the httpd task via queue_work)   */
/* ------------------------------------------------------------------ */
typedef struct {
    httpd_handle_t hd;
    int            fd;
    char           msg[EVENT_MSG_LEN];
} ws_work_t;

static void ws_do_send(void *arg)
{
    ws_work_t *w = (ws_work_t *)arg;
    httpd_ws_frame_t f = {
        .final      = true,
        .fragmented = false,
        .type       = HTTPD_WS_TYPE_TEXT,
        .payload    = (uint8_t *)w->msg,
        .len        = strlen(w->msg),
    };
    if (httpd_ws_send_frame_async(w->hd, w->fd, &f) != ESP_OK) {
        s_ws_fd = -1;   /* mark disconnected */
    }
    free(w);
    xSemaphoreGive(s_send_sem);   /* allow next send */
}

/* ------------------------------------------------------------------ */
/*  Captive-portal DNS responder — answers every query with 192.168.4.1 */
/* ------------------------------------------------------------------ */
static void dns_task(void *arg)
{
    (void)arg;
    static const uint8_t AP_IP[4] = {192, 168, 4, 1};
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }

    struct sockaddr_in saddr = {
        .sin_family = AF_INET,
        .sin_port   = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        close(sock); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "DNS responder started (port 53)");

    uint8_t buf[256];
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);

    for (;;) {
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&caddr, &clen);
        if (n < 12) continue;

        uint8_t resp[512];
        int qlen = n;
        memcpy(resp, buf, qlen);
        resp[2] = 0x81; resp[3] = 0x80;
        resp[6] = 0x00; resp[7] = 0x01;
        resp[8] = 0x00; resp[9] = 0x00;
        resp[10]= 0x00; resp[11]= 0x00;
        int p = qlen;
        if (p + 16 > (int)sizeof(resp)) { continue; }
        resp[p++] = 0xC0; resp[p++] = 0x0C;
        resp[p++] = 0x00; resp[p++] = 0x01;
        resp[p++] = 0x00; resp[p++] = 0x01;
        resp[p++] = 0x00; resp[p++] = 0x00;
        resp[p++] = 0x00; resp[p++] = 0x3C;
        resp[p++] = 0x00; resp[p++] = 0x04;
        resp[p++] = AP_IP[0]; resp[p++] = AP_IP[1];
        resp[p++] = AP_IP[2]; resp[p++] = AP_IP[3];
        sendto(sock, resp, p, 0, (struct sockaddr *)&caddr, clen);
    }
}

/* ------------------------------------------------------------------ */
/*  HTTP handlers                                                       */
/* ------------------------------------------------------------------ */

/* Catch-all: serves the landing page for every GET URI, which also
 * satisfies iOS captive-portal detection so Safari opens automatically. */
static esp_err_t root_handler(httpd_req_t *req)
{
    const char *data = (const char *)landing_html_start;
    size_t      len  = (size_t)(landing_html_end - landing_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    /* Send in 1 KB chunks to avoid socket-buffer overflow on large pages */
    const size_t CHUNK = 1024;
    size_t sent = 0;
    while (sent < len) {
        size_t n = (len - sent) < CHUNK ? (len - sent) : CHUNK;
        esp_err_t ret = httpd_resp_send_chunk(req, data + sent, (ssize_t)n);
        if (ret != ESP_OK) return ret;
        sent += n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* WebSocket upgrade + frame handler */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Handshake — store fd so we can push to this client */
        s_ws_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WS client connected, fd=%d", s_ws_fd);
        return ESP_OK;
    }
    /* Drain incoming frames (we only care about outbound events) */
    httpd_ws_frame_t f = {0};
    uint8_t buf[64] = {0};
    f.payload = buf;
    esp_err_t err = httpd_ws_recv_frame(req, &f, sizeof(buf) - 1);
    if (err != ESP_OK) {
        return err;
    }
    if (f.type == HTTPD_WS_TYPE_TEXT && f.len > 0 && s_cmd_handler) {
        buf[f.len < sizeof(buf) ? f.len : sizeof(buf) - 1] = '\0';
        s_cmd_handler((const char *)buf);
    }
    if (f.type == HTTPD_WS_TYPE_CLOSE) {
        s_ws_fd = -1;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Event dispatch task — drains queue → WebSocket frames              */
/* ------------------------------------------------------------------ */
static void web_event_task(void *arg)
{
    (void)arg;
    char msg[EVENT_MSG_LEN];
    for (;;) {
        if (xQueueReceive(s_eq, msg, portMAX_DELAY) != pdTRUE) continue;
        if (!s_server || s_ws_fd < 0) continue;

        ws_work_t *w = malloc(sizeof(ws_work_t));
        if (!w) continue;
        w->hd = s_server;
        w->fd = s_ws_fd;
        snprintf(w->msg, sizeof(w->msg), "%s", msg);

        /* Wait for the previous frame to finish sending (max 1 s) */
        xSemaphoreTake(s_send_sem, pdMS_TO_TICKS(1000));

        if (httpd_queue_work(s_server, ws_do_send, w) != ESP_OK) {
            free(w);
            s_ws_fd = -1;
            xSemaphoreGive(s_send_sem);  /* unblock for next attempt */
        }
    }
}

/* ------------------------------------------------------------------ */
/*  WiFi event handler                                                  */
/* ------------------------------------------------------------------ */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
            ESP_LOGI(TAG, "Station " MACSTR " joined", MAC2STR(e->mac));
            web_console_log_event("Device connected to MOCO Jib WiFi");
        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
            ESP_LOGI(TAG, "Station " MACSTR " left", MAC2STR(e->mac));
            s_ws_fd = -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  HTTP server startup                                                 */
/* ------------------------------------------------------------------ */
static void start_http_server(void)
{
    httpd_config_t cfg       = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable     = true;
    cfg.max_open_sockets     = 10;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    /* WebSocket handler must be registered BEFORE the catch-all */
    static const httpd_uri_t ws_uri = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    /* Root page */
    static const httpd_uri_t root_uri = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = root_handler,
    };
    httpd_register_uri_handler(s_server, &root_uri);

    ESP_LOGI(TAG, "HTTP server started on port 80");
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */
void web_console_init(void)
{
    s_eq = xQueueCreate(EVENT_QUEUE_DEPTH, EVENT_MSG_LEN);
    if (!s_eq) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return;
    }

    s_send_sem = xSemaphoreCreateBinary();
    if (!s_send_sem) {
        ESP_LOGE(TAG, "Failed to create send semaphore");
        return;
    }
    xSemaphoreGive(s_send_sem);  /* start in available state */

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t el = esp_event_loop_create_default();
    if (el != ESP_OK && el != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(el);
    }

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));

    wifi_config_t ap_cfg;
    memset(&ap_cfg, 0, sizeof(ap_cfg));
    memcpy(ap_cfg.ap.ssid,     AP_SSID, sizeof(AP_SSID) - 1);
    memcpy(ap_cfg.ap.password, AP_PASS, sizeof(AP_PASS) - 1);
    ap_cfg.ap.ssid_len       = (uint8_t)(sizeof(AP_SSID) - 1);
    ap_cfg.ap.max_connection = AP_MAX_CONN;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.capable  = false;
    ap_cfg.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started — SSID: \"%s\"  IP: 192.168.4.1", AP_SSID);

    start_http_server();

    xTaskCreatePinnedToCore(web_event_task, "web_evt", 4096, NULL, 2, NULL, 1);
}

void web_console_log_event(const char *msg)
{
    if (!s_eq || !msg) return;
    char buf[EVENT_MSG_LEN];
    snprintf(buf, sizeof(buf), "%s", msg);
    xQueueSendToBack(s_eq, buf, 0);   /* non-blocking; drops if full */
}

void web_console_set_cmd_handler(web_console_cmd_handler_t handler)
{
    s_cmd_handler = handler;
}
