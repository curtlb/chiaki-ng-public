// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloudbillingclient.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QTcpSocket>
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

CloudBillingClient::Result CloudBillingClient::whoami(const QString &host, quint16 port, const QString &email)
{
	QJsonObject o = baseReq(host, port, QStringLiteral("whoami"));
	o[QStringLiteral("email")] = email;
	return request(o);
}

static QString normalizeTitleSku(QString sku)
{
	sku = sku.trimmed();
	if(sku.isEmpty())
		return {};
	const QString up = sku.toUpper();
	const QStringList prefixes = {
		QStringLiteral("CUSA"), QStringLiteral("PPSA"), QStringLiteral("NPEA"),
		QStringLiteral("NPEB"), QStringLiteral("NPUB"), QStringLiteral("NPUA"),
		QStringLiteral("NPUG")
	};
	for(const QString &pref : prefixes) {
		if(!up.startsWith(pref))
			continue;
		const QString rest = up.mid(pref.size());
		if(rest.contains(QLatin1Char('_')))
			return up;
		bool ok = false;
		rest.toULongLong(&ok);
		if(ok && !rest.isEmpty())
			return pref + rest + QStringLiteral("_00");
		return up;
	}
	return sku;
}

static QString extractTitleSku(const QString &product_id)
{
	const QString pid = product_id.trimmed();
	if(pid.isEmpty())
		return {};
	const int dash = pid.indexOf(QLatin1Char('-'));
	if(dash >= 0 && dash + 1 < pid.size()) {
		const QString rest = pid.mid(dash + 1);
		const int nextDash = rest.indexOf(QLatin1Char('-'));
		return normalizeTitleSku(nextDash > 0 ? rest.left(nextDash) : rest);
	}
	return normalizeTitleSku(pid);
}

static QString chihiroImageUrl(const QString &product_id, const QString &locale = QStringLiteral("en-GB"))
{
	const QString pid = product_id.trimmed();
	if(pid.isEmpty())
		return {};
	Q_UNUSED(locale);
	QString country = QStringLiteral("GB");
	const QString prefix = pid.left(2).toUpper();
	if(prefix == QStringLiteral("UP") || prefix == QStringLiteral("HP") || prefix == QStringLiteral("HN"))
		country = QStringLiteral("US");
	const QString sku = extractTitleSku(pid);
	if(sku.isEmpty())
		return {};
	return QStringLiteral("https://store.playstation.com/store/api/chihiro/00_09_000/titlecontainer/%1/en/999/%2/image?w=440&h=440")
		.arg(country, sku);
}

static bool readTcpLine(QTcpSocket &tcp, QByteArray *out_line, int timeout_ms)
{
	out_line->clear();
	while(!out_line->contains('\n')) {
		if(tcp.bytesAvailable() <= 0) {
			if(!tcp.waitForReadyRead(timeout_ms))
				return false;
		}
		out_line->append(tcp.readLine(1024 * 1024));
		if(out_line->isEmpty() && tcp.state() != QAbstractSocket::ConnectedState)
			return false;
	}
	return true;
}

static bool readTcpExact(QTcpSocket &tcp, qint64 nbytes, QByteArray *out, int timeout_ms)
{
	out->clear();
	out->reserve(static_cast<int>(nbytes));
	while(out->size() < nbytes) {
		if(tcp.bytesAvailable() <= 0) {
			if(!tcp.waitForReadyRead(timeout_ms))
				return false;
		}
		const QByteArray chunk = tcp.read(nbytes - out->size());
		if(chunk.isEmpty() && tcp.state() != QAbstractSocket::ConnectedState)
			return false;
		out->append(chunk);
	}
	return out->size() == nbytes;
}

