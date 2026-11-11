// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "curl_http.h"
#include <chiaki/httptrace.h>

#include <curl/curl.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // strncasecmp

#define CHIAKI_HTTP_DEFAULT_TIMEOUT_MS 30000L
#define CHIAKI_HTTP_USER_AGENT "pylux-cloudcatalog/1.0"

#if defined(_MSC_VER)
#define CC_THREAD_LOCAL __declspec(thread)
#else
#define CC_THREAD_LOCAL __thread
#endif

static CC_THREAD_LOCAL ChiakiHttpInspectFn cc_tls_inspect = NULL;
static CC_THREAD_LOCAL void *cc_tls_inspect_user = NULL;
static CC_THREAD_LOCAL int cc_tls_http_index = 0;

CHIAKI_EXPORT void chiaki_http_set_thread_inspector(ChiakiHttpInspectFn fn, void *user)
{
	cc_tls_inspect = fn;
	cc_tls_inspect_user = user;
	if(!fn)
		cc_tls_http_index = 0;
}

static char *cc_join_request_headers(const CCHttpRequest *request)
{
	size_t total = 1;
	for(size_t i = 0; i < request->header_count; i++)
	{
		if(request->headers[i])
			total += strlen(request->headers[i]) + 1;
	}
	char *out = (char *)malloc(total);
	if(!out)
		return NULL;
	char *p = out;
	for(size_t i = 0; i < request->header_count; i++)
	{
		const char *h = request->headers[i];
		if(!h)
			continue;
		size_t n = strlen(h);
		memcpy(p, h, n);
		p += n;
		*p++ = '\n';
	}
	*p = '\0';
	return out;
}

static size_t cc_request_body_len(const CCHttpRequest *request)
{
	if(!request->body)
		return 0;
	if(request->body_len > 0)
		return request->body_len;
	return strlen(request->body);
}

// Returns false only when a phase-0 inspector refuses the send.
static bool cc_inspect(int phase, int index, const CCHttpRequest *request,
	const char *response_headers, size_t response_headers_len,
	const char *response_body, size_t response_body_len,
	long status_code, const char *redirect_url,
	const char *transport_error, long elapsed_ms)
{
	if(!cc_tls_inspect)
		return true;
	char *joined = cc_join_request_headers(request);
	ChiakiHttpExchange ex;
	memset(&ex, 0, sizeof(ex));
	ex.phase = phase;
	ex.index = index;
	ex.method = (request->method && request->method[0]) ? request->method : "GET";
	ex.url = request->url;
	ex.request_headers = joined ? joined : "";
	ex.request_body = request->body;
	ex.request_body_len = cc_request_body_len(request);
	ex.fallback_user_agent = CHIAKI_HTTP_USER_AGENT;
	ex.status_code = status_code;
	ex.response_headers = response_headers;
	ex.response_headers_len = response_headers_len;
	ex.response_body = response_body;
	ex.response_body_len = response_body_len;
	ex.redirect_url = redirect_url;
	ex.transport_error = transport_error;
	ex.elapsed_ms = elapsed_ms;
	bool ok = cc_tls_inspect(cc_tls_inspect_user, &ex);
	free(joined);
	return ok;
}

typedef struct grow_buffer_t
{
	char *data;
	size_t size;
} GrowBuffer;

static size_t grow_buffer_write(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t realsize = size * nmemb;
	GrowBuffer *buf = (GrowBuffer *)userdata;
	char *tmp = realloc(buf->data, buf->size + realsize + 1);
	if(!tmp)
	{
		free(buf->data);
		buf->data = NULL;
		buf->size = 0;
		return 0;
	}
	buf->data = tmp;
	memcpy(&buf->data[buf->size], ptr, realsize);
	buf->size += realsize;
	buf->data[buf->size] = 0;
	return realsize;
}

