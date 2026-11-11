// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// PSCLOUD slot allocation lives on the user's VM. This file only:
//   1. POST /prepare  -> datacenters plus the session key the existing ping needs
//   2. ping those datacenters here (cc_ping_datacenter)
//   3. POST /allocate -> the same fields a local lock used to fill
// There is no Sony/Gaikai call and no fallback to one on failure.

#include "cloudsession_internal.h"
#undef CC_MS_SLEEP
#include "cloudcatalog_internal.h"
#include "curl_http.h"

#include <chiaki/cloudcatalog.h>
#include <chiaki/random.h>
#include <chiaki/thread.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Hostname the app calls. CURLOPT_RESOLVE pins the TCP connection at the VM
// where pscloud_slot_server.py listens, so a stale public DNS record cannot
// send the account material anywhere else.
#define SLOT_PREPARE_URL  "http://pscloud.4cloud.pro:40200/prepare"
#define SLOT_ALLOCATE_URL "http://pscloud.4cloud.pro:40200/allocate"
#define SLOT_RESOLVE      "pscloud.4cloud.pro:40200:5.183.190.150"
#define SLOT_CONNECT_TIMEOUT_MS 10000L
#define SLOT_PREPARE_TIMEOUT_MS 600000L
#define SLOT_ALLOCATE_TIMEOUT_MS 960000L
#define SLOT_PING_DEADLINE_MS 15000
#define SLOT_PING_MAX 64
#define SLOT_DOWN_MSG "Cloud slot server is unreachable (pscloud.4cloud.pro:40200). Start the allocator on the VM and try again."

typedef struct
{
	ChiakiLog log;
	bool has_log;
	char *name;
	char *ip;
	char *session_key;
	int port;
	int bw;
	int64_t rtt_us;
	uint32_t mtu_in;
	uint32_t mtu_out;
	bool ok;
	atomic_int done;
	atomic_int handoff;
} RemotePingJob;

static void remote_progress(const ChiakiCloudProvisionConfig *cfg, const char *stage)
{
	if(cfg && cfg->progress && stage)
		cfg->progress(stage, cfg->user);
}

static bool remote_cancelled(const ChiakiCloudProvisionConfig *cfg)
{
	return cfg && cfg->is_cancelled && cfg->is_cancelled(cfg->user);
}

static void remote_set_msg(ChiakiCloudProvisionResult *out, const char *msg)
{
	if(!out)
		return;
	free(out->error_message);
	out->error_message = (msg && *msg) ? strdup(msg) : NULL;
}

static const char *remote_error_text(struct json_object *j)
{
	const char *s = cc_json_str(j, "error");
	if(!s || !*s)
		return NULL;
	if(strcmp(s, "AUTHORIZATION_FAILED") == 0 ||
	   strcmp(s, "PS_PLUS_SUBSCRIPTION_REQUIRED") == 0 ||
	   strcmp(s, "PING_TIMEOUT") == 0)
		return s;
	if(strlen(s) > 300)
		return NULL;
	if(cc_strcasestr(s, "npsso") || cc_strcasestr(s, "handshake") ||
	   cc_strcasestr(s, "launch") || cc_strcasestr(s, "token") ||
	   cc_strcasestr(s, "bearer") || cc_strcasestr(s, "code="))
		return NULL;
	return s;
}