CloudBillingClient::Result CloudBillingClient::fetchCatalog(const QString &host, quint16 port,
	const QString &service_type, const QString &platform, bool only_billable)
{
	// Catalog is ~hundreds of KB — UDP chunk floods are dropped by NAT/firewall (0/0).
	// Use TCP on billing_port+1 (default 13751).
	Result result;
	const quint16 tcp_port = static_cast<quint16>(port + 1);
	if(host.trimmed().isEmpty()) {
		result.error = QStringLiteral("Не задан адрес сервера биллинга (cloud_billing_host)");
		return result;
	}

	QJsonObject o;
	o[QStringLiteral("id")] = QUuid::createUuid().toString(QUuid::WithoutBraces);
	o[QStringLiteral("action")] = QStringLiteral("catalog");
	if(!service_type.trimmed().isEmpty())
		o[QStringLiteral("service_type")] = service_type.trimmed().toLower();
	if(!platform.trimmed().isEmpty())
		o[QStringLiteral("platform")] = platform.trimmed().toLower();
	if(only_billable)
		o[QStringLiteral("only_billable")] = true;

	QTcpSocket tcp;
	tcp.connectToHost(host, tcp_port);
	if(!tcp.waitForConnected(10000)) {
		result.error = QStringLiteral("Не удалось подключиться к каталогу биллинга %1:%2 — %3")
			.arg(host).arg(tcp_port).arg(tcp.errorString());
		return result;
	}

	const QByteArray req = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
	if(tcp.write(req) != req.size() || !tcp.waitForBytesWritten(10000)) {
		result.error = QStringLiteral("Не удалось отправить запрос каталога по TCP");
		return result;
	}

	QByteArray header_line;
	if(!readTcpLine(tcp, &header_line, 60000)) {
		result.error = QStringLiteral("Таймаут заголовка каталога биллинга (%1:%2)").arg(host).arg(tcp_port);
		return result;
	}

	const QJsonDocument header_doc = QJsonDocument::fromJson(header_line.trimmed());
	if(!header_doc.isObject()) {
		result.error = QStringLiteral("Некорректный заголовок каталога биллинга");
		return result;
	}
	const QJsonObject header = header_doc.object();
	if(!header.value(QStringLiteral("ok")).toBool(false)) {
		result.error = header.value(QStringLiteral("ui_message")).toString();
		if(result.error.isEmpty())
			result.error = header.value(QStringLiteral("error")).toString();
		if(result.error.isEmpty())
			result.error = QStringLiteral("Ошибка каталога биллинга");
		return result;
	}

	// Legacy: whole JSON in one TCP line (no binary blob).
	if(header.contains(QStringLiteral("games")) && !header.contains(QStringLiteral("nbytes"))) {
		result.data = header;
		result.ok = true;
	} else {
		const qint64 nbytes = static_cast<qint64>(header.value(QStringLiteral("nbytes")).toDouble(-1));
		if(nbytes <= 0 || nbytes > 50 * 1024 * 1024) {
			result.error = QStringLiteral("Некорректный размер каталога биллинга");
			return result;
		}
		QByteArray blob;
		if(!readTcpExact(tcp, nbytes, &blob, 60000)) {
			result.error = QStringLiteral("Таймаут загрузки каталога биллинга (%1/%2 байт)")
				.arg(blob.size()).arg(nbytes);
			return result;
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
		if(header.contains(QStringLiteral("totalGames")))
			result.data.insert(QStringLiteral("totalGames"), header.value(QStringLiteral("totalGames")));
		if(header.contains(QStringLiteral("catalog_filter_version")))
			result.data.insert(QStringLiteral("catalog_filter_version"), header.value(QStringLiteral("catalog_filter_version")));
	}

	QJsonArray games = result.data.value(QStringLiteral("games")).toArray();
	for(int i = 0; i < games.size(); ++i) {
		QJsonObject g = games.at(i).toObject();
		QString existing = g.value(QStringLiteral("imageUrl")).toString();
		if(existing.contains(QLatin1String("/chihiro/"))
		   && existing.contains(QLatin1String("/container/"))
		   && !existing.contains(QLatin1String("/titlecontainer/")))
			existing.clear();
		if(existing.isEmpty()) {
			const QString pid = g.value(QStringLiteral("productId")).toString();
			const QString url = chihiroImageUrl(pid.isEmpty() ? g.value(QStringLiteral("streamIdentifier")).toString() : pid);
			if(!url.isEmpty())
				g.insert(QStringLiteral("imageUrl"), url);
		} else {
			g.insert(QStringLiteral("imageUrl"), existing);
		}
		if(g.value(QStringLiteral("streamServiceType")).toString().isEmpty())
			g.insert(QStringLiteral("streamServiceType"), g.value(QStringLiteral("serviceType")).toString());
		games.replace(i, g);
	}
	result.data.insert(QStringLiteral("games"), games);
	if(!result.data.contains(QStringLiteral("totalGames")))
		result.data.insert(QStringLiteral("totalGames"), games.size());
	result.ok = true;
	return result;
}