// Mask the value following each credential key in buf (in place). Handles the forms
// "npsso=XYZ", "Authorization: Bearer XYZ", and JSON "access_token":"XYZ". Verbose HTTP
// logs are meant to be shared for debugging (e.g. ios/logs/pylux.log streams at
// CHIAKI_LOG_ALL in Debug builds), so credentials must never appear in them verbatim.
static void redact_credentials(char *buf, size_t len)
{
	static const char *const keys[] = {
		"npsso", "jsessionid", "bearer", "access_token", "refresh_token", "id_token", "code",
		// Gaikai stream secrets: the allocate response's handshake key / launch spec
		// and the live session header are enough to hijack the stream they protect.
		"handshakekey", "launchspecification", "x-gaikai-session",
	};
	for(size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
	{
		size_t key_len = strlen(keys[k]);
		for(size_t i = 0; i + key_len < len; i++)
		{
			if(strncasecmp(buf + i, keys[k], key_len) != 0)
				continue;
			// Require a token boundary before the key so "code" doesn't hit "status_code".
			if(i > 0 && (isalnum((unsigned char)buf[i - 1]) || buf[i - 1] == '_'))
				continue;
			size_t j = i + key_len;
			// Skip the key/value separator ("=", ": ", "\":\"", "Bearer ").
			size_t sep = j;
			while(sep < len && (buf[sep] == '"' || buf[sep] == ':' || buf[sep] == '=' || buf[sep] == ' '))
				sep++;
			if(sep == j)
				continue; // key not followed by a separator -> not a credential assignment
			for(; sep < len; sep++)
			{
				char c = buf[sep];
				if(c == '&' || c == '"' || c == '\'' || c == ';' || c == ',' ||
				   c == ' ' || c == '\r' || c == '\n')
					break;
				buf[sep] = '*';
			}
			i = sep;
		}
	}
}

static bool cc_http_trace_secrets(void)
{
	const char *v = getenv("CHIAKI_CLOUD_HTTP_TRACE_SECRETS");
	return v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T');
}

static bool cc_http_want_trace(ChiakiLog *log)
{
	if(!log)
		return false;
	const char *v = getenv("CHIAKI_CLOUD_HTTP_TRACE");
	if(v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F'))
		return false;
	if(v && v[0])
		return true;
	return (log->level_mask & CHIAKI_LOG_VERBOSE) != 0;
}

static void log_http_blob(ChiakiLog *log, ChiakiLogLevel level, const char *label, const char *data, size_t size)
{
	if(!log || !data || !size)
	{
		if(log && label)
			CHIAKI_LOGI(log, "%s (empty)", label);
		return;
	}
	char *copy = malloc(size + 1);
	if(!copy)
		return;
	memcpy(copy, data, size);
	copy[size] = 0;
	if(!cc_http_trace_secrets())
		redact_credentials(copy, size);
	if(label && *label)
		chiaki_log(log, level, "%s", label);
	// chiaki_cloud.log lines: split large bodies so nothing is truncated silently.
	const size_t chunk = 3500;
	for(size_t off = 0; off < size; off += chunk)
	{
		size_t n = size - off;
		if(n > chunk)
			n = chunk;
		char *part = malloc(n + 1);
		if(!part)
			break;
		memcpy(part, copy + off, n);
		part[n] = 0;
		chiaki_log(log, level, "%s", part);
		free(part);
	}
	free(copy);
}

static void log_verbose_redacted(ChiakiLog *log, const char *label, const char *data, size_t size)
{
	log_http_blob(log, CHIAKI_LOG_VERBOSE, label, data, size);
}

static void cc_http_log_outgoing(ChiakiLog *log, const CCHttpRequest *request)
{
	const char *method = (request->method && *request->method) ? request->method : "GET";
	CHIAKI_LOGI(log, "[HTTP] >>> %s %s", method, request->url ? request->url : "(null)");
	for(size_t i = 0; i < request->header_count; i++)
	{
		const char *h = request->headers[i];
		if(!h)
			continue;
		char line[1024];
		snprintf(line, sizeof(line), "%s", h);
		if(!cc_http_trace_secrets())
			redact_credentials(line, strlen(line));
		CHIAKI_LOGI(log, "[HTTP] >>> hdr: %s", line);
	}
	if(request->body && (request->body_len > 0 || request->body[0]))
	{
		size_t blen = request->body_len > 0 ? (size_t)request->body_len : strlen(request->body);
		log_http_blob(log, CHIAKI_LOG_INFO, "[HTTP] >>> body:", request->body, blen);
	}
}

static void cc_http_log_incoming(ChiakiLog *log, const CCHttpRequest *request, const CCHttpResponse *response)
{
	CHIAKI_LOGI(log, "[HTTP] <<< %ld %s", response->status_code,
		request->url ? request->url : "(null)");
	if(response->redirect_url && *response->redirect_url)
	{
		char redir[2048];
		snprintf(redir, sizeof(redir), "%s", response->redirect_url);
		if(!cc_http_trace_secrets())
			redact_credentials(redir, strlen(redir));
		CHIAKI_LOGI(log, "[HTTP] <<< redirect: %s", redir);
	}
	if(response->headers && response->headers_size)
		log_http_blob(log, CHIAKI_LOG_INFO, "[HTTP] <<< headers:", response->headers, response->headers_size);
	if(response->data && response->size)
		log_http_blob(log, CHIAKI_LOG_INFO, "[HTTP] <<< body:", response->data, response->size);
}

static int cc_http_debug_cb(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr)
{
	(void)handle;
	ChiakiLog *log = (ChiakiLog *)userptr;
	if(!log || !(log->level_mask & CHIAKI_LOG_VERBOSE))
		return 0;
	switch(type)
	{
		case CURLINFO_HEADER_OUT:
			log_verbose_redacted(log, ">>> HTTP Request Headers:", data, size);
			break;
		case CURLINFO_DATA_OUT:
			log_verbose_redacted(log, ">>> HTTP Request Body:", data, size);
			break;
		case CURLINFO_HEADER_IN:
			log_verbose_redacted(log, "<<< HTTP Response Headers:", data, size);
			break;
		case CURLINFO_DATA_IN:
			log_verbose_redacted(log, "<<< HTTP Response Body:", data, size);
			break;
		default:
			break;
	}
	return 0;
}

static CURL *easy_init_logged(ChiakiLog *log)
{
	CURL *curl = curl_easy_init();
	if(!curl)
		return NULL;

	// These requests run on worker threads alongside session/holepunch threads; without
	// NOSIGNAL, a DNS timeout on a libcurl built without the threaded resolver raises
	// SIGALRM/longjmp in a multithreaded process.
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	// mbedTLS (Android and other non-system-trust backends) needs the CA bundle
	// path explicitly. Harmless when the env var is unset or on Secure Transport.
	const char *ca_bundle = getenv("CHIAKI_CA_BUNDLE");
	if(ca_bundle && *ca_bundle)
		curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);

	if(log && cc_http_want_trace(log))
	{
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, cc_http_debug_cb);
		curl_easy_setopt(curl, CURLOPT_DEBUGDATA, log);
	}
	return curl;
}