static void remote_fail_http(ChiakiLog *log, ChiakiCloudProvisionResult *out,
	const CCHttpResponse *resp, const char *what)
{
	if(resp && resp->status_code == 401)
	{
		struct json_object *auth = (resp->data) ? json_tokener_parse(resp->data) : NULL;
		const char *text = auth ? remote_error_text(auth) : NULL;
		if(text && strcmp(text, "AUTHORIZATION_FAILED") == 0)
			remote_set_msg(out, text);
		else
			remote_set_msg(out, "╨Т╨╛╨╣╨┤╨╕╤В╨╡ ╨▓ ╨░╨║╨║╨░╤Г╨╜╤В 4cloud.pro ╤Б ╨┐╨╛╨┤╨┐╨╕╤Б╨║╨╛╨╣ ╨╜╨░ ╨╛╨▒╨╗╨░╨║╨╛ ╤Ж╨╕╤Д╤А╨╛╨▓╤Л╤Е ╨╕╨│╤А.");
		if(auth)
			json_object_put(auth);
		CHIAKI_LOGE(log, "[PSCLOUD] %s rejected (HTTP 401)", what);
		return;
	}
	struct json_object *j = (resp && resp->data) ? json_tokener_parse(resp->data) : NULL;
	const char *text = j ? remote_error_text(j) : NULL;
	if(text)
		remote_set_msg(out, text);
	else
	{
		char buf[160];
		snprintf(buf, sizeof(buf), "Cloud slot server %s failed (HTTP %ld).",
			what, resp ? resp->status_code : 0);
		remote_set_msg(out, buf);
	}
	if(j)
		json_object_put(j);
	CHIAKI_LOGE(log, "[PSCLOUD] %s", out && out->error_message ? out->error_message : what);
}

static ChiakiErrorCode remote_post(ChiakiLog *log, const char *url, const char *body,
	const char *jwt, long timeout_ms, CCHttpResponse *resp)
{
	char *auth = NULL;
	size_t auth_len = strlen("Authorization: Bearer ") + (jwt ? strlen(jwt) : 0) + 1;
	auth = (char *)malloc(auth_len);
	if(!auth)
		return CHIAKI_ERR_MEMORY;
	snprintf(auth, auth_len, "Authorization: Bearer %s", jwt ? jwt : "");
	const char *hdrs[] = {
		"Content-Type: application/json",
		"Accept: application/json",
		auth
	};
	CCHttpRequest req;
	memset(&req, 0, sizeof(req));
	req.method = "POST";
	req.url = url;
	req.headers = hdrs;
	req.header_count = 3;
	req.body = body ? body : "";
	req.timeout_ms = timeout_ms;
	req.connect_timeout_ms = SLOT_CONNECT_TIMEOUT_MS;
	req.resolve = SLOT_RESOLVE;
	req.quiet = true;
	req.noproxy = true;
	ChiakiErrorCode err = cc_http_perform(log, &req, resp);
	free(auth);
	return err;
}

static void remote_timezone(char *tz, size_t tz_sz)
{
	time_t t = time(NULL);
	struct tm lt, gt;
#ifdef _WIN32
	lt = *localtime(&t);
	gt = *gmtime(&t);
#else
	localtime_r(&t, &lt);
	gmtime_r(&t, &gt);
#endif
	gt.tm_isdst = lt.tm_isdst;
	long off = (long)difftime(mktime(&lt), mktime(&gt));
	int oh = (int)(off / 3600);
	int om = (int)((off < 0 ? -off : off) % 3600) / 60;
	snprintf(tz, tz_sz, "UTC%c%02d:%02d", off >= 0 ? '+' : '-', oh < 0 ? -oh : oh, om);
}

static struct json_object *remote_ping_obj(const char *dc, int rtt_ms, uint32_t mtu_in, uint32_t mtu_out,
	int port, const char *ip, int max_bw, bool measured)
{
	struct json_object *o = json_object_new_object();
	struct json_object *rtts = json_object_new_array();
	json_object_object_add(o, "dataCenter", json_object_new_string(dc ? dc : ""));
	json_object_object_add(o, "rtt", json_object_new_int(rtt_ms));
	json_object_array_add(rtts, json_object_new_int(rtt_ms));
	json_object_object_add(o, "rtts", rtts);
	json_object_object_add(o, "mtu_in", json_object_new_int((int)mtu_in));
	json_object_object_add(o, "mtu_out", json_object_new_int((int)mtu_out));
	json_object_object_add(o, "port", json_object_new_int(port));
	json_object_object_add(o, "publicIp", json_object_new_string(ip ? ip : ""));
	json_object_object_add(o, "maxBandwidth", json_object_new_int(max_bw));
	json_object_object_add(o, "measured", json_object_new_boolean(measured));
	return o;
}

