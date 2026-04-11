// ra_http.cpp — Asynchronous HTTP for RetroAchievements on MiSTer
//
// Worker thread executes curl via popen(), responses are queued back to the
// main thread for safe rc_client callback dispatch.

#include "ra_http.h"
#include "achievements.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Debug (uses same style as achievements.cpp)
// ---------------------------------------------------------------------------

#define HTTP_LOG(fmt, ...) ra_log_write("RA: HTTP_WORKER: " fmt "\n", ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Request / Response structures
// ---------------------------------------------------------------------------

struct ra_http_req {
	char *url;
	char *post_data;
	char *content_type;
	ra_http_callback_t callback;
	void *callback_data;
};

struct ra_http_resp {
	int http_status;
	char *body;
	size_t body_len;
	ra_http_callback_t callback;
	void *callback_data;
};

// ---------------------------------------------------------------------------
// Thread-safe queues (fixed-size ring buffers)
// ---------------------------------------------------------------------------

static constexpr int QUEUE_CAP = 16;

// Request queue: main→worker
static ra_http_req  s_req_queue[QUEUE_CAP];
static int s_req_head = 0, s_req_tail = 0;
static pthread_mutex_t s_req_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_req_cond  = PTHREAD_COND_INITIALIZER;

// Response queue: worker→main
static ra_http_resp s_resp_queue[QUEUE_CAP];
static int s_resp_head = 0, s_resp_tail = 0;
static pthread_mutex_t s_resp_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_t s_worker;
static bool s_quit = false;
static bool s_inited = false;
static char s_user_agent[256] = "MiSTer";

static int queue_count(int head, int tail)
{
	return head - tail;
}

// ---------------------------------------------------------------------------
// curl execution via popen()
// ---------------------------------------------------------------------------

// Grow-able buffer for reading curl output
struct buf {
	char *data;
	size_t len;
	size_t cap;
};

static void buf_init(struct buf *b)
{
	b->cap = 4096;
	b->data = (char *)malloc(b->cap);
	b->len = 0;
	if (b->data) b->data[0] = '\0';
}

static void buf_append(struct buf *b, const char *chunk, size_t n)
{
	if (!b->data) return;
	while (b->len + n + 1 > b->cap) {
		b->cap *= 2;
		char *tmp = (char *)realloc(b->data, b->cap);
		if (!tmp) { free(b->data); b->data = NULL; return; }
		b->data = tmp;
	}
	memcpy(b->data + b->len, chunk, n);
	b->len += n;
	b->data[b->len] = '\0';
}

// Build a shell command string for curl.
// We write status code on a separate last line using -w '\n%{http_code}'.
static char *build_curl_cmd(const char *url, const char *post_data,
	const char *content_type)
{
	// Estimate command length
	size_t len = 256 + strlen(url);
	if (post_data) len += strlen(post_data) + 32;
	if (content_type) len += strlen(content_type) + 32;

	char *cmd = (char *)malloc(len);
	if (!cmd) return NULL;

	// Base: silent, follow redirects, 30s timeout, output body then HTTP code
	int off = snprintf(cmd, len,
		"curl -s -L --max-time 30 -w '\\n%%{http_code}' -A '%s'", s_user_agent);

	if (post_data && post_data[0]) {
		// Use --data-binary with stdin to avoid shell injection from post_data
		off += snprintf(cmd + off, len - off, " -X POST");
		if (content_type && content_type[0]) {
			off += snprintf(cmd + off, len - off, " -H 'Content-Type: %s'", content_type);
		} else {
			off += snprintf(cmd + off, len - off,
				" -H 'Content-Type: application/x-www-form-urlencoded'");
		}
		off += snprintf(cmd + off, len - off, " --data-binary @-");
	}

	// URL: use single quotes to prevent shell interpretation, escaping any
	// embedded single quotes.
	off += snprintf(cmd + off, len - off, " '");
	for (const char *p = url; *p; p++) {
		if (*p == '\'') {
			off += snprintf(cmd + off, len - off, "'\\''");
		} else {
			cmd[off++] = *p;
		}
	}
	off += snprintf(cmd + off, len - off, "'");
	cmd[off] = '\0';

	return cmd;
}

