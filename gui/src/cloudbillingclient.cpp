// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloudbillingclient.h"

#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QUuid>

#ifndef CHIAKI_LIB_ENABLE_MBEDTLS
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#endif

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
	const QString expect_id = send_payload.value(QStringLiteral("id")).toString();

	const QByteArray send_data = QJsonDocument(send_payload).toJson(QJsonDocument::Compact);
	if(socket.writeDatagram(send_data, host, port) < 0) {
		result.error = QStringLiteral("Не удалось отправить запрос биллинга: %1").arg(socket.errorString());
		return result;
	}

	// Blocking wait — safe from worker std::thread (unlike QEventLoop + readyRead).
	const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + qMax(1, timeout_ms);
	while(QDateTime::currentMSecsSinceEpoch() < deadline) {
		const int left = static_cast<int>(deadline - QDateTime::currentMSecsSinceEpoch());
		if(left <= 0)
			break;
		if(!socket.waitForReadyRead(left))
			continue;
		while(socket.hasPendingDatagrams()) {
			const QNetworkDatagram dg = socket.receiveDatagram();
			const QJsonDocument doc = QJsonDocument::fromJson(dg.data());
			if(!doc.isObject())
				continue;
			const QJsonObject obj = doc.object();
			if(obj.value(QStringLiteral("id")).toString() != expect_id)
				continue;
			result.ok = obj.value(QStringLiteral("ok")).toBool();
			result.error = obj.value(QStringLiteral("error")).toString();
			result.ui_message = obj.value(QStringLiteral("ui_message")).toString();
			result.data = obj;
			return result;
		}
	}

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
	const QString &service_type, const QString &game_identifier, const QString &game_name,
	qint64 account_id)
{
	QJsonObject o = baseReq(host, port, QStringLiteral("quote"));
	o[QStringLiteral("email")] = email;
	o[QStringLiteral("service_type")] = service_type;
	o[QStringLiteral("game_identifier")] = game_identifier;
	o[QStringLiteral("game_name")] = game_name;
	if(account_id > 0)
		o[QStringLiteral("account_id")] = account_id;
	return request(o);
}

