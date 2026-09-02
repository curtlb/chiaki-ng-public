// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloudbillingclient.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QTimer>
#include <QUuid>

CloudBillingClient::Result CloudBillingClient::request(const QJsonObject &payload, int timeout_ms)
{
	Result result;
	QUdpSocket socket;

	QHostAddress host;
	quint16 port = 13750;
	if(payload.contains(QStringLiteral("_host"))) {
		const QString host_str = payload.value(QStringLiteral("_host")).toString().trimmed();
		if(host_str.isEmpty()) {
			result.error = QStringLiteral("Не задан адрес сервера биллинга (cloud_billing_host)");
			return result;
		}
		host = QHostAddress(host_str);
	} else {
		result.error = QStringLiteral("Не задан адрес сервера биллинга");
		return result;
	}
	if(payload.contains(QStringLiteral("_port")))
		port = static_cast<quint16>(payload.value(QStringLiteral("_port")).toInt());

	QJsonObject send_payload = payload;
	send_payload.remove(QStringLiteral("_host"));
	send_payload.remove(QStringLiteral("_port"));

	const QByteArray send_data = QJsonDocument(send_payload).toJson(QJsonDocument::Compact);
	if(socket.writeDatagram(send_data, host, port) < 0) {
		result.error = QStringLiteral("Не удалось отправить запрос биллинга: %1").arg(socket.errorString());
		return result;
	}

	QEventLoop loop;
	QTimer timer;
	timer.setSingleShot(true);
	QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
	QObject::connect(&socket, &QUdpSocket::readyRead, &loop, [&]() {
		while(socket.hasPendingDatagrams()) {
			const QNetworkDatagram dg = socket.receiveDatagram();
			const QJsonDocument doc = QJsonDocument::fromJson(dg.data());
			if(!doc.isObject())
				continue;
			const QJsonObject obj = doc.object();
			if(obj.value(QStringLiteral("id")).toString() != payload.value(QStringLiteral("id")).toString())
				continue;
			result.ok = obj.value(QStringLiteral("ok")).toBool();
			result.error = obj.value(QStringLiteral("error")).toString();
			result.ui_message = obj.value(QStringLiteral("ui_message")).toString();
			result.data = obj;
			loop.quit();
		}
	});
	timer.start(timeout_ms);
	loop.exec();

	if(result.data.isEmpty() && result.error.isEmpty())
		result.error = QStringLiteral("Таймаут ответа сервера биллинга (%1:%2)").arg(host.toString()).arg(port);
	return result;
}

static QJsonObject baseReq(const QString &host, quint16 port, const QString &action)
{
	QJsonObject o;
	o[QStringLiteral("id")] = QUuid::createUuid().toString(QUuid::WithoutBraces);
	o[QStringLiteral("action")] = action;
	o[QStringLiteral("_host")] = host;
	o[QStringLiteral("_port")] = static_cast<int>(port);
	return o;
}

CloudBillingClient::Result CloudBillingClient::ping(const QString &host, quint16 port)
{
	return request(baseReq(host, port, QStringLiteral("ping")));
}

CloudBillingClient::Result CloudBillingClient::quote(const QString &host, quint16 port, const QString &email,
	const QString &service_type, const QString &game_identifier, const QString &game_name)
{
	QJsonObject o = baseReq(host, port, QStringLiteral("quote"));
	o[QStringLiteral("email")] = email;
	o[QStringLiteral("service_type")] = service_type;
	o[QStringLiteral("game_identifier")] = game_identifier;
	o[QStringLiteral("game_name")] = game_name;
	return request(o);
}

CloudBillingClient::Result CloudBillingClient::start(const QString &host, quint16 port, const QString &email,
	const QString &service_type, const QString &game_identifier, const QString &game_name)
{
	QJsonObject o = baseReq(host, port, QStringLiteral("start"));
	o[QStringLiteral("email")] = email;
	o[QStringLiteral("service_type")] = service_type;
	o[QStringLiteral("game_identifier")] = game_identifier;
	o[QStringLiteral("game_name")] = game_name;
	o[QStringLiteral("confirm")] = true;
	return request(o, 120000);
}

CloudBillingClient::Result CloudBillingClient::heartbeat(const QString &host, quint16 port, const QString &session_token, bool streaming)
{
	QJsonObject o = baseReq(host, port, QStringLiteral("heartbeat"));
	o[QStringLiteral("session_token")] = session_token;
	o[QStringLiteral("streaming")] = streaming;
	return request(o);
}

CloudBillingClient::Result CloudBillingClient::renew(const QString &host, quint16 port, const QString &email, const QString &session_token)
{
	QJsonObject o = baseReq(host, port, QStringLiteral("renew"));
	o[QStringLiteral("email")] = email;
	o[QStringLiteral("session_token")] = session_token;
	return request(o, 120000);
}

CloudBillingClient::Result CloudBillingClient::endStream(const QString &host, quint16 port, const QString &session_token)
{
	QJsonObject o = baseReq(host, port, QStringLiteral("end_stream"));
	o[QStringLiteral("session_token")] = session_token;
	return request(o);
}
