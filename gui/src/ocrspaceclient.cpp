// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "ocrspaceclient.h"
#include <QNetworkRequest>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>

OCRSpaceClient::OCRSpaceClient(const QString &api_key, QObject *parent)
	: QObject(parent), api_key(api_key)
{
	network_manager = new QNetworkAccessManager(this);
	connect(network_manager, &QNetworkAccessManager::finished, this, &OCRSpaceClient::handleNetworkReply);
}

OCRSpaceClient::~OCRSpaceClient()
{
	delete network_manager;
}

void OCRSpaceClient::setApiKey(const QString &key)
{
	api_key = key;
}

void OCRSpaceClient::recognizeText(const QImage &image, const QString &language)
{
	if(api_key.isEmpty())
	{
		emit ocrError("OCR.space API ключ не установлен. Получите бесплатный ключ на https://ocr.space/ocrapi");
		return;
	}

	if(image.isNull())
	{
		emit ocrError("Недопустимое изображение");
		return;
	}

	// OCR.space API endpoint
	QUrl url("https://api.ocr.space/parse/image");
	QNetworkRequest request(url);

	// Create multipart form data
	QHttpMultiPart *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

	// API Key
	QHttpPart apiKeyPart;
	apiKeyPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"apikey\""));
	apiKeyPart.setBody(api_key.toUtf8());
	multiPart->append(apiKeyPart);

	// Language
	QHttpPart languagePart;
	languagePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"language\""));
	languagePart.setBody(language.toUtf8());
	multiPart->append(languagePart);

	// OCR Engine (2 is best for game text)
	QHttpPart enginePart;
	enginePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"OCREngine\""));
	enginePart.setBody("2");
	multiPart->append(enginePart);

	// Scale image for better OCR
	QHttpPart scalePart;
	scalePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"scale\""));
	scalePart.setBody("true");
	multiPart->append(scalePart);

	// Detect orientation
	QHttpPart orientationPart;
	orientationPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"detectOrientation\""));
	orientationPart.setBody("true");
	multiPart->append(orientationPart);

	// Convert image to JPEG with compression
	QByteArray imageData;
	QBuffer buffer(&imageData);
	buffer.open(QIODevice::WriteOnly);
	
	// Resize if too large (max 1024px width for speed)
	QImage processedImage = image;
	if(image.width() > 1024)
		processedImage = image.scaledToWidth(1024, Qt::SmoothTransformation);
	
	// Save as JPEG with 85% quality for good compression
	processedImage.save(&buffer, "JPG", 85);

	// Image data
	QHttpPart imagePart;
	imagePart.setHeader(QNetworkRequest::ContentDispositionHeader, 
		QVariant("form-data; name=\"file\"; filename=\"screenshot.jpg\""));
	imagePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("image/jpeg"));
	imagePart.setBody(imageData);
	multiPart->append(imagePart);

	// Send request
	QNetworkReply *reply = network_manager->post(request, multiPart);
	multiPart->setParent(reply); // Delete multipart with reply
}

void OCRSpaceClient::handleNetworkReply(QNetworkReply *reply)
{
	reply->deleteLater();

	if(reply->error() != QNetworkReply::NoError)
	{
		emit ocrError(QString("Ошибка сети: %1").arg(reply->errorString()));
		return;
	}

	QByteArray response = reply->readAll();
	QJsonDocument doc = QJsonDocument::fromJson(response);

	if(!doc.isObject())
	{
		emit ocrError("Некорректный JSON ответ");
		return;
	}

	QJsonObject obj = doc.object();
	
	// Check for API errors
	if(obj.contains("IsErroredOnProcessing") && obj["IsErroredOnProcessing"].toBool())
	{
		QString errorMsg = obj.contains("ErrorMessage") 
			? obj["ErrorMessage"].toArray()[0].toString()
			: "Неизвестная ошибка OCR";
		emit ocrError(QString("OCR ошибка: %1").arg(errorMsg));
		return;
	}

	// Extract recognized text
	if(!obj.contains("ParsedResults"))
	{
		emit ocrError("Нет результатов распознавания");
		return;
	}

	QJsonArray results = obj["ParsedResults"].toArray();
	if(results.isEmpty())
	{
		emit ocrError("Текст не распознан");
		return;
	}

	QString recognizedText = results[0].toObject()["ParsedText"].toString();

	if(recognizedText.trimmed().isEmpty())
	{
		emit ocrError("Текст не найден на изображении");
		return;
	}

	emit textRecognized(recognizedText.trimmed());
}