CHIAKI_EXPORT ChiakiErrorCode cc_http_perform(
	ChiakiLog *log,
	const CCHttpRequest *request,
	CCHttpResponse *response)
{
	if(!request || !request->url || !response)
		return CHIAKI_ERR_INVALID_DATA;

	memset(response, 0, sizeof(*response));
	const bool trace = cc_http_want_trace(log) && !request->quiet;
	const bool inspect = cc_tls_inspect != NULL && !request->quiet;
	int exchange_index = 0;
	if(inspect)
	{
		exchange_index = ++cc_tls_http_index;
		if(!cc_inspect(0, exchange_index, request, NULL, 0, NULL, 0, 0, NULL, NULL, 0))
			return CHIAKI_ERR_CANCELED;
	}

	CURL *curl = easy_init_logged(log);
	if(!curl)
		return CHIAKI_ERR_MEMORY;

	ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
	struct curl_slist *header_list = NULL;
	struct curl_slist *resolve_list = NULL;
	GrowBuffer body_buf = { 0 };
	GrowBuffer header_buf = { 0 };

	for(size_t i = 0; i < request->header_count; i++)
	{
		struct curl_slist *next = curl_slist_append(header_list, request->headers[i]);
		if(!next)
		{
			err = CHIAKI_ERR_MEMORY;
			goto cleanup;
		}
		header_list = next;
	}

	curl_easy_setopt(curl, CURLOPT_URL, request->url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, CHIAKI_HTTP_USER_AGENT);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, request->follow_redirects ? 1L : 0L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
		request->timeout_ms > 0 ? request->timeout_ms : CHIAKI_HTTP_DEFAULT_TIMEOUT_MS);
	if(request->connect_timeout_ms > 0)
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, request->connect_timeout_ms);
	if(request->quiet)
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
	if(request->noproxy)
	{
		curl_easy_setopt(curl, CURLOPT_PROXY, "");
		curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
	}
	if(request->resolve && request->resolve[0])
	{
		resolve_list = curl_slist_append(NULL, request->resolve);
		if(!resolve_list)
		{
			err = CHIAKI_ERR_MEMORY;
			goto cleanup;
		}
		curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve_list);
	}
	if(header_list)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

	if(request->method && strcmp(request->method, "POST") == 0)
	{
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		const char *body = request->body ? request->body : "";
		curl_off_t len = (curl_off_t)(request->body_len > 0 ? request->body_len : strlen(body));
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, len);
	}
	else if(request->method && strcmp(request->method, "GET") != 0)
	{
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request->method);
	}

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, grow_buffer_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_buf);
	// Always capture response headers when tracing or when the launch console is attached.
	if(request->capture_headers || trace || inspect)
	{
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, grow_buffer_write);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_buf);
	}

	if(trace)
		cc_http_log_outgoing(log, request);

	CURLcode res = curl_easy_perform(curl);
	if(res != CURLE_OK)
	{
		if(log)
			CHIAKI_LOGE(log, "cc_http_perform: %s (%s)", curl_easy_strerror(res), request->url);
		err = CHIAKI_ERR_NETWORK;
		if(inspect)
			cc_inspect(1, exchange_index, request,
				header_buf.data, header_buf.size, body_buf.data, body_buf.size,
				0, NULL, curl_easy_strerror(res), 0);
		goto cleanup;
	}

	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	response->status_code = status;

	char *redirect = NULL;
	curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect);
	if(redirect)
		response->redirect_url = strdup(redirect);

	response->data = body_buf.data;
	response->size = body_buf.size;
	body_buf.data = NULL;
	if(request->capture_headers || trace || inspect)
	{
		response->headers = header_buf.data;
		response->headers_size = header_buf.size;
		header_buf.data = NULL;
	}

	if(inspect)
	{
		double secs = 0;
		curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &secs);
		cc_inspect(1, exchange_index, request,
			response->headers, response->headers_size,
			response->data, response->size,
			response->status_code, response->redirect_url, NULL,
			(long)(secs * 1000.0));
	}

	if(trace && (err == CHIAKI_ERR_SUCCESS || response->data || response->headers))
		cc_http_log_incoming(log, request, response);

