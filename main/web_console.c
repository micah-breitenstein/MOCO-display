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
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define AP_SSID        "MOCO"
#define AP_PASS        ""
#define AP_MAX_CONN    2

#define RING_BUF_SIZE  64
#define EVENT_MSG_LEN  160

#define DNS_PORT       53
#define DNS_MAX_LEN    512

static const char *TAG = "web_console";

extern const uint8_t loading_html_start[]   asm("_binary_loading_html_start");
extern const uint8_t loading_html_end[]     asm("_binary_loading_html_end");
extern const uint8_t landing_html_start[]   asm("_binary_landing_html_start");
extern const uint8_t landing_html_end[]     asm("_binary_landing_html_end");
extern const uint8_t timelapse_html_start[] asm("_binary_timelapse_html_start");
extern const uint8_t timelapse_html_end[]   asm("_binary_timelapse_html_end");
extern const uint8_t bounce_html_start[]    asm("_binary_bounce_html_start");
extern const uint8_t bounce_html_end[]      asm("_binary_bounce_html_end");
extern const uint8_t dronemode_html_start[] asm("_binary_dronemode_html_start");
extern const uint8_t dronemode_html_end[]   asm("_binary_dronemode_html_end");
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

typedef enum {
    PAGE_TYPE_UNKNOWN = 0,
    PAGE_TYPE_CONSOLE = 1,
    PAGE_TYPE_SETTINGS = 2,
    PAGE_TYPE_TIMELAPSE = 3,
    PAGE_TYPE_BOUNCE = 4,
    PAGE_TYPE_DRONEMODE = 5,
} page_type_t;

static httpd_handle_t    s_server = NULL;
static int               s_ws_fd  = -1;
static page_type_t       s_page_type = PAGE_TYPE_UNKNOWN;
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

