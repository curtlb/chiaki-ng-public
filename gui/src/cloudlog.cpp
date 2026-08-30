// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <cloudlog.h>

#include <chiaki/log.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>

static QMutex cloud_log_mutex;

QString CloudLogFilePath()
{
	static QString path;
	if(path.isEmpty())
	{
		const QString dir = QCoreApplication::applicationDirPath();
		path = QDir(dir).absoluteFilePath(QStringLiteral("chiaki_cloud.log"));
	}
	return path;
}

void CloudLogWrite(const char *tag, ChiakiLogLevel level, const char *message)
{
	if(!message)
		return;

	static const QString date_format = QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz");
	const QString line = QStringLiteral("[%1] [%2] [%3] %4\n")
		.arg(QDateTime::currentDateTime().toString(date_format),
		     QString::fromUtf8(tag ? tag : "Cloud"),
		     QString(chiaki_log_level_char(level)),
		     QString::fromUtf8(message));

	QMutexLocker lock(&cloud_log_mutex);
	QFile file(CloudLogFilePath());
	if(file.open(QIODevice::WriteOnly | QIODevice::Append))
	{
		file.write(line.toUtf8());
		file.flush();
	}
}

void CloudLogMessage(const char *tag, const char *message)
{
	CloudLogWrite(tag, CHIAKI_LOG_INFO, message);
}

void CloudChiakiLog::LogCb(ChiakiLogLevel level, const char *msg, void *user)
{
	auto *ctx = static_cast<Context *>(user);
	CloudLogWrite(ctx->tag.constData(), level, msg);
}

CloudChiakiLog::CloudChiakiLog(uint32_t level_mask, const char *tag)
{
	ctx.tag = QByteArray(tag ? tag : "Cloud");
	chiaki_log_init(&log, level_mask, LogCb, &ctx);
}