cleanup:
	free(body_buf.data);
	free(header_buf.data);
	if(header_list)
		curl_slist_free_all(header_list);
	if(resolve_list)
		curl_slist_free_all(resolve_list);
	curl_easy_cleanup(curl);
	return err;
}

CHIAKI_EXPORT void cc_http_response_fini(CCHttpResponse *response)
{
	if(!response)
		return;
	free(response->data);
	free(response->headers);
	free(response->redirect_url);
	memset(response, 0, sizeof(*response));
}

CHIAKI_EXPORT ChiakiErrorCode cc_http_make_bearer_header(char **out, const char *token)
{
	if(!out || !token)
		return CHIAKI_ERR_INVALID_DATA;
	static const char fmt[] = "Authorization: Bearer %s";
	size_t len = sizeof(fmt) + strlen(token);
	*out = malloc(len);
	if(!*out)
		return CHIAKI_ERR_MEMORY;
	snprintf(*out, len, fmt, token);
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode cc_http_make_cookie_header(
	char **out, const char *name, const char *value)
{
	if(!out || !name || !value)
		return CHIAKI_ERR_INVALID_DATA;
	static const char fmt[] = "Cookie: %s=%s";
	size_t len = sizeof(fmt) + strlen(name) + strlen(value);
	*out = malloc(len);
	if(!*out)
		return CHIAKI_ERR_MEMORY;
	snprintf(*out, len, fmt, name, value);
	return CHIAKI_ERR_SUCCESS;
}
