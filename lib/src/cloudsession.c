// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Cloud session provisioning -- orchestrator + public entry points.
// Mirrors cloudcatalog.c. Routes PSNOW (Kamaji resolve -> Gaikai allocate) vs
// PSCLOUD (Gaikai allocate directly on the entitlementId), threads one shared
// DUID through both, and performs the one-shot noGameForEntitlementId fallback
// (re-run the full Kamaji resolve when an owned fast-path entitlement is
// rejected by Gaikai).

#include "cloudsession_internal.h"
#include "curl_http.h"

#include <chiaki/cloudsession.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>             // INET6_ADDRSTRLEN (needed by holepunch.h)
#endif
#include <chiaki/remote/holepunch.h> // chiaki_holepunch_generate_client_device_uid

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void result_init(ChiakiCloudProvisionResult *out)
{
	memset(out, 0, sizeof(*out));
	out->err = CHIAKI_ERR_UNKNOWN;
	out->server_port = 0;
}

CHIAKI_EXPORT void chiaki_cloud_provision_result_fini(ChiakiCloudProvisionResult *out)
{
	if(!out)
		return;
	free(out->handshake_key);
	free(out->launch_spec);
	free(out->session_id);
	free(out->datacenter_pings);
	free(out->error_message);
	out->handshake_key = NULL;
	out->launch_spec = NULL;
	out->session_id = NULL;
	out->datacenter_pings = NULL;
	out->error_message = NULL;
}

// --- Pre-flight authorization check (was each platform's checkAuthorization) ---
// POST the service's authorize parameters to authorizeCheck with the npsso cookie;
// HTTP 200/204 means the NPSSO is still valid. Scopes are SPACE-separated here (they
// go in a JSON body), unlike the %20-encoded OAuth-query scopes in
// cloudsession_kamaji.c. PS5 uses the same PS Now pre-flight as PS4/PS3: that is
// the check that precedes a session Gaikai will lock and allocate.
#define CA_URL            "https://ca.account.sony.com/api/authz/v3/oauth/authorizeCheck"
#define CA_PSNOW_CLIENT   CS_PSNOW_CLIENT_ID
#define CA_PSNOW_SCOPE    "kamaji:commerce_native kamaji:commerce_container kamaji:lists kamaji:s2s.subscriptionsPremium.get"
#define CA_PSNOW_REDIR    CS_PSNOW_REDIRECT
#define CA_PSNOW_UA       CS_PSNOW_USER_AGENT

// Validate the NPSSO before any real work (silently, like the old platform pre-flight).
// Returns SUCCESS on HTTP 200/204; any other status / transport error -> failure.
static ChiakiErrorCode cc_authorize_check(ChiakiLog *log,
	const ChiakiCloudProvisionConfig *cfg, const char *duid)
{
	const char *client_id = CA_PSNOW_CLIENT;
	const char *scope     = CA_PSNOW_SCOPE;
	const char *redirect  = CA_PSNOW_REDIR;
	const char *ua        = CA_PSNOW_UA;

	char body[768];
	snprintf(body, sizeof(body),
		"{\"client_id\":\"%s\",\"scope\":\"%s\",\"redirect_uri\":\"%s\","
		"\"response_type\":\"code\",\"service_entity\":\"urn:service-entity:psn\",\"duid\":\"%s\"}",
		client_id, scope, redirect, duid);

	char *h_cookie = NULL;
	if(cc_http_make_cookie_header(&h_cookie, "npsso", cfg->npsso) != CHIAKI_ERR_SUCCESS)
		return CHIAKI_ERR_UNKNOWN;
	char ua_hdr[512];
	snprintf(ua_hdr, sizeof(ua_hdr), "User-Agent: %s", ua);
	const char *hdrs[] = {
		"Content-Type: application/json; charset=UTF-8",
		ua_hdr,
		h_cookie
	};
	CCHttpRequest req = { 0 };
	req.method = "POST"; req.url = CA_URL;
	req.headers = hdrs; req.header_count = 3; req.body = body;
	CCHttpResponse resp = { 0 };
	ChiakiErrorCode e = cc_http_perform(log, &req, &resp);
	long status = resp.status_code;
	cc_http_response_fini(&resp);
	free(h_cookie);
	if(e != CHIAKI_ERR_SUCCESS)
		return e;
	if(status >= 200 && status < 300)   // any 2xx (the original accepted all QNetworkReply::NoError)
		return CHIAKI_ERR_SUCCESS;
	CHIAKI_LOGE(log, "[CLOUDSESSION] authorizeCheck failed (HTTP %ld); NPSSO likely expired", status);
	return CHIAKI_ERR_UNKNOWN;
}

static bool cp_cancelled(const ChiakiCloudProvisionConfig *cfg)
{
	return cfg->is_cancelled && cfg->is_cancelled(cfg->user);
}