static void execute_request(ra_http_req *req, ra_http_resp *resp)
{
	resp->callback = req->callback;
	resp->callback_data = req->callback_data;
	resp->http_status = 0;
	resp->body = NULL;
	resp->body_len = 0;

	// Build final shell command.
	// For POST: printf '%s' '<data>' | curl ... --data-binary @-
	// This avoids temp files and double-popen.
	size_t url_len  = strlen(req->url);
	size_t post_len = req->post_data ? strlen(req->post_data) : 0;
	size_t ua_len   = strlen(s_user_agent);
	size_t cmd_len  = 512 + url_len + post_len * 4 + ua_len; // *4: worst-case shell escaping
	char *cmd = (char *)malloc(cmd_len);
	if (!cmd) {
		HTTP_LOG("ERROR: malloc failed for command buffer");
		resp->body = strdup("malloc failed");
		resp->body_len = resp->body ? strlen(resp->body) : 0;
		return;
	}

	int off = 0;

	if (req->post_data && req->post_data[0]) {
		// Use printf to feed POST body into curl's stdin.
		// Shell-escape the post_data using single-quote wrapping
		// (replace each ' with '\'' ).
		off += snprintf(cmd + off, cmd_len - off, "printf '%%s' '");
		for (const char *p = req->post_data; *p && (size_t)off < cmd_len - 10; p++) {
			if (*p == '\'') {
				off += snprintf(cmd + off, cmd_len - off, "'\\''");
			} else {
				cmd[off++] = *p;
			}
		}
		off += snprintf(cmd + off, cmd_len - off, "' | ");
	}

	// curl base flags (-k: skip SSL cert verification — MiSTer CA bundle is outdated)
	off += snprintf(cmd + off, cmd_len - off,
		"curl -s -k -L --max-time 30 -w '\\n%%{http_code}' -A '%s'", s_user_agent);

	if (req->post_data && req->post_data[0]) {
		const char *ct = (req->content_type && req->content_type[0])
			? req->content_type
			: "application/x-www-form-urlencoded";
		off += snprintf(cmd + off, cmd_len - off,
			" -X POST -H 'Content-Type: %s' --data-binary @-", ct);
	}

	// URL: single-quote wrapped with ' escaped as '\''
	off += snprintf(cmd + off, cmd_len - off, " '");
	for (const char *p = req->url; *p && (size_t)off < cmd_len - 10; p++) {
		if (*p == '\'') {
			off += snprintf(cmd + off, cmd_len - off, "'\\''");
		} else {
			cmd[off++] = *p;
		}
	}
	off += snprintf(cmd + off, cmd_len - off, "'");
	cmd[off] = '\0';

	// Log the command with token masked
	{
		const char *token_pos = strstr(cmd, "&t=");
		if (token_pos) {
			const char *token_end = strchr(token_pos + 3, '&');
			if (!token_end) token_end = strchr(token_pos + 3, '\'');
			if (!token_end) token_end = token_pos + 3 + strlen(token_pos + 3);
			HTTP_LOG("CMD: %.*s&t=***%s",
				(int)(token_pos - cmd), cmd, token_end);
		} else {
			HTTP_LOG("CMD: %s", cmd);
		}
	}

	FILE *fp = popen(cmd, "r");
	free(cmd);

	if (!fp) {
		HTTP_LOG("ERROR: popen() failed: %s", strerror(errno));
		resp->body = strdup("popen() failed");
		resp->body_len = resp->body ? strlen(resp->body) : 0;
		return;
	}

	// Read all output
	struct buf output;
	buf_init(&output);
	char chunk[4096];
	while (1) {
		size_t n = fread(chunk, 1, sizeof(chunk), fp);
		if (n == 0) break;
		buf_append(&output, chunk, n);
	}

	int exit_code = pclose(fp);
	HTTP_LOG("pclose exit_code=%d output_len=%zu", exit_code, output.len);

	if (!output.data || output.len == 0) {
		HTTP_LOG("ERROR: curl returned no output (exit=%d)", exit_code);
		resp->body = strdup("curl returned no output");
		resp->body_len = resp->body ? strlen(resp->body) : 0;
		free(output.data);
		return;
	}

	// Parse HTTP status code from last line (-w '\n%{http_code}')
	char *last_nl = NULL;
	for (size_t i = output.len; i > 0; i--) {
		if (output.data[i - 1] == '\n') {
			last_nl = output.data + i - 1;
			break;
		}
	}

	if (last_nl && last_nl < output.data + output.len - 1) {
		resp->http_status = atoi(last_nl + 1);
		*last_nl = '\0';
		resp->body_len = (size_t)(last_nl - output.data);
	} else {
		resp->http_status = 0;
		resp->body_len = output.len;
	}

	resp->body = output.data; // ownership transferred

	HTTP_LOG("Response: HTTP %d, %zu bytes", resp->http_status, resp->body_len);
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

static void *http_worker(void *)
{
	while (1) {
		ra_http_req req;

		// Wait for a request
		pthread_mutex_lock(&s_req_mutex);
		while (queue_count(s_req_head, s_req_tail) == 0 && !s_quit) {
			pthread_cond_wait(&s_req_cond, &s_req_mutex);
		}
		if (s_quit && queue_count(s_req_head, s_req_tail) == 0) {
			pthread_mutex_unlock(&s_req_mutex);
			break;
		}

		// Dequeue request
		req = s_req_queue[s_req_tail % QUEUE_CAP];
		s_req_tail++;
		pthread_mutex_unlock(&s_req_mutex);

		// Execute HTTP
		ra_http_resp resp;
		memset(&resp, 0, sizeof(resp));
		execute_request(&req, &resp);

		// Free request strings
		free(req.url);
		free(req.post_data);
		free(req.content_type);

		// Enqueue response for main thread
		pthread_mutex_lock(&s_resp_mutex);
		if (queue_count(s_resp_head, s_resp_tail) < QUEUE_CAP) {
			s_resp_queue[s_resp_head % QUEUE_CAP] = resp;
			s_resp_head++;
		} else {
			HTTP_LOG("WARNING: Response queue full, dropping response");
			free(resp.body);
		}
		pthread_mutex_unlock(&s_resp_mutex);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ra_http_init(void)
{
	if (s_inited) return;

	s_req_head = s_req_tail = 0;
	s_resp_head = s_resp_tail = 0;
	s_quit = false;

	pthread_mutex_init(&s_req_mutex, NULL);
	pthread_cond_init(&s_req_cond, NULL);
	pthread_mutex_init(&s_resp_mutex, NULL);

	pthread_attr_t attr;
	pthread_attr_init(&attr);

	// Run HTTP worker on CPU core #0 (same as offload thread, different from
	// the main loop which runs on core #1)
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);

	pthread_create(&s_worker, &attr, http_worker, NULL);
	pthread_attr_destroy(&attr);

	s_inited = true;
	HTTP_LOG("HTTP worker thread started");
}

void ra_http_deinit(void)
{
	if (!s_inited) return;

	HTTP_LOG("Shutting down HTTP worker...");

	pthread_mutex_lock(&s_req_mutex);
	s_quit = true;
	pthread_cond_signal(&s_req_cond);
	pthread_mutex_unlock(&s_req_mutex);

	pthread_join(s_worker, NULL);

	// Free any remaining responses
	pthread_mutex_lock(&s_resp_mutex);
	while (queue_count(s_resp_head, s_resp_tail) > 0) {
		ra_http_resp *r = &s_resp_queue[s_resp_tail % QUEUE_CAP];
		free(r->body);
		s_resp_tail++;
	}
	pthread_mutex_unlock(&s_resp_mutex);

	s_inited = false;
	HTTP_LOG("HTTP worker stopped");
}

void ra_http_request(const char *url, const char *post_data,
	const char *content_type, ra_http_callback_t callback, void *callback_data)
{
	if (!s_inited) {
		HTTP_LOG("ERROR: ra_http_request called before init");
		// Can't enqueue — call back synchronously with error
		if (callback) {
			ra_http_resp err;
			memset(&err, 0, sizeof(err));
			err.body = (char *)"HTTP not initialized";
			err.body_len = strlen(err.body);
			callback(&err, callback_data);
		}
		return;
	}

	pthread_mutex_lock(&s_req_mutex);

	if (queue_count(s_req_head, s_req_tail) >= QUEUE_CAP) {
		HTTP_LOG("WARNING: Request queue full, rejecting request to %s", url);
		pthread_mutex_unlock(&s_req_mutex);

		// Must still call callback so the caller doesn't hang
		ra_http_resp err_resp;
		memset(&err_resp, 0, sizeof(err_resp));
		err_resp.http_status = 0;
		err_resp.body = strdup("Request queue full");
		err_resp.body_len = err_resp.body ? strlen(err_resp.body) : 0;
		err_resp.callback = callback;
		err_resp.callback_data = callback_data;

		// Enqueue the error response for main-thread dispatch
		pthread_mutex_lock(&s_resp_mutex);
		if (queue_count(s_resp_head, s_resp_tail) < QUEUE_CAP) {
			s_resp_queue[s_resp_head % QUEUE_CAP] = err_resp;
			s_resp_head++;
		} else {
			free(err_resp.body);
		}
		pthread_mutex_unlock(&s_resp_mutex);
		return;
	}

	ra_http_req *r = &s_req_queue[s_req_head % QUEUE_CAP];
	r->url = strdup(url ? url : "");
	r->post_data = post_data ? strdup(post_data) : NULL;
	r->content_type = content_type ? strdup(content_type) : NULL;
	r->callback = callback;
	r->callback_data = callback_data;
	s_req_head++;

	pthread_cond_signal(&s_req_cond);
	pthread_mutex_unlock(&s_req_mutex);
}

int ra_http_poll(void)
{
	if (!s_inited) return 0;

	int dispatched = 0;

	pthread_mutex_lock(&s_resp_mutex);
	while (queue_count(s_resp_head, s_resp_tail) > 0) {
		ra_http_resp resp = s_resp_queue[s_resp_tail % QUEUE_CAP];
		s_resp_tail++;
		pthread_mutex_unlock(&s_resp_mutex);

		// Dispatch callback on main thread
		if (resp.callback) {
			resp.callback(&resp, resp.callback_data);
		}
		free(resp.body);
		dispatched++;

		pthread_mutex_lock(&s_resp_mutex);
	}
	pthread_mutex_unlock(&s_resp_mutex);

	return dispatched;
}

int ra_http_pending(void)
{
	if (!s_inited) return 0;

	int req_pending, resp_pending;

	pthread_mutex_lock(&s_req_mutex);
	req_pending = queue_count(s_req_head, s_req_tail);
	pthread_mutex_unlock(&s_req_mutex);

	pthread_mutex_lock(&s_resp_mutex);
	resp_pending = queue_count(s_resp_head, s_resp_tail);
	pthread_mutex_unlock(&s_resp_mutex);

	return req_pending + resp_pending;
}

void ra_http_set_user_agent(const char *user_agent)
{
	if (user_agent && user_agent[0])
		snprintf(s_user_agent, sizeof(s_user_agent), "%s", user_agent);
}