CloudBillingClient::Result CloudBillingClient::start(const QString &host, quint16 port, const QString &email,
	const QString &service_type, const QString &game_identifier, const QString &game_name,
	qint64 account_id)
{
	QJsonObject o = baseReq(host, port, QStringLiteral("start"));
	o[QStringLiteral("email")] = email;
	o[QStringLiteral("service_type")] = service_type;
	o[QStringLiteral("game_identifier")] = game_identifier;
	o[QStringLiteral("game_name")] = game_name;
	o[QStringLiteral("confirm")] = true;
	if(account_id > 0)
		o[QStringLiteral("account_id")] = account_id;
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

CloudBillingClient::Result CloudBillingClient::catalogNpsso(const QString &host, quint16 port, const QString &email)
{
	QJsonObject o = baseReq(host, port, QStringLiteral("catalog_npsso"));
	o[QStringLiteral("email")] = email;
	return request(o, 15000);
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


// Auth VM public key only (encrypt). Private key + password salt stay on the auth server.
static const char kCloudAuthPublicKeyPem[] =
	"-----BEGIN PUBLIC KEY-----\n"
	"MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEApj2jWV4Y3y47V3H/NKiD\n"
	"1BncACWNFnR9EB0Elg5yKdh+Z3PujHTWDa7zUtKqudJtJLn+oAfmCCqS5/a2pF9M\n"
	"mFTaZB77SfneRu8OfoTK8Jd9l8+XqOh4zpvZzxRJRS4zZGdZdmpWYXMmw1Jcmc0y\n"
	"hDAur2mYucIviRqrqprf8DDzNq+aPxDtSenHA7bfC6HqdDhnAXLKloLysowkvSC4\n"
	"rGgm7YJhtgCs7pD7BMD0deSgth2IsHU/Hz/vtniGrAVNwqNcXn+rmsR+J5HWrZh6\n"
	"RjGUi5G9TgmQDjd5KPqeiZ0hVwxFUuvVig3J6BWcUiTUeEXoRCUFNXOLME5h2fQC\n"
	"iQIDAQAB\n"
	"-----END PUBLIC KEY-----\n";

QString CloudBillingClient::encryptAuthCredentialsToken(const QString &email, const QString &password, QString *error_out)
{
#ifdef CHIAKI_LIB_ENABLE_MBEDTLS
	if(error_out)
		*error_out = QStringLiteral("Шифрование входа требует OpenSSL");
	return {};
#else
	QJsonObject payload;
	payload.insert(QStringLiteral("email"), email.trimmed().toLower());
	payload.insert(QStringLiteral("password"), password);
	payload.insert(QStringLiteral("ts"), QDateTime::currentSecsSinceEpoch());
	const QByteArray plain = QJsonDocument(payload).toJson(QJsonDocument::Compact);

	unsigned char aes_key[32];
	unsigned char iv[12];
	if(RAND_bytes(aes_key, sizeof(aes_key)) != 1 || RAND_bytes(iv, sizeof(iv)) != 1) {
		if(error_out)
			*error_out = QStringLiteral("Не удалось сгенерировать ключ шифрования");
		return {};
	}

	QByteArray ciphertext;
	ciphertext.resize(plain.size());
	unsigned char tag[16];
	int out_len = 0;
	int total_len = 0;
	EVP_CIPHER_CTX *cctx = EVP_CIPHER_CTX_new();
	if(!cctx) {
		if(error_out)
			*error_out = QStringLiteral("Ошибка OpenSSL (cipher ctx)");
		return {};
	}
	bool ok = EVP_EncryptInit_ex(cctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
		&& EVP_CIPHER_CTX_ctrl(cctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(iv), nullptr) == 1
		&& EVP_EncryptInit_ex(cctx, nullptr, nullptr, aes_key, iv) == 1
		&& EVP_EncryptUpdate(cctx, reinterpret_cast<unsigned char *>(ciphertext.data()), &out_len,
			reinterpret_cast<const unsigned char *>(plain.constData()), plain.size()) == 1;
	total_len = out_len;
	ok = ok && EVP_EncryptFinal_ex(cctx, reinterpret_cast<unsigned char *>(ciphertext.data()) + total_len, &out_len) == 1;
	total_len += out_len;
	ok = ok && EVP_CIPHER_CTX_ctrl(cctx, EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag) == 1;
	EVP_CIPHER_CTX_free(cctx);
	if(!ok) {
		if(error_out)
			*error_out = QStringLiteral("Ошибка AES-GCM шифрования");
		return {};
	}
	ciphertext.resize(total_len);

	BIO *bio = BIO_new_mem_buf(kCloudAuthPublicKeyPem, -1);
	if(!bio) {
		if(error_out)
			*error_out = QStringLiteral("Ошибка OpenSSL (BIO)");
		return {};
	}
	EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
	BIO_free(bio);
	if(!pkey) {
		if(error_out)
			*error_out = QStringLiteral("Не удалось загрузить публичный ключ авторизации");
		return {};
	}

	EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new(pkey, nullptr);
	size_t enc_len = 0;
	QByteArray enc_key;
	ok = pctx
		&& EVP_PKEY_encrypt_init(pctx) == 1
		&& EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_OAEP_PADDING) == 1
		&& EVP_PKEY_CTX_set_rsa_oaep_md(pctx, EVP_sha256()) == 1
		&& EVP_PKEY_encrypt(pctx, nullptr, &enc_len, aes_key, sizeof(aes_key)) == 1;
	if(ok) {
		enc_key.resize(static_cast<int>(enc_len));
		ok = EVP_PKEY_encrypt(pctx, reinterpret_cast<unsigned char *>(enc_key.data()), &enc_len, aes_key, sizeof(aes_key)) == 1;
		enc_key.resize(static_cast<int>(enc_len));
	}
	EVP_PKEY_CTX_free(pctx);
	EVP_PKEY_free(pkey);
	if(!ok) {
		if(error_out)
			*error_out = QStringLiteral("Ошибка RSA шифрования токена входа");
		return {};
	}

	QByteArray body;
	body.append(reinterpret_cast<const char *>(iv), sizeof(iv));
	body.append(reinterpret_cast<const char *>(tag), sizeof(tag));
	body.append(ciphertext);

	return QStringLiteral("CA1.")
		+ QString::fromLatin1(enc_key.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))
		+ QLatin1Char('.')
		+ QString::fromLatin1(body.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
#endif
}

CloudBillingClient::Result CloudBillingClient::authSignIn(const QString &host, quint16 port,
	const QString &email, const QString &password)
{
	QString enc_error;
	const QString token = encryptAuthCredentialsToken(email, password, &enc_error);
	if(token.isEmpty()) {
		Result r;
		r.error = enc_error.isEmpty() ? QStringLiteral("Не удалось зашифровать данные входа") : enc_error;
		return r;
	}
	QJsonObject o = baseReq(host, port, QStringLiteral("sign_in"));
	o[QStringLiteral("token")] = token;
	return request(o, 15000);
}

CloudBillingClient::Result CloudBillingClient::authCheckSession(const QString &host, quint16 port, const QString &jwt)
{
	QJsonObject o = baseReq(host, port, QStringLiteral("check_session"));
	o[QStringLiteral("jwt")] = jwt;
	return request(o, 15000);
}