static struct json_object *remote_find_dc(struct json_object *arr, const char *name)
{
	if(!arr || !name || json_object_get_type(arr) != json_type_array)
		return NULL;
	size_t n = json_object_array_length(arr);
	for(size_t i = 0; i < n; i++)
	{
		struct json_object *row = json_object_array_get_idx(arr, i);
		if(strcmp(cc_json_str(row, "dataCenter"), name) == 0)
			return row;
	}
	return NULL;
}

static void remote_ping_job_free(RemotePingJob *j)
{
	if(!j)
		return;
	free(j->name);
	free(j->ip);
	free(j->session_key);
	free(j);
}

static void *remote_ping_thread(void *arg)
{
	RemotePingJob *j = (RemotePingJob *)arg;
	j->rtt_us = -1;
	j->mtu_in = 0;
	j->mtu_out = 0;
	ChiakiErrorCode err = cc_ping_datacenter(j->has_log ? &j->log : NULL,
		j->ip, j->port, j->session_key, "pscloud",
		&j->rtt_us, &j->mtu_in, &j->mtu_out, NULL);
	j->ok = (err == CHIAKI_ERR_SUCCESS && j->rtt_us > 0);
	atomic_store(&j->done, 1);
	if(atomic_exchange(&j->handoff, 1) == 1)
		remote_ping_job_free(j);
	return NULL;
}

static void remote_store_picker(const ChiakiCloudProvisionConfig *cfg,
	ChiakiCloudProvisionResult *out, struct json_object *pings)
{
	struct json_object *picker = cc_json_clone(pings);
	struct json_object *prior = NULL;
	if(!picker)
		return;
	if(cfg->prior_datacenters_json && *cfg->prior_datacenters_json)
		prior = json_tokener_parse(cfg->prior_datacenters_json);
	if(prior && json_object_get_type(prior) == json_type_array)
	{
		size_t n = json_object_array_length(prior);
		for(size_t i = 0; i < n; i++)
		{
			struct json_object *row = json_object_array_get_idx(prior, i);
			const char *name = cc_json_str(row, "dataCenter");
			if(*name && !remote_find_dc(picker, name))
			{
				struct json_object *copy = cc_json_clone(row);
				if(copy)
					json_object_array_add(picker, copy);
			}
		}
	}
	if(prior)
		json_object_put(prior);
	const char *text = json_object_to_json_string(picker);
	if(text)
	{
		free(out->datacenter_pings);
		out->datacenter_pings = strdup(text);
	}
	json_object_put(picker);
}

// Parallel senkusha pings. Same deadline and abandon rules as the local path.
static ChiakiErrorCode remote_ping_all(ChiakiLog *log, const ChiakiCloudProvisionConfig *cfg,
	struct json_object *dcs, const char *session_key, struct json_object **out_pings)
{
	*out_pings = json_object_new_array();
	if(!*out_pings)
		return CHIAKI_ERR_MEMORY;
	size_t n = json_object_array_length(dcs);
	if(n > SLOT_PING_MAX)
		n = SLOT_PING_MAX;

	RemotePingJob **jobs = (RemotePingJob **)calloc(n, sizeof(RemotePingJob *));
	ChiakiThread *threads = (ChiakiThread *)calloc(n, sizeof(ChiakiThread));
	bool *threaded = (bool *)calloc(n, sizeof(bool));
	if(!jobs || !threads || !threaded)
	{
		free(jobs);
		free(threads);
		free(threaded);
		return CHIAKI_ERR_MEMORY;
	}

