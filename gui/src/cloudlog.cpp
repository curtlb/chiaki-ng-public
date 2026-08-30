// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <cloudlog.h>

#include <chiaki/log.h>
#include <chiaki/version.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QMutex>
#include <QStandardPaths>

Q_DECLARE_LOGGING_CATEGORY(chiakiGui)

static QMutex cloud_log_mutex;
static bool cloud_log_initialized = false;

static QString resolveCloudLogPath()
{
	const QString exe_dir = QCoreApplication::applicationDirPath();
	if(!exe_dir.isEmpty())
	{
		const QString exe_path = QDir(exe_dir).absoluteFilePath(QStringLiteral("chiaki_cloud.log"));
		QFile probe(exe_path);
		if(probe.open(QIODevice::WriteOnly | QIODevice::Append))
		{
			probe.close();
			return exe_path;
		}
	}

	const QString fallback_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	if(!fallback_dir.isEmpty())
	{
		QDir().mkpath(fallback_dir);
		return QDir(fallback_dir).absoluteFilePath(QStringLiteral("chiaki_cloud.log"));
	}

	return QString();
}

QString CloudLogFilePath()
{
	static QString path;
	if(path.isEmpty())
		path = resolveCloudLogPath();
	return path;
}

static bool appendCloudLogLine(const QString &line)
{
	const QString path = CloudLogFilePath();
	if(path.isEmpty())
	{
		qCWarning(chiakiGui) << "Cloud log: no writable path (applicationDirPath="
		                     << QCoreApplication::applicationDirPath() << ")";
		return false;
	}

	QFile file(path);
	if(!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
	{
		qCWarning(chiakiGui) << "Cloud log: failed to open" << path << ":" << file.errorString();
		return false;
	}

	file.write(line.toUtf8());
	file.flush();
	return true;
}

void CloudLogInit()
{
	QMutexLocker lock(&cloud_log_mutex);
	if(cloud_log_initialized)
		return;
	cloud_log_initialized = true;

	const QString path = CloudLogFilePath();
	const QString line = QStringLiteral("[%1] [Cloud] [I] Chiaki cloud log started (version %2), path: %3\n")
		.arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
		     QStringLiteral(CHIAKI_VERSION),
		     path.isEmpty() ? QStringLiteral("<unavailable>") : path);

	if(!appendCloudLogLine(line))
		qCWarning(chiakiGui) << "Cloud log: initialization write failed";
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
	if(!cloud_log_initialized)
		cloud_log_initialized = true;
	appendCloudLogLine(line);
}

void CloudLogMessage(const char *tag, const char *message)
{
	CloudLogWrite(tag, CHIAKI_LOG_INFO, message);
}

void CloudLogMessage(const QString &tag, const QString &message)
{
	CloudLogWrite(tag.toUtf8().constData(), CHIAKI_LOG_INFO, message.toUtf8().constData());
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