// One provisioning attempt: PSNOW resolves via Kamaji then allocates via Gaikai;
// PSCLOUD asks the slot host (game_identifier is already the PS5 entitlementId).
static ChiakiErrorCode provision_once(ChiakiLog *log,
	const ChiakiCloudProvisionConfig *cfg, const char *duid,
	ChiakiCloudProvisionResult *out)
{
	bool pscloud = cfg->service_type && strcmp(cfg->service_type, "pscloud") == 0;
	char entitlement[128] = "";
	char platform[8] = "ps4";

	if(pscloud)
	{
		snprintf(entitlement, sizeof(entitlement), "%s", cfg->game_identifier ? cfg->game_identifier : "");
		snprintf(platform, sizeof(platform), "ps5");
	}
	else
	{
		char *kerr = NULL;
		ChiakiErrorCode e = cc_kamaji_resolve(log, cfg, duid, entitlement, platform, &kerr);
		if(e != CHIAKI_ERR_SUCCESS)
		{
			if(kerr) { free(out->error_message); out->error_message = kerr; }
			return e;
		}
		free(kerr);
	}

	if(pscloud)
		return cc_pscloud_remote_allocate(log, cfg, duid, entitlement, out);
	return cc_gaikai_allocate(log, cfg, duid, platform, entitlement, out);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_cloud_provision_session(
	const ChiakiCloudProvisionConfig *cfg,
	ChiakiCloudProvisionResult *out,
	ChiakiLog *log)
{
	if(!out)
		return CHIAKI_ERR_INVALID_DATA;
	result_init(out);   // init before any other check so the caller's result_fini is always safe
	if(!cfg || !cfg->npsso || !*cfg->npsso)
	{
		out->err = CHIAKI_ERR_INVALID_DATA;
		return out->err;
	}

	// NULL fields treated as "" (documented config contract) — normalise before first use.
	ChiakiCloudProvisionConfig c = *cfg;
	#define CC_NZ(f) do { if(!c.f) c.f = ""; } while(0)
	CC_NZ(service_type); CC_NZ(game_identifier); CC_NZ(npsso);
	CC_NZ(store_country); CC_NZ(store_lang); CC_NZ(game_language);
	CC_NZ(owned_entitlement_id); CC_NZ(owned_platform);
	CC_NZ(forced_datacenter); CC_NZ(prior_datacenters_json);
	CC_NZ(fourcloud_jwt);
	#undef CC_NZ
	cfg = &c;

	// Cancelled before any work (e.g. the user backed out while the platform was
	// still spinning up the worker): return without touching the network.
	if(cp_cancelled(cfg))
	{
		out->err = CHIAKI_ERR_CANCELED;
		return out->err;
	}

	// One shared client device uid for the Kamaji + Gaikai OAuth exchanges.
	char duid[64];
	size_t duid_size = sizeof(duid);
	if(chiaki_holepunch_generate_client_device_uid(duid, &duid_size) != CHIAKI_ERR_SUCCESS)
	{
		out->err = CHIAKI_ERR_UNKNOWN;
		return out->err;
	}

	// PSNOW pre-flight: validate the NPSSO before any real work (silently, like the
	// old per-platform checkAuthorization) so an expired token fails fast with no
	// progress UI. Platforms map AUTHORIZATION_FAILED -> "token expired, please
	// re-login" — so only emit it when the server actually answered (non-2xx). A
	// transport failure (offline, DNS, TLS) says nothing about the token; sending
	// the user to re-login for a network blip would be wrong, and platforms show
	// unmatched messages verbatim in their generic error dialog.
	// PSCLOUD does this check on the slot server. Doing it here would be a Sony
	// call from the client, and a failure on this network would hide a server
	// that can still reach PlayStation.
	bool pscloud_remote = strcmp(cfg->service_type, "pscloud") == 0;
	if(!pscloud_remote)
	{
		ChiakiErrorCode auth_err = cc_authorize_check(log, cfg, duid);
		if(auth_err != CHIAKI_ERR_SUCCESS)
		{
			out->error_message = strdup(auth_err == CHIAKI_ERR_NETWORK
				? "Network error while contacting PlayStation. Check your connection and try again."
				: "AUTHORIZATION_FAILED");
			out->err = auth_err == CHIAKI_ERR_NETWORK ? CHIAKI_ERR_NETWORK : CHIAKI_ERR_UNKNOWN;
			return out->err;
		}
	}

	// Poll between the pre-flight and the real flow (header contract: "polled
	// between steps") so a cancel during the auth check stops before Kamaji runs.
	if(cp_cancelled(cfg))
	{
		out->err = CHIAKI_ERR_CANCELED;
		return out->err;
	}

	ChiakiErrorCode e = provision_once(log, cfg, duid, out);

	// One-shot fallback: an owned fast-path entitlement that Gaikai rejects
	// (noGameForEntitlementId) -> re-run the full Kamaji resolve/acquire once.
	bool used_fast_path = cfg->owned_entitlement_id && *cfg->owned_entitlement_id;
	if(e != CHIAKI_ERR_SUCCESS && used_fast_path && out->error_message &&
	   strstr(out->error_message, "noGameForEntitlement") && !cp_cancelled(cfg))
	{
		CHIAKI_LOGW(log, "[CLOUDSESSION] owned entitlement rejected by Gaikai; retrying full resolve flow");
		chiaki_cloud_provision_result_fini(out);
		result_init(out);
		ChiakiCloudProvisionConfig cfg2 = *cfg;
		cfg2.owned_entitlement_id = "";
		cfg2.owned_platform = "";
		e = provision_once(log, &cfg2, duid, out);
	}

	out->err = e;
	return e;
}