static bool ring_buf_peek(char *msg) {
    bool success = false;
    xSemaphoreTake(s_ring.mutex, portMAX_DELAY);
    if (s_ring.count > 0) {
        snprintf(msg, EVENT_MSG_LEN, "%s", s_ring.buf[s_ring.tail]);
        success = true;
    }
    xSemaphoreGive(s_ring.mutex);
    return success;
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

/* Root page handler - minimal loading page */
static esp_err_t root_handler(httpd_req_t *req)
{
    const char *data = (const char *)loading_html_start;
    size_t      len  = (size_t)(loading_html_end - loading_html_start);
    
    ESP_LOGI(TAG, "Serving / (loading page, %u bytes)", (unsigned)len);
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    
    esp_err_t ret = httpd_resp_send(req, data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send loading page: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* Console page handler - full landing page */
static esp_err_t console_handler(httpd_req_t *req)
{
    const char *data = (const char *)landing_html_start;
    size_t      len  = (size_t)(landing_html_end - landing_html_start);
    
    ESP_LOGI(TAG, "Serving /console (%u bytes)", (unsigned)len);
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    
    esp_err_t ret = httpd_resp_send(req, data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send console page: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* Timelapse page handler */
static esp_err_t timelapse_handler(httpd_req_t *req)
{
    const char *data = (const char *)timelapse_html_start;
    size_t      len  = (size_t)(timelapse_html_end - timelapse_html_start);
    
    ESP_LOGI(TAG, "Serving /timelapse (%u bytes)", (unsigned)len);
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    
    esp_err_t ret = httpd_resp_send(req, data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send timelapse page: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* Bounce page handler */
static esp_err_t bounce_handler(httpd_req_t *req)
{
    const char *data = (const char *)bounce_html_start;
    size_t      len  = (size_t)(bounce_html_end - bounce_html_start);
    
    ESP_LOGI(TAG, "Serving /bounce (%u bytes)", (unsigned)len);
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    
    esp_err_t ret = httpd_resp_send(req, data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send bounce page: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* Drone mode page handler */
static esp_err_t dronemode_handler(httpd_req_t *req)
{
    const char *data = (const char *)dronemode_html_start;
    size_t      len  = (size_t)(dronemode_html_end - dronemode_html_start);
    
    ESP_LOGI(TAG, "Serving /dronemode (%u bytes)", (unsigned)len);
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    
    esp_err_t ret = httpd_resp_send(req, data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send dronemode page: %s", esp_err_to_name(ret));
    }
    return ret;
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
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    httpd_resp_set_hdr(req, "Connection", "close");
    
    esp_err_t ret = httpd_resp_send(req, data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send settings page: %s", esp_err_to_name(ret));
    }
    return ret;
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
    esp_err_t err = httpd_ws_send_frame_async(w->hd, w->fd, &f);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send WebSocket frame to fd=%d: %s (msg: %.50s)", 
                 w->fd, esp_err_to_name(err), w->msg);
        if (s_ws_fd == w->fd) {
            s_ws_fd = -1;
        }
    }
    free(w);
    xSemaphoreGive(s_send_sem);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int new_fd = httpd_req_to_sockfd(req);
        s_ws_fd = new_fd;
        s_page_type = PAGE_TYPE_UNKNOWN;
        ESP_LOGI(TAG, "WS handshake on fd=%d, ring buffer has %u events, waiting for page identification", new_fd, s_ring.count);
        
        /* Send welcome but wait for page to identify itself before flushing events */
        vTaskDelay(pdMS_TO_TICKS(500));
        
        if (s_server && s_ws_fd == new_fd) {
            char welcome[64];
            snprintf(welcome, sizeof(welcome), "WebSocket ready (%lu buffered events)", (unsigned long)s_ring.count);
            httpd_ws_frame_t frame = {
                .final = true,
                .fragmented = false,
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)welcome,
                .len = strlen(welcome),
            };
            esp_err_t err = httpd_ws_send_frame_async(s_server, new_fd, &frame);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send welcome message: %s", esp_err_to_name(err));
            }
        }
        
        /* Don't flush buffered events yet - wait for page to identify itself */
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
        
        /* Check for page type identification */
        if (strncmp((char *)buf, "PAGE:CONSOLE", 12) == 0) {
            s_page_type = PAGE_TYPE_CONSOLE;
            ESP_LOGI(TAG, "Page identified as CONSOLE, flushing %u buffered events", s_ring.count);
            if (s_ring.count > 0) {
                xSemaphoreGive(s_ring.notify);
            }
            return ESP_OK;
        } else if (strncmp((char *)buf, "PAGE:SETTINGS", 13) == 0) {
            s_page_type = PAGE_TYPE_SETTINGS;
            ESP_LOGI(TAG, "Page identified as SETTINGS, not flushing buffered events (they're preserved for console)");
            return ESP_OK;
        } else if (strncmp((char *)buf, "PAGE:TIMELAPSE", 14) == 0) {
            s_page_type = PAGE_TYPE_TIMELAPSE;
            ESP_LOGI(TAG, "Page identified as TIMELAPSE, not flushing buffered events (they're preserved for console)");
            return ESP_OK;
        } else if (strncmp((char *)buf, "PAGE:BOUNCE", 11) == 0) {
            s_page_type = PAGE_TYPE_BOUNCE;
            ESP_LOGI(TAG, "Page identified as BOUNCE, not flushing buffered events (they're preserved for console)");
            return ESP_OK;
        } else if (strncmp((char *)buf, "PAGE:DRONEMODE", 14) == 0) {
            s_page_type = PAGE_TYPE_DRONEMODE;
            ESP_LOGI(TAG, "Page identified as DRONEMODE, not flushing buffered events (they're preserved for console)");
            return ESP_OK;
        }
        
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
            s_page_type = PAGE_TYPE_UNKNOWN;
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
        /* Wait for notification that events are available */
        xSemaphoreTake(s_ring.notify, portMAX_DELAY);
        
        /* Drain all events to active WebSocket */
        if (s_server && s_ws_fd >= 0) {
            int count = 0;
            int saved_fd = s_ws_fd;
            page_type_t saved_page_type = s_page_type;
            
            while (ring_buf_peek(msg)) {
                /* Filter CONSOLE: messages - only send to console page */
                if (strncmp(msg, "CONSOLE:", 8) == 0 && saved_page_type != PAGE_TYPE_CONSOLE) {
                    /* Console events blocked by non-console page - pop and skip */
                    ring_buf_pop(msg);
                    continue;
                }
                /* Filter DRONE_STICK:, DRONE_MODIFIER:, and TRIGGER:MANUAL_DRONE messages - only send to dronemode page */
                if ((strncmp(msg, "DRONE_STICK:", 12) == 0 || strncmp(msg, "DRONE_MODIFIER:", 15) == 0 || strncmp(msg, "TRIGGER:MANUAL_DRONE", 20) == 0) 
                    && saved_page_type != PAGE_TYPE_DRONEMODE) {
                    /* Dronemode events blocked by non-dronemode page - pop and skip */
                    ring_buf_pop(msg);
                    continue;
                }
                
                /* Message is appropriate for this page - pop and send it */
                ring_buf_pop(msg);
                
                ws_work_t *w = malloc(sizeof(ws_work_t));
                if (!w) {
                    ESP_LOGW(TAG, "Failed to allocate ws_work_t for event: %s", msg);
                    break;
                }
                w->hd = s_server;
                w->fd = saved_fd;
                snprintf(w->msg, sizeof(w->msg), "%s", msg);

                xSemaphoreTake(s_send_sem, pdMS_TO_TICKS(1000));

                if (httpd_queue_work(s_server, ws_do_send, w) != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to queue event for fd=%d: %s", saved_fd, msg);
                    free(w);
                    if (s_ws_fd == saved_fd) {
                        s_ws_fd = -1;
                    }
                    xSemaphoreGive(s_send_sem);
                    break;
                }
                
                count++;
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            
            if (count > 0) {
                ESP_LOGI(TAG, "Queued %d events to WebSocket fd=%d", count, saved_fd);
            }
        } else {
            ESP_LOGI(TAG, "No active WebSocket (fd=%d), events remain buffered (%u)", s_ws_fd, s_ring.count);
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
    cfg.max_open_sockets = 5;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 30;
    cfg.keep_alive_interval = 10;
    cfg.keep_alive_count = 3;
    cfg.backlog_conn = 5;
    cfg.stack_size = 8192;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }
    ESP_LOGI(TAG, "HTTP server started with recv/send timeout=20s");

    /* Register root route first */
    static const httpd_uri_t root_uri = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = root_handler,
    };
    httpd_register_uri_handler(s_server, &root_uri);

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

    static const httpd_uri_t timelapse_uri = {
        .uri     = "/timelapse",
        .method  = HTTP_GET,
        .handler = timelapse_handler,
    };
    httpd_register_uri_handler(s_server, &timelapse_uri);

    static const httpd_uri_t bounce_uri = {
        .uri     = "/bounce",
        .method  = HTTP_GET,
        .handler = bounce_handler,
    };
    httpd_register_uri_handler(s_server, &bounce_uri);

    static const httpd_uri_t dronemode_uri = {
        .uri     = "/dronemode",
        .method  = HTTP_GET,
        .handler = dronemode_handler,
    };
    httpd_register_uri_handler(s_server, &dronemode_uri);

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

    static const httpd_uri_t console_uri = {
        .uri     = "/console",
        .method  = HTTP_GET,
        .handler = console_handler,
    };
    httpd_register_uri_handler(s_server, &console_uri);

    ESP_LOGI(TAG, "HTTP server started");
}

/* DNS server for captive portal detection */
static void dns_server_task(void *arg)
{
    uint8_t rx_buf[DNS_MAX_LEN];
    uint8_t tx_buf[DNS_MAX_LEN];
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(NULL);
        return;
    }
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(DNS_PORT);
    
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "DNS server listening on port %d", DNS_PORT);
    
    for (;;) {
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0,
                          (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (len < 12) continue;  // Too small to be valid DNS query
        
        // Copy query to response buffer
        memcpy(tx_buf, rx_buf, len);
        
        // Set response flags: QR=1 (response), AA=1 (authoritative)
        tx_buf[2] = 0x81;  // QR=1, Opcode=0, AA=1
        tx_buf[3] = 0x80;  // RA=1, RCODE=0 (no error)
        
        // Get query type at offset 12 + domain name length
        int offset = 12;
        while (offset < len && rx_buf[offset] != 0) {
            offset += rx_buf[offset] + 1;
        }
        offset++;  // Skip null terminator
        
        if (offset + 4 > len) continue;  // Invalid query
        
        uint16_t qtype = (rx_buf[offset] << 8) | rx_buf[offset + 1];
        
        // Only respond to A record queries (type 1)
        if (qtype != 1) continue;
        
        // Set answer count to 1
        tx_buf[6] = 0x00;
        tx_buf[7] = 0x01;
        
        // Build answer: pointer to question name (0xC00C), type A, class IN, TTL, length 4, IP
        int pos = len;
        tx_buf[pos++] = 0xC0;  // Pointer to offset 12
        tx_buf[pos++] = 0x0C;
        tx_buf[pos++] = 0x00;  // Type A
        tx_buf[pos++] = 0x01;
        tx_buf[pos++] = 0x00;  // Class IN
        tx_buf[pos++] = 0x01;
        tx_buf[pos++] = 0x00;  // TTL (60 seconds)
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x3C;
        tx_buf[pos++] = 0x00;  // Data length (4 bytes)
        tx_buf[pos++] = 0x04;
        tx_buf[pos++] = 192;   // 192.168.4.1
        tx_buf[pos++] = 168;
        tx_buf[pos++] = 4;
        tx_buf[pos++] = 1;
        
        sendto(sock, tx_buf, pos, 0, (struct sockaddr *)&client_addr, client_addr_len);
    }
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
    
    xTaskCreatePinnedToCore(dns_server_task, "dns_srv", 4096, NULL, 2, NULL, 1);
    ESP_LOGI(TAG, "DNS server started on port 53");
}

void web_console_log_event(const char *msg)
{
    if (!s_ring.mutex || !msg) return;
    
    /* Deduplicate repeated messages - thread-safe version
     * Only log if different from last message or if >1 second has passed */
    static char last_msg[EVENT_MSG_LEN] = {0};
    static int64_t last_time_us = 0;
    
    /* Protect deduplication check with mutex to make it thread-safe */
    xSemaphoreTake(s_ring.mutex, portMAX_DELAY);
    
    int64_t now_us = esp_timer_get_time();
    bool should_log = false;
    
    if (strcmp(msg, last_msg) != 0) {
        /* Different message - log it */
        should_log = true;
        snprintf(last_msg, sizeof(last_msg), "%s", msg);
        last_time_us = now_us;
    } else if ((now_us - last_time_us) > 1000000) {
        /* Same message but more than 1 second - log it */
        should_log = true;
        last_time_us = now_us;
    }
    
    if (should_log) {
        /* Push to ring buffer (mutex already held) */
        if (s_ring.count >= RING_BUF_SIZE) {
            /* Buffer full - drop oldest */
            s_ring.tail = (s_ring.tail + 1) % RING_BUF_SIZE;
        } else {
            s_ring.count++;
        }
        snprintf(s_ring.buf[s_ring.head], EVENT_MSG_LEN, "%s", msg);
        s_ring.head = (s_ring.head + 1) % RING_BUF_SIZE;
        
        /* Only notify if this event can be sent to the current page */
        bool is_console_msg = (strncmp(msg, "CONSOLE:", 8) == 0);
        bool is_dronemode_msg = (strncmp(msg, "DRONE_STICK:", 12) == 0 || strncmp(msg, "DRONE_MODIFIER:", 15) == 0 || strncmp(msg, "TRIGGER:MANUAL_DRONE", 20) == 0);
        bool can_send = (!is_console_msg || (s_page_type == PAGE_TYPE_CONSOLE))
                     && (!is_dronemode_msg || (s_page_type == PAGE_TYPE_DRONEMODE));
        
        if (can_send && s_ws_fd >= 0) {
            xSemaphoreGive(s_ring.notify);
        }
    }
    
    xSemaphoreGive(s_ring.mutex);
}

void web_console_broadcast_setting(const char *msg)
{
    /* Bypass ring buffer - send SETTINGS: messages directly to WebSocket client */
    if (!msg || s_ws_fd < 0 || !s_server) {
        return;
    }
    
    ws_work_t *w = malloc(sizeof(ws_work_t));
    if (!w) {
        ESP_LOGW(TAG, "Failed to allocate for setting broadcast: %s", msg);
        return;
    }
    
    w->hd = s_server;
    w->fd = s_ws_fd;
    snprintf(w->msg, sizeof(w->msg), "%s", msg);
    
    xSemaphoreTake(s_send_sem, pdMS_TO_TICKS(1000));
    
    if (httpd_queue_work(s_server, ws_do_send, w) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue setting broadcast: %s", msg);
        free(w);
        xSemaphoreGive(s_send_sem);
    }
}

bool web_console_has_client(void)
{
    return (s_ws_fd >= 0);
}
void web_console_set_cmd_handler(web_console_cmd_handler_t handler)
{
    ESP_LOGI(TAG, "web_console_set_cmd_handler: handler=%p", handler);
    s_cmd_handler = handler;
    ESP_LOGI(TAG, "s_cmd_handler now set to: %p", s_cmd_handler);
}