	remote_progress(cfg, "Pinging datacenters");
	for(size_t i = 0; i < n; i++)
	{
		struct json_object *dc = json_object_array_get_idx(dcs, i);
		const char *name = cc_json_str(dc, "dataCenter");
		const char *ip = cc_json_str(dc, "publicIp");
		int port = cc_json_int(dc, "port");
		int bw = cc_json_int(dc, "maxBandwidth");
		if(!*ip)
		{
			json_object_array_add(*out_pings, remote_ping_obj(name, 999, 0, 0, port, ip, bw, false));
			continue;
		}
		RemotePingJob *j = (RemotePingJob *)calloc(1, sizeof(RemotePingJob));
		if(!j)
		{
			json_object_array_add(*out_pings, remote_ping_obj(name, 999, 0, 0, port, ip, bw, false));
			continue;
		}
		if(log)
		{
			j->log = *log;
			j->has_log = true;
		}
		j->name = strdup(name);
		j->ip = strdup(ip);
		j->port = port;
		j->bw = bw;
		j->session_key = strdup(session_key ? session_key : "");
		atomic_init(&j->done, 0);
		atomic_init(&j->handoff, 0);
		jobs[i] = j;
		if(chiaki_thread_create(&threads[i], remote_ping_thread, j) == CHIAKI_ERR_SUCCESS)
			threaded[i] = true;
		else
			remote_ping_thread(j);
	}

	uint64_t waited = 0;
	bool cancelled = false;
	bool detached_any = false;
	size_t remaining = 0;
	for(size_t i = 0; i < n; i++)
		if(jobs[i] && threaded[i] && !atomic_load(&jobs[i]->done))
			remaining++;
	while(remaining > 0 && waited < SLOT_PING_DEADLINE_MS)
	{
		if(remote_cancelled(cfg))
		{
			cancelled = true;
			break;
		}
		CC_MS_SLEEP(100);
		waited += 100;
		remaining = 0;
		for(size_t i = 0; i < n; i++)
			if(jobs[i] && threaded[i] && !atomic_load(&jobs[i]->done))
				remaining++;
	}

	for(size_t i = 0; i < n; i++)
	{
		if(!jobs[i])
			continue;
		bool done = !threaded[i] || atomic_load(&jobs[i]->done) != 0;
		bool have_result = done;
		if(done)
		{
			if(threaded[i])
				chiaki_thread_join(&threads[i], NULL);
		}
		else
		{
			chiaki_thread_detach(&threads[i]);
			detached_any = true;
			if(atomic_exchange(&jobs[i]->handoff, 1) == 1)
				have_result = true;
		}
		if(have_result)
		{
			RemotePingJob *j = jobs[i];
			if(j->ok)
			{
				json_object_array_add(*out_pings, remote_ping_obj(j->name, (int)(j->rtt_us / 1000),
					j->mtu_in, j->mtu_out, j->port, j->ip, j->bw, true));
				CHIAKI_LOGI(log, "[PSCLOUD] ping %s = %dms", j->name, (int)(j->rtt_us / 1000));
			}
			else
			{
				json_object_array_add(*out_pings, remote_ping_obj(j->name, 999, 0, 0, j->port, j->ip, j->bw, false));
				CHIAKI_LOGI(log, "[PSCLOUD] ping %s = unreachable", j->name);
			}
			remote_ping_job_free(j);
		}
		else
		{
			struct json_object *dc = json_object_array_get_idx(dcs, i);
			json_object_array_add(*out_pings, remote_ping_obj(
				cc_json_str(dc, "dataCenter"), 999, 0, 0,
				cc_json_int(dc, "port"), cc_json_str(dc, "publicIp"),
				cc_json_int(dc, "maxBandwidth"), false));
			CHIAKI_LOGI(log, "[PSCLOUD] ping %s = abandoned", cc_json_str(dc, "dataCenter"));
		}
	}
	free(jobs);
	free(threaded);
#ifdef _WIN32
	if(!detached_any)
		free(threads);
#else
	(void)detached_any;
	free(threads);
#endif
	if(cancelled)
		return CHIAKI_ERR_CANCELED;
	return CHIAKI_ERR_SUCCESS;
}

