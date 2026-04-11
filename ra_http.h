#ifndef RA_HTTP_H
#define RA_HTTP_H

#include <stdint.h>

// Asynchronous HTTP module for RetroAchievements on MiSTer.
//
// Uses a background worker thread that executes requests via the system
// `curl` command (available on MiSTer Linux). Responses are queued back
// to the main thread and dispatched by ra_http_poll().
//
// Thread model:
//   Main thread: ra_http_request() enqueues → ra_http_poll() dispatches callbacks
//   Worker thread: dequeues requests → executes curl → enqueues responses
//
// This keeps all rc_client callbacks on the main thread (required by rcheevos).

// Callback type matching rc_client's server callback signature.
// (defined here so ra_http can be used without rcheevos headers)
typedef void (*ra_http_callback_t)(const void *server_response, void *callback_data);

// Initialize the HTTP worker thread. Call once at startup.
void ra_http_init(void);

// Shutdown the HTTP worker thread. Blocks until pending requests complete.
void ra_http_deinit(void);

// Enqueue an HTTP request to be executed asynchronously.
// url:           Full URL (e.g., "https://retroachievements.org/dorequest.php")
// post_data:     POST body (NULL for GET)
// content_type:  Content-Type header (NULL for default application/x-www-form-urlencoded)
// callback:      Called on the main thread (from ra_http_poll) when the response arrives
// callback_data: Opaque userdata passed through to callback
void ra_http_request(const char *url, const char *post_data,
	const char *content_type, ra_http_callback_t callback, void *callback_data);

// Poll for completed HTTP responses and dispatch their callbacks.
// Must be called periodically from the main thread (e.g., from achievements_poll).
// Returns number of callbacks dispatched.
int ra_http_poll(void);

// Returns number of requests currently in-flight (queued + executing).
int ra_http_pending(void);

// Set the User-Agent header sent with every request.
// Must be called before ra_http_request(). Not thread-safe — call from main thread.
void ra_http_set_user_agent(const char *user_agent);

#endif // RA_HTTP_H
