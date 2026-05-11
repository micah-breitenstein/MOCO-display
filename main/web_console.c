#include "web_console.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define AP_SSID        "MOCO"
#define AP_PASS        ""
#define AP_MAX_CONN    2

#define RING_BUF_SIZE  64
#define EVENT_MSG_LEN  160

static const char *TAG = "web_console";

extern const uint8_t landing_html_start[]   asm("_binary_landing_html_start");
extern const uint8_t landing_html_end[]     asm("_binary_landing_html_end");

/* Ring buffer for console events */
typedef struct {
    char     buf[RING_BUF_SIZE][EVENT_MSG_LEN];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t notify;
} ring_buf_t;

static httpd_handle_t    s_server = NULL;
static int               s_ws_fd  = -1;
static ring_buf_t        s_ring   = {0};
static SemaphoreHandle_t s_send_sem = NULL;

static bool ring_buf_is_empty(void) {
    bool empty;
    xSemaphoreTake(s_ring.mutex, portMAX_DELAY);
    empty = (s_ring.count == 0);
    xSemaphoreGive(s_ring.mutex);
    return empty;
}

static bool ring_buf_push(const char *msg) {
    xSemaphoreTake(s_ring.mutex, portMAX_DELAY);
    if (s_ring.count >= RING_BUF_SIZE) {
        /* Buffer full - drop oldest */
        s_ring.tail = (s_ring.tail + 1) % RING_BUF_SIZE;
    } else {
        s_ring.count++;
    }
    snprintf(s_ring.buf[s_ring.head], EVENT_MSG_LEN, "%s", msg);
    s_ring.head = (s_ring.head + 1) % RING_BUF_SIZE;
    xSemaphoreGive(s_ring.mutex);
    xSemaphoreGive(s_ring.notify);  /* Wake up send task */
    return true;
}

static bool ring_buf_pop(char *msg) {
    bool success = false;
    xSemaphoreTake(s_ring.mutex, portMAX_DELAY);
    if (s_ring.count > 0) {
        snprintf(msg, EVENT_MSG_LEN, "%s", s_ring.buf[s_ring.tail]);
        s_ring.tail = (s_ring.tail + 1) % RING_BUF_SIZE;
        s_ring.count--;
        success = true;
    }
    xSemaphoreGive(s_ring.mutex);
    return success;
}

/* Root page handler */
static esp_err_t root_handler(httpd_req_t *req)
{
    const char *data = (const char *)landing_html_start;
    size_t      len  = (size_t)(landing_html_end - landing_html_start);
    
    ESP_LOGI(TAG, "Serving / (%u bytes)", (unsigned)len);
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    return httpd_resp_send(req, data, len);
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* WebSocket async send */
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
        if (s_ws_fd == w->fd) s_ws_fd = -1;
    }
    free(w);
    xSemaphoreGive(s_send_sem);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int new_fd = httpd_req_to_sockfd(req);
        s_ws_fd = new_fd;
        ESP_LOGI(TAG, "WS handshake on fd=%d", new_fd);
        return ESP_OK;
    }
    
    httpd_ws_frame_t f = {0};
    uint8_t buf[64] = {0};
    f.payload = buf;
    esp_err_t err = httpd_ws_recv_frame(req, &f, sizeof(buf) - 1);
    if (err != ESP_OK) {
        return err;
    }
    
    if (f.type == HTTPD_WS_TYPE_CLOSE) {
        int current_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WS close on fd=%d", current_fd);
        if (s_ws_fd == current_fd) {
            s_ws_fd = -1;
        }
    }
    return ESP_OK;
}

/* Status endpoint for "current" indicator */
static esp_err_t status_handler(httpd_req_t *req)
{
    bool is_current = ring_buf_is_empty();
    const char *resp = is_current ? "{\"current\":true}" : "{\"current\":false}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, resp, strlen(resp));
}

/* Event dispatch task */
static void web_event_task(void *arg)
{
    (void)arg;
    char msg[EVENT_MSG_LEN];
    for (;;) {
        /* Wait for notification */
        xSemaphoreTake(s_ring.notify, portMAX_DELAY);
        
        /* Drain all pending events */
        while (ring_buf_pop(msg)) {
            if (!s_server || s_ws_fd < 0) continue;

            ws_work_t *w = malloc(sizeof(ws_work_t));
            if (!w) continue;
            w->hd = s_server;
            w->fd = s_ws_fd;
            snprintf(w->msg, sizeof(w->msg), "%s", msg);

            xSemaphoreTake(s_send_sem, pdMS_TO_TICKS(1000));

            if (httpd_queue_work(s_server, ws_do_send, w) != ESP_OK) {
                free(w);
                s_ws_fd = -1;
                xSemaphoreGive(s_send_sem);
            }
        }
    }
}

/* WiFi event handler */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
            ESP_LOGI(TAG, "Station " MACSTR " joined", MAC2STR(e->mac));
            web_console_log_event("Device connected");
        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
            ESP_LOGI(TAG, "Station " MACSTR " left", MAC2STR(e->mac));
            s_ws_fd = -1;
        }
    }
}

/* HTTP server startup */
static void start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    static const httpd_uri_t ws_uri = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    static const httpd_uri_t favicon_uri = {
        .uri     = "/favicon.ico",
        .method  = HTTP_GET,
        .handler = favicon_handler,
    };
    httpd_register_uri_handler(s_server, &favicon_uri);

    static const httpd_uri_t status_uri = {
        .uri     = "/status",
        .method  = HTTP_GET,
        .handler = status_handler,
    };
    httpd_register_uri_handler(s_server, &status_uri);

    static const httpd_uri_t root_uri = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = root_handler,
    };
    httpd_register_uri_handler(s_server, &root_uri);

    ESP_LOGI(TAG, "HTTP server started");
}

/* Public API */
void web_console_init(void)
{
    /* Initialize ring buffer */
    s_ring.head = 0;
    s_ring.tail = 0;
    s_ring.count = 0;
    s_ring.mutex = xSemaphoreCreateMutex();
    s_ring.notify = xSemaphoreCreateBinary();
    if (!s_ring.mutex || !s_ring.notify) {
        ESP_LOGE(TAG, "Failed to create ring buffer semaphores");
        return;
    }

    s_send_sem = xSemaphoreCreateBinary();
    if (!s_send_sem) {
        ESP_LOGE(TAG, "Failed to create send semaphore");
        return;
    }
    xSemaphoreGive(s_send_sem);

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

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = 0,
            .channel        = 1,
            .password       = AP_PASS,
            .max_connection = AP_MAX_CONN,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started — SSID: \"%s\" (open) IP: 192.168.4.1", AP_SSID);

    start_http_server();
    
    xTaskCreatePinnedToCore(web_event_task, "web_evt", 4096, NULL, 2, NULL, 1);
    ESP_LOGI(TAG, "Console event task started");
}

void web_console_log_event(const char *msg)
{
    if (!s_ring.mutex || !msg) return;
    ring_buf_push(msg);
}

void web_console_set_cmd_handler(web_console_cmd_handler_t handler)
{
    (void)handler;
}
