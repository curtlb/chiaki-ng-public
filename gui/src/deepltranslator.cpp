// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "deepltranslator.h"
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>

DeepLTranslator::DeepLTranslator(const QString &api_key, bool free_api, QObject *parent)
	: QObject(parent), api_key(api_key), is_free_api(free_api)
{
	network_manager = new QNetworkAccessManager(this);
	connect(network_manager, &QNetworkAccessManager::finished, this, &DeepLTranslator::handleNetworkReply);
}

DeepLTranslator::~DeepLTranslator()
{
	delete network_manager;
}

void DeepLTranslator::setApiKey(const QString &key)
{
	api_key = key;
}

void DeepLTranslator::translate(const QString &text, const QString &source_lang, const QString &target_lang)
{
	if(api_key.isEmpty())
	{
		emit translationError("DeepL API ключ не установлен. Установите его в Настройках → Конфигурация");
		return;
	}

	if(text.trimmed().isEmpty())
	{
		emit translationReady("");
		return;
	}

	// DeepL API endpoint
	QString endpoint = is_free_api 
		? "https://api-free.deepl.com/v2/translate"
		: "https://api.deepl.com/v2/translate";

	QUrl url(endpoint);
	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
	request.setRawHeader("Authorization", QString("DeepL-Auth-Key %1").arg(api_key).toUtf8());

	// Build POST data
	QUrlQuery params;
	params.addQueryItem("text", text);
	params.addQueryItem("target_lang", target_lang);
	if(source_lang != "AUTO")
		params.addQueryItem("source_lang", source_lang);

	QByteArray post_data = params.toString(QUrl::FullyEncoded).toUtf8();

	network_manager->post(request, post_data);
}

void DeepLTranslator::handleNetworkReply(QNetworkReply *reply)
{
	reply->deleteLater();

	if(reply->error() != QNetworkReply::NoError)
	{
		emit translationError(QString("Network error: %1").arg(reply->errorString()));
		return;
	}

	QByteArray response = reply->readAll();
	QJsonDocument doc = QJsonDocument::fromJson(response);

	if(!doc.isObject())
	{
		emit translationError("Invalid JSON response");
		return;
	}

	QJsonObject obj = doc.object();
	if(!obj.contains("translations"))
	{
		emit translationError("No translations in response");
		return;
	}

	QJsonArray translations = obj["translations"].toArray();
	if(translations.isEmpty())
	{
		emit translationError("Empty translations array");
		return;
	}

	QJsonObject first_translation = translations[0].toObject();
	QString translated_text = first_translation["text"].toString();

	emit translationReady(translated_text);
}

