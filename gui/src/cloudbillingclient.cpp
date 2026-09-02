// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloudbillingclient.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QNetworkDatagram>
#include <QAbstractSocket>
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

CloudBillingClient::Result CloudBillingClient::confirmStream(const QString &host, quint16 port,
	const QString &email, const QString &session_token)
{
	QJsonObject o = baseReq(host, port, QStringLiteral("confirm_stream"));
	o[QStringLiteral("email")] = email;
	o[QStringLiteral("session_token")] = session_token;
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

static QString chihiroImageUrl(const QString &product_id)
{
	const QString pid = product_id.trimmed();
	if(pid.isEmpty())
		return {};
	QString country = QStringLiteral("US");
	const QString prefix = pid.left(2).toUpper();
	if(prefix == QStringLiteral("EP") || prefix == QStringLiteral("EE") || prefix == QStringLiteral("EC"))
		country = QStringLiteral("GB");
	return QStringLiteral("https://store.playstation.com/store/api/chihiro/00_09_000/container/%1/en/999/%2/image")
		.arg(country, pid);
}

CloudBillingClient::Result CloudBillingClient::fetchCatalog(const QString &host, quint16 port,
	const QString &service_type, const QString &platform, bool only_billable)
{
	// Server replies with many MTU-safe qCompress chunks for one catalog request.
	Result result;
	QUdpSocket socket;
	socket.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 8 * 1024 * 1024);

	const QHostAddress addr(host);
	if(host.trimmed().isEmpty() || addr.isNull()) {
		result.error = QStringLiteral("Не задан адрес сервера биллинга (cloud_billing_host)");
		return result;
	}

	QJsonObject o = baseReq(host, port, QStringLiteral("catalog"));
	o.remove(QStringLiteral("_host"));
	o.remove(QStringLiteral("_port"));
	if(!service_type.trimmed().isEmpty())
		o[QStringLiteral("service_type")] = service_type.trimmed().toLower();
	if(!platform.trimmed().isEmpty())
		o[QStringLiteral("platform")] = platform.trimmed().toLower();
	if(only_billable)
		o[QStringLiteral("only_billable")] = true;
	o[QStringLiteral("transfer")] = QStringLiteral("qcompress_chunks");

	const QString req_id = o.value(QStringLiteral("id")).toString();
	const QByteArray send_data = QJsonDocument(o).toJson(QJsonDocument::Compact);
	if(socket.writeDatagram(send_data, addr, port) < 0) {
		result.error = QStringLiteral("Не удалось отправить запрос каталога: %1").arg(socket.errorString());
		return result;
	}

	QMap<int, QByteArray> parts;
	int expected_parts = -1;
	int total_games = -1;
	QString error;

	QEventLoop loop;
	QTimer idle;
	idle.setSingleShot(true);
	QTimer hard;
	hard.setSingleShot(true);
	QObject::connect(&idle, &QTimer::timeout, &loop, &QEventLoop::quit);
	QObject::connect(&hard, &QTimer::timeout, &loop, &QEventLoop::quit);
	QObject::connect(&socket, &QUdpSocket::readyRead, &loop, [&]() {
		while(socket.hasPendingDatagrams()) {
			const QNetworkDatagram dg = socket.receiveDatagram();
			const QJsonDocument doc = QJsonDocument::fromJson(dg.data());
			if(!doc.isObject())
				continue;
			const QJsonObject obj = doc.object();
			if(obj.value(QStringLiteral("id")).toString() != req_id)
				continue;

			if(!obj.value(QStringLiteral("ok")).toBool(true)) {
				error = obj.value(QStringLiteral("ui_message")).toString();
				if(error.isEmpty())
					error = obj.value(QStringLiteral("error")).toString();
				loop.quit();
				return;
			}

			// Legacy single-page / paged JSON (older server) — accept and stop.
			if(obj.contains(QStringLiteral("games")) && !obj.contains(QStringLiteral("transfer"))) {
				result.ok = true;
				result.data = obj;
				loop.quit();
				return;
			}

			if(obj.value(QStringLiteral("transfer")).toString() != QStringLiteral("qcompress_chunks"))
				continue;

			const int part = obj.value(QStringLiteral("part")).toInt(-1);
			expected_parts = obj.value(QStringLiteral("parts")).toInt(expected_parts);
			total_games = obj.value(QStringLiteral("totalGames")).toInt(total_games);
			const QByteArray piece = QByteArray::fromBase64(obj.value(QStringLiteral("data")).toString().toLatin1());
			if(part < 0 || piece.isEmpty())
				continue;
			parts.insert(part, piece);
			idle.start(8000); // reset idle watchdog while chunks arrive
			if(expected_parts > 0 && parts.size() >= expected_parts) {
				loop.quit();
				return;
			}
		}
	});

	idle.start(15000);
	hard.start(120000);
	loop.exec();

	if(!result.data.isEmpty() && result.data.contains(QStringLiteral("games"))) {
		// Legacy path already filled result.data
	} else if(!error.isEmpty()) {
		result.error = error;
		return result;
	} else if(expected_parts <= 0 || parts.size() < expected_parts) {
		result.error = QStringLiteral(
			"Таймаут каталога биллинга (%1:%2): получено %3/%4 чанков")
			.arg(host).arg(port).arg(parts.size()).arg(qMax(0, expected_parts));
		return result;
	} else {
		QByteArray blob;
		blob.reserve(parts.size() * 700);
		for(int i = 0; i < expected_parts; ++i) {
			if(!parts.contains(i)) {
				result.error = QStringLiteral("Каталог биллинга: потерян чанк %1/%2").arg(i).arg(expected_parts);
				return result;
			}
			blob.append(parts.value(i));
		}
		const QByteArray json = qUncompress(blob);
		if(json.isEmpty()) {
			result.error = QStringLiteral("Не удалось распаковать каталог биллинга");
			return result;
		}
		const QJsonDocument doc = QJsonDocument::fromJson(json);
		if(!doc.isObject()) {
			result.error = QStringLiteral("Некорректный JSON каталога биллинга");
			return result;
		}
		result.data = doc.object();
		result.ok = true;
	}

	QJsonArray games = result.data.value(QStringLiteral("games")).toArray();
	for(int i = 0; i < games.size(); ++i) {
		QJsonObject g = games.at(i).toObject();
		if(g.value(QStringLiteral("imageUrl")).toString().isEmpty()) {
			const QString pid = g.value(QStringLiteral("productId")).toString();
			const QString url = chihiroImageUrl(pid.isEmpty() ? g.value(QStringLiteral("streamIdentifier")).toString() : pid);
			if(!url.isEmpty())
				g.insert(QStringLiteral("imageUrl"), url);
		}
		if(g.value(QStringLiteral("streamServiceType")).toString().isEmpty())
			g.insert(QStringLiteral("streamServiceType"), g.value(QStringLiteral("serviceType")).toString());
		games.replace(i, g);
	}
	result.data.insert(QStringLiteral("games"), games);
	if(!result.data.contains(QStringLiteral("totalGames")))
		result.data.insert(QStringLiteral("totalGames"), games.size());
	else if(total_games > 0)
		result.data.insert(QStringLiteral("totalGames"), total_games);
	result.ok = true;
	return result;
}
