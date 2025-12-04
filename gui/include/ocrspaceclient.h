// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_OCRSPACECLIENT_H
#define CHIAKI_OCRSPACECLIENT_H

#include <QObject>
#include <QString>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class OCRSpaceClient : public QObject
{
	Q_OBJECT

	private:
		QString api_key;
		QNetworkAccessManager *network_manager;

	public:
		explicit OCRSpaceClient(const QString &api_key, QObject *parent = nullptr);
		~OCRSpaceClient();

		void recognizeText(const QImage &image, const QString &language = "eng");
		void setApiKey(const QString &key);
		bool hasApiKey() const { return !api_key.isEmpty(); }

	signals:
		void textRecognized(const QString &text);
		void ocrError(const QString &error_message);

	private slots:
		void handleNetworkReply(QNetworkReply *reply);
};

#endif // CHIAKI_OCRSPACECLIENT_H