static struct json_object *remote_select_ping(const ChiakiCloudProvisionConfig *cfg,
	struct json_object *pings, bool *ping_timeout, bool *forced_missing)
{
	*ping_timeout = false;
	*forced_missing = false;
	const char *forced = cfg->forced_datacenter;
	bool use_forced = forced && *forced && strcmp(forced, "Auto") != 0;
	size_t n = json_object_array_length(pings);
	if(n == 0)
		return NULL;
	if(use_forced)
	{
		struct json_object *row = remote_find_dc(pings, forced);
		if(!row)
			*forced_missing = true;
		return row;
	}
	struct json_object *best = NULL;
	int best_rtt = 0;
	for(size_t i = 0; i < n; i++)
	{
		struct json_object *row = json_object_array_get_idx(pings, i);
		if(!cc_json_bool(row, "measured"))
			continue;
		int rtt = cc_json_int(row, "rtt");
		if(!best || rtt < best_rtt)
		{
			best = row;
			best_rtt = rtt;
		}
	}
	if(!best || best_rtt > 80)
		*ping_timeout = true;
	return best;
}

ChiakiErrorCode cc_pscloud_remote_allocate(ChiakiLog *log,
	const ChiakiCloudProvisionConfig *cfg, const char *duid,
	const char *entitlement_id,
	ChiakiCloudProvisionResult *out)
{
	if(!cfg || !out)
		return CHIAKI_ERR_INVALID_DATA;
	snprintf(out->platform, sizeof(out->platform), "ps5");
	snprintf(out->entitlement_id, sizeof(out->entitlement_id), "%s", entitlement_id ? entitlement_id : "");
	if(!entitlement_id || !*entitlement_id)
	{
		remote_set_msg(out, "No entitlement id for this title.");
		return CHIAKI_ERR_INVALID_DATA;
	}
	if(!cfg->npsso || !*cfg->npsso)
	{
		remote_set_msg(out, "AUTHORIZATION_FAILED");
		return CHIAKI_ERR_UNKNOWN;
	}
	if(!cfg->fourcloud_jwt || !*cfg->fourcloud_jwt)
	{
		remote_set_msg(out, "╨Т╨╛╨╣╨┤╨╕╤В╨╡ ╨▓ ╨░╨║╨║╨░╤Г╨╜╤В 4cloud.pro ╤Б ╨┐╨╛╨┤╨┐╨╕╤Б╨║╨╛╨╣ ╨╜╨░ ╨╛╨▒╨╗╨░╨║╨╛ ╤Ж╨╕╤Д╤А╨╛╨▓╤Л╤Е ╨╕╨│╤А.");
		return CHIAKI_ERR_UNKNOWN;
	}

	uint8_t nonce_raw[16];
	char nonce[33];
	if(chiaki_random_bytes_crypt(nonce_raw, sizeof(nonce_raw)) != CHIAKI_ERR_SUCCESS)
		return CHIAKI_ERR_UNKNOWN;
	for(int i = 0; i < 16; i++)
		snprintf(nonce + (i * 2), 3, "%02x", nonce_raw[i]);

	char lang[16];
	char tz[16];
	chiaki_cloud_gaikai_language(cfg->game_language ? cfg->game_language : "", lang, sizeof(lang));
	remote_timezone(tz, sizeof(tz));
	int bitrate = cfg->bitrate_kbps > 0 ? cfg->bitrate_kbps : 25000;
	int resolution = cfg->resolution > 0 ? cfg->resolution : 1080;

	struct json_object *prep = json_object_new_object();
	json_object_object_add(prep, "npsso", json_object_new_string(cfg->npsso));
	json_object_object_add(prep, "entitlementId", json_object_new_string(entitlement_id));
	json_object_object_add(prep, "language", json_object_new_string(lang));
	json_object_object_add(prep, "duid", json_object_new_string(duid ? duid : ""));
	json_object_object_add(prep, "service", json_object_new_string("pscloud"));
	json_object_object_add(prep, "resolution", json_object_new_int(resolution));
	json_object_object_add(prep, "bitrateKbps", json_object_new_int(bitrate));
	json_object_object_add(prep, "timeZone", json_object_new_string(tz));
	json_object_object_add(prep, "nonce", json_object_new_string(nonce));
	const char *prep_body = json_object_to_json_string(prep);

	remote_progress(cfg, "Starting cloud session...");
	CCHttpResponse prep_resp;
	memset(&prep_resp, 0, sizeof(prep_resp));
	ChiakiErrorCode err = remote_post(log, SLOT_PREPARE_URL, prep_body, cfg->fourcloud_jwt, SLOT_PREPARE_TIMEOUT_MS, &prep_resp);
	json_object_put(prep);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		cc_http_response_fini(&prep_resp);
		remote_set_msg(out, SLOT_DOWN_MSG);
		CHIAKI_LOGE(log, "[PSCLOUD] prepare transport failed");
		return err;
	}
	if(prep_resp.status_code != 200 || !prep_resp.data)
	{
		remote_fail_http(log, out, &prep_resp, "prepare");
		cc_http_response_fini(&prep_resp);
		return CHIAKI_ERR_UNKNOWN;
	}

	struct json_object *prep_json = json_tokener_parse(prep_resp.data);
	cc_http_response_fini(&prep_resp);
	if(!prep_json)
	{
		remote_set_msg(out, "Cloud slot server returned an invalid prepare response.");
		return CHIAKI_ERR_UNKNOWN;
	}
	char *handle = strdup(cc_json_str(prep_json, "handle"));
	char *session_key = strdup(cc_json_str(prep_json, "session_key"));
	struct json_object *dcs_src = cc_json_arr(prep_json, "datacenters");
	struct json_object *dcs = dcs_src ? cc_json_clone(dcs_src) : NULL;
	json_object_put(prep_json);
	if(!handle || !*handle || !session_key || !*session_key || !dcs ||
	   json_object_array_length(dcs) == 0)
	{
		free(handle);
		free(session_key);
		if(dcs)
			json_object_put(dcs);
		remote_set_msg(out, "Cloud slot server returned no datacenters to ping.");
		return CHIAKI_ERR_UNKNOWN;
	}
	CHIAKI_LOGI(log, "[PSCLOUD] prepare ok, %u datacenters, language %s",
		(unsigned)json_object_array_length(dcs), lang);

	if(remote_cancelled(cfg))
	{
		free(handle);
		free(session_key);
		json_object_put(dcs);
		return CHIAKI_ERR_CANCELED;
	}

	struct json_object *pings = NULL;
	err = remote_ping_all(log, cfg, dcs, session_key, &pings);
	json_object_put(dcs);
	free(session_key);
	session_key = NULL;
	if(err != CHIAKI_ERR_SUCCESS)
	{
		if(pings)
			json_object_put(pings);
		free(handle);
		if(err == CHIAKI_ERR_CANCELED)
			return err;
		remote_set_msg(out, "Datacenter ping failed.");
		return err;
	}
	remote_store_picker(cfg, out, pings);

	bool ping_timeout = false;
	bool forced_missing = false;
	struct json_object *selected = remote_select_ping(cfg, pings, &ping_timeout, &forced_missing);
	if(forced_missing)
	{
		char msg[128];
		snprintf(msg, sizeof(msg), "Selected datacenter '%s' not available",
			cfg->forced_datacenter ? cfg->forced_datacenter : "");
		remote_set_msg(out, msg);
		json_object_put(pings);
		free(handle);
		return CHIAKI_ERR_UNKNOWN;
	}
	if(ping_timeout || !selected)
	{
		remote_set_msg(out, "PING_TIMEOUT");
		CHIAKI_LOGE(log, "[PSCLOUD] datacenter ping above 80ms or unreachable");
		json_object_put(pings);
		free(handle);
		return CHIAKI_ERR_UNKNOWN;
	}

	const char *selected_name = cc_json_str(selected, "dataCenter");
	int selected_rtt = cc_json_int(selected, "rtt");
	{
		char stage[96];
		if(cc_json_bool(selected, "measured"))
			snprintf(stage, sizeof(stage), "Selecting datacenter %s (%d ms)", selected_name, selected_rtt);
		else
			snprintf(stage, sizeof(stage), "Selecting datacenter %s", selected_name);
		remote_progress(cfg, stage);
		CHIAKI_LOGI(log, "[PSCLOUD] selected %s (%d ms)", selected_name, selected_rtt);
	}

	struct json_object *alloc = json_object_new_object();
	json_object_object_add(alloc, "handle", json_object_new_string(handle));
	json_object_object_add(alloc, "selectedDataCenter", json_object_new_string(selected_name));
	json_object_object_add(alloc, "pingResults", json_object_get(pings));
	const char *alloc_body = json_object_to_json_string(alloc);
	CCHttpResponse alloc_resp;
	memset(&alloc_resp, 0, sizeof(alloc_resp));
	err = remote_post(log, SLOT_ALLOCATE_URL, alloc_body, cfg->fourcloud_jwt, SLOT_ALLOCATE_TIMEOUT_MS, &alloc_resp);
	json_object_put(alloc);
	json_object_put(pings);
	free(handle);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		cc_http_response_fini(&alloc_resp);
		remote_set_msg(out, SLOT_DOWN_MSG);
		CHIAKI_LOGE(log, "[PSCLOUD] allocate transport failed");
		return err;
	}
	if(alloc_resp.status_code != 200 || !alloc_resp.data)
	{
		remote_fail_http(log, out, &alloc_resp, "allocate");
		cc_http_response_fini(&alloc_resp);
		return CHIAKI_ERR_UNKNOWN;
	}

	struct json_object *got = json_tokener_parse(alloc_resp.data);
	cc_http_response_fini(&alloc_resp);
	if(!got)
	{
		remote_set_msg(out, "Cloud slot server returned an invalid allocate response.");
		return CHIAKI_ERR_UNKNOWN;
	}
	const char *ip = cc_json_str(got, "server_ip");
	if(!ip || !*ip)
	{
		const char *text = remote_error_text(got);
		remote_set_msg(out, text ? text : "Cloud slot server returned an empty stream address.");
		json_object_put(got);
		return CHIAKI_ERR_UNKNOWN;
	}
	snprintf(out->server_ip, sizeof(out->server_ip), "%s", ip);
	out->server_port = cc_json_int(got, "server_port");
	free(out->handshake_key);
	free(out->launch_spec);
	free(out->session_id);
	out->handshake_key = strdup(cc_json_str(got, "handshake_key"));
	out->launch_spec = strdup(cc_json_str(got, "launch_spec"));
	out->session_id = strdup(cc_json_str(got, "session_id"));
	if(cc_json_str(got, "entitlement_id")[0])
		snprintf(out->entitlement_id, sizeof(out->entitlement_id), "%s", cc_json_str(got, "entitlement_id"));
	if(cc_json_str(got, "platform")[0])
		snprintf(out->platform, sizeof(out->platform), "%s", cc_json_str(got, "platform"));
	out->psn_wrapper_type = cc_json_has(got, "psn_wrapper_type")
		? (uint8_t)cc_json_int(got, "psn_wrapper_type") : 0x01;
	out->mtu_in = (uint32_t)cc_json_int(got, "mtu_in");
	out->mtu_out = (uint32_t)cc_json_int(got, "mtu_out");
	out->rtt_us = (uint64_t)cc_json_int(got, "rtt_us");
	if(out->mtu_in == 0)
		out->mtu_in = 1454;
	if(out->mtu_out == 0)
		out->mtu_out = 1254;
	json_object_put(got);
	if(!out->handshake_key || !out->launch_spec || !out->session_id)
		return CHIAKI_ERR_MEMORY;
	CHIAKI_LOGI(log, "[PSCLOUD] stream %s:%d", out->server_ip, out->server_port);
	return CHIAKI_ERR_SUCCESS;
}
