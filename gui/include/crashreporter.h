// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CRASHREPORTER_H
#define CHIAKI_CRASHREPORTER_H

#include <QObject>
#include <QString>
#include <QJsonObject>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

class QUdpSocket;

class CrashReporter : public QObject
{
	Q_OBJECT

public:
	explicit CrashReporter(QObject *parent = nullptr);
	~CrashReporter();

	// Инициализация обработчиков крашей
	static void Initialize();

	// Отправка отчета об ошибке
	static void SendReport(const QString &errorType, const QString &message, const QString &stackTrace = QString());

	// Отправка отчета об исключении
	static void SendExceptionReport(const QString &exceptionType, const QString &what);

private:
	static CrashReporter *instance;
	QUdpSocket *udpSocket;
	QString serverHost;
	quint16 serverPort;

	// Сбор системной информации
	static QJsonObject CollectSystemInfo();
	static QString GetOSInfo();
	static QString GetAppVersion();
	
	// Создание JSON отчета
	static QByteArray CreateReport(const QString &errorType, const QString &message, const QString &stackTrace = QString());

	// Отправка данных
	void sendData(const QByteArray &data);

	// Обработчики сигналов (для крашей)
#ifdef Q_OS_WIN
	static LONG WINAPI ExceptionHandler(EXCEPTION_POINTERS *exceptionInfo);
#else
	static void SignalHandler(int signal);
#endif

	// Обработчик std::terminate
	static void TerminateHandler();
};

#endif // CHIAKI_CRASHREPORTER_H

