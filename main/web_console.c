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
extern const uint8_t settings_html_start[]  asm("_binary_settings_html_start");
extern const uint8_t settings_html_end[]    asm("_binary_settings_html_end");

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
static web_console_cmd_handler_t s_cmd_handler = NULL;

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

/* Settings page handler */
static esp_err_t settings_handler(httpd_req_t *req)
{
    const char *data = (const char *)settings_html_start;
    size_t      len  = (size_t)(settings_html_end - settings_html_start);
    
    ESP_LOGI(TAG, "Serving /settings (%u bytes)", (unsigned)len);
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    return httpd_resp_send(req, data, len);
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
        /* Trigger event task to flush any accumulated events from ring buffer */
        xSemaphoreGive(s_ring.notify);
        return ESP_OK;
    }
    
    httpd_ws_frame_t f = {0};
    uint8_t buf[128] = {0};
    f.payload = buf;
    
    ESP_LOGI(TAG, "WS recv attempt, fd=%d", httpd_req_to_sockfd(req));
    
    esp_err_t err = httpd_ws_recv_frame(req, &f, sizeof(buf) - 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS recv failed: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "WS frame type=%d len=%d", f.type, (int)f.len);
    
    if (f.type == HTTPD_WS_TYPE_TEXT && f.len > 0 && f.len < sizeof(buf)) {
        /* Null-terminate the received message */
        buf[f.len] = '\0';
        
        ESP_LOGI(TAG, "WS recv: '%s' (len=%d)", (char *)buf, (int)f.len);
        
        /* Call the registered command handler if available */
        if (s_cmd_handler) {
            ESP_LOGI(TAG, "Calling cmd_handler at %p with '%s'", s_cmd_handler, (char *)buf);
            s_cmd_handler((const char *)buf);
            ESP_LOGI(TAG, "cmd_handler returned successfully");
        } else {
            ESP_LOGW(TAG, "No cmd_handler registered!");
        }
    } else if (f.type == HTTPD_WS_TYPE_CLOSE) {
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

/* API endpoint to get current settings */
static esp_err_t api_settings_get_handler(httpd_req_t *req)
{
    /* Return current default values - actual values will sync via web_cmd_handler */
    const char *resp = "{\"mtx_brt\":5,\"bright\":100,\"r_mute\":0,\"theme\":0,\"tl_int\":15,\"tl_step\":100}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, resp, strlen(resp));
}

/* API endpoint to update settings */
static esp_err_t api_settings_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    /* Parse JSON and extract key/value - simple parse for {"key":"...","value":...} */
    char *key_start = strstr(buf, "\"key\":\"");
    char *val_start = strstr(buf, "\"value\":");
    if (!key_start || !val_start) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    key_start += 7; /* skip "key":" */
    char *key_end = strchr(key_start, '"');
    if (!key_end) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid key");
        return ESP_FAIL;
    }
    
    size_t key_len = key_end - key_start;
    char key[32];
    if (key_len >= sizeof(key)) key_len = sizeof(key) - 1;
    memcpy(key, key_start, key_len);
    key[key_len] = '\0';
    
    val_start += 8; /* skip "value": */
    int value = atoi(val_start);
    
    /* Send command to main */
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "SET:%s:%d", key, value);
    if (s_cmd_handler) {
        s_cmd_handler(cmd);
    }
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* Event dispatch task */
static void web_event_task(void *arg)
{
    (void)arg;
    char msg[EVENT_MSG_LEN];
    for (;;) {
        /* Wait for notification */
        xSemaphoreTake(s_ring.notify, portMAX_DELAY);
        
        /* Only drain events if we have an active WebSocket connection */
        if (s_server && s_ws_fd >= 0) {
            /* Drain all pending events */
            while (ring_buf_pop(msg)) {
                ws_work_t *w = malloc(sizeof(ws_work_t));
                if (!w) {
                    /* Out of memory - put message back by not popping it next time */
                    break;
                }
                w->hd = s_server;
                w->fd = s_ws_fd;
                snprintf(w->msg, sizeof(w->msg), "%s", msg);

                xSemaphoreTake(s_send_sem, pdMS_TO_TICKS(1000));

                if (httpd_queue_work(s_server, ws_do_send, w) != ESP_OK) {
                    free(w);
                    s_ws_fd = -1;
                    xSemaphoreGive(s_send_sem);
                    break;  /* Stop sending on error */
                }
            }
        }
        /* If no WebSocket connection, events stay in ring buffer */
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
    cfg.max_open_sockets = 5;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 15;
    cfg.keep_alive_interval = 5;
    cfg.keep_alive_count = 3;

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

    static const httpd_uri_t settings_uri = {
        .uri     = "/settings",
        .method  = HTTP_GET,
        .handler = settings_handler,
    };
    httpd_register_uri_handler(s_server, &settings_uri);

    static const httpd_uri_t api_settings_get_uri = {
        .uri     = "/api/settings",
        .method  = HTTP_GET,
        .handler = api_settings_get_handler,
    };
    httpd_register_uri_handler(s_server, &api_settings_get_uri);

    static const httpd_uri_t api_settings_post_uri = {
        .uri     = "/api/settings",
        .method  = HTTP_POST,
        .handler = api_settings_post_handler,
    };
    httpd_register_uri_handler(s_server, &api_settings_post_uri);

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
    ESP_LOGI(TAG, "web_console_set_cmd_handler: handler=%p", handler);
    s_cmd_handler = handler;
    ESP_LOGI(TAG, "s_cmd_handler now set to: %p", s_cmd_handler);
}
