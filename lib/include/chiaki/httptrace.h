// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Optional per-thread observer for cc_http_perform. The cloud-launch console
// installs this on the provisioning thread. Other threads are unaffected.
// phase 0 runs before any socket is opened; returning false cancels the send.

#ifndef CHIAKI_HTTPTRACE_H
#define CHIAKI_HTTPTRACE_H

#include <chiaki/common.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chiaki_http_exchange_t
{
	int phase; /**< 0 = about to send, 1 = finished (or transport error) */
	int index; /**< 1-based, same value for the request and its response */
	const char *method;
	const char *url;
	const char *request_headers; /**< "Name: value\n" lines, may be empty */
	const char *request_body;
	size_t request_body_len;
	const char *fallback_user_agent; /**< libcurl UA used when no User-Agent header is set */
	long status_code;
	const char *response_headers;
	size_t response_headers_len;
	const char *response_body;
	size_t response_body_len;
	const char *redirect_url;
	const char *transport_error; /**< non-NULL when curl itself failed */
	long elapsed_ms;
} ChiakiHttpExchange;

/** phase 0: return false to skip the send. phase 1: the return value is ignored. */
typedef bool (*ChiakiHttpInspectFn)(void *user, const ChiakiHttpExchange *exchange);

/** Install or clear (fn == NULL) the inspector for the calling thread only. */
CHIAKI_EXPORT void chiaki_http_set_thread_inspector(ChiakiHttpInspectFn fn, void *user);

#ifdef __cplusplus
}
#endif

#endif
