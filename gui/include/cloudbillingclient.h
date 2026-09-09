// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CLOUDBILLINGCLIENT_H
#define CLOUDBILLINGCLIENT_H

#include <QJsonObject>
#include <QString>

/**
 * Lightweight JSON-over-UDP client for the cloud hourly billing service.
 * Server host/port configured in Settings (cloud_billing_host / cloud_billing_port).
 */
class CloudBillingClient
{
public:
	struct Result {
		bool ok = false;
		QString error;
		QString ui_message;
		QJsonObject data;
	};

	static Result request(const QJsonObject &payload, int timeout_ms = 8000);

	static Result ping(const QString &host, quint16 port);
	static Result quote(const QString &host, quint16 port, const QString &email,
		const QString &service_type, const QString &game_identifier, const QString &game_name,
		qint64 account_id = 0);
	static Result start(const QString &host, quint16 port, const QString &email,
		const QString &service_type, const QString &game_identifier, const QString &game_name,
		qint64 account_id = 0);
	static Result confirmStream(const QString &host, quint16 port, const QString &email,
		const QString &session_token);
	static Result heartbeat(const QString &host, quint16 port, const QString &session_token, bool streaming);
	static Result renew(const QString &host, quint16 port, const QString &email, const QString &session_token);
	static Result endStream(const QString &host, quint16 port, const QString &session_token);
	static Result whoami(const QString &host, quint16 port, const QString &email);
	/** NPSSO from the player's first assigned CloudStreaming_Accounts (soft-assigns if needed). */
	static Result catalogNpsso(const QString &host, quint16 port, const QString &email);
	/** Full game catalog from CloudStreaming_Catalog (no player NPSSO).
	 *  Fetched over TCP on billing_port+1 (default 13751) as a qCompress blob. */
	static Result fetchCatalog(const QString &host, quint16 port,
		const QString &service_type = QString(), const QString &platform = QString(),
		bool only_billable = false);

	/** Auth UDP (default port 13752). Credentials go as RSA+AES encrypted token only. */
	static Result authSignIn(const QString &host, quint16 port, const QString &email, const QString &password);
	static Result authCheckSession(const QString &host, quint16 port, const QString &jwt);

private:
	/** Opaque login token: CA1.<b64 rsa-oaep aes-key>.<b64 iv+ct+tag>. No salt in client. */
	static QString encryptAuthCredentialsToken(const QString &email, const QString &password, QString *error_out);
};

#endif
