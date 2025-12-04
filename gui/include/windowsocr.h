// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_WINDOWSOCR_H
#define CHIAKI_WINDOWSOCR_H

#include <QObject>
#include <QString>
#include <QImage>

#ifdef _WIN32
#include <windows.h>
#endif

class WindowsOCR : public QObject
{
	Q_OBJECT

	public:
		explicit WindowsOCR(QObject *parent = nullptr);
		~WindowsOCR();

		void recognizeText(const QImage &image, const QString &language = "en");
		static bool isAvailable();

	signals:
		void textRecognized(const QString &text);
		void ocrError(const QString &error_message);

	private:
		void processImage(const QImage &image, const QString &language);
};

#endif // CHIAKI_WINDOWSOCR_H

