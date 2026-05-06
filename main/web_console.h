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

#ifdef __cplusplus
}
#endif
