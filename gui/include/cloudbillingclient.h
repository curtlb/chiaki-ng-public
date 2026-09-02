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
		const QString &service_type, const QString &game_identifier, const QString &game_name);
	static Result start(const QString &host, quint16 port, const QString &email,
		const QString &service_type, const QString &game_identifier, const QString &game_name);
	static Result confirmStream(const QString &host, quint16 port, const QString &email,
		const QString &session_token);
	static Result heartbeat(const QString &host, quint16 port, const QString &session_token, bool streaming);
	static Result renew(const QString &host, quint16 port, const QString &email, const QString &session_token);
	static Result endStream(const QString &host, quint16 port, const QString &session_token);
};

#endif
