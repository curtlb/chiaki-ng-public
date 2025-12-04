// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_DEEPLTRANSLATOR_H
#define CHIAKI_DEEPLTRANSLATOR_H

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class DeepLTranslator : public QObject
{
	Q_OBJECT

	private:
		QString api_key;
		QNetworkAccessManager *network_manager;
		bool is_free_api;

	public:
		explicit DeepLTranslator(const QString &api_key, bool free_api = true, QObject *parent = nullptr);
		~DeepLTranslator();

		void translate(const QString &text, const QString &source_lang = "EN", const QString &target_lang = "RU");
		void setApiKey(const QString &key);
		bool hasApiKey() const { return !api_key.isEmpty(); }

	signals:
		void translationReady(const QString &translated_text);
		void translationError(const QString &error_message);

	private slots:
		void handleNetworkReply(QNetworkReply *reply);
};

#endif // CHIAKI_DEEPLTRANSLATOR_H

