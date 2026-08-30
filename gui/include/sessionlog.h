// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_SESSIONLOG_H
#define CHIAKI_SESSIONLOG_H

#include <chiaki/log.h>

#include <QString>
#include <QDir>
#include <QMutex>

class QFile;
class StreamSession;

class SessionLog
{
	friend class SessionLogPrivate;

	private:
		StreamSession *session;
		ChiakiLog log;
		QFile *file;
		QMutex file_mutex;

		void Log(ChiakiLogLevel level, const char *msg);

	public:
		SessionLog(StreamSession *session, uint32_t level_mask, const QString &filename);
		~SessionLog();

		ChiakiLog *GetChiakiLog()	{ return &log; }
};

QString GetLogBaseDir();
QString CreateLogFilename();
QString CreateCloudLogFilename();

// File-backed Chiaki log for operations that run outside StreamSession (cloud catalog, provisioning).
class ChiakiFileLog
{
	friend class ChiakiFileLogPrivate;

	private:
		ChiakiLog log;
		QFile *file;
		QMutex file_mutex;
		QString process_name;

		void Log(ChiakiLogLevel level, const char *msg);

	public:
		ChiakiFileLog(uint32_t level_mask, const QString &filename, const QString &process = QStringLiteral("System"));
		~ChiakiFileLog();

		ChiakiLog *GetChiakiLog()	{ return &log; }
		QString Filename() const;
};

#endif //CHIAKI_SESSIONLOG_H
