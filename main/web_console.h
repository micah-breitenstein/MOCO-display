#pragma once

/**
 * web_console.h
 *
 * WiFi AP + HTTP/WebSocket event console for MOCO Jib.
 *
 * SSID:     "MOCO Jib"
 * Password: "openjib"
 * URL:      http://192.168.4.1  (also auto-pops as captive portal on iOS)
 *
 * Call web_console_init() once after nvs_flash_init().
 * Call web_console_log_event() from any task to push a message to
 * all connected browser clients.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise WiFi AP, start HTTP server, and launch the event-dispatch task.
 * Must be called after nvs_flash_init().
 */
void web_console_init(void);

/**
 * Queue an event string for delivery to connected WebSocket clients.
 * Thread-safe. Non-blocking — drops silently if the queue is full.
 * msg is copied; caller does not need to keep it alive.
 */
void web_console_log_event(const char *msg);

/**
 * Register a callback that is invoked (from the httpd task context) when
 * the browser sends a text frame over WebSocket.  Pass NULL to deregister.
 * The callback must be non-blocking.
 */
typedef void (*web_console_cmd_handler_t)(const char *cmd);
void web_console_set_cmd_handler(web_console_cmd_handler_t handler);

#ifdef __cplusplus
}
#endif
