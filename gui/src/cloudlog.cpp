// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <cloudlog.h>

#include <chiaki/log.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMutex>
#include <QStandardPaths>

#include <cstdio>

Q_DECLARE_LOGGING_CATEGORY(chiakiGui)

static QMutex cloud_log_mutex;
static bool cloud_log_initialized = false;
static QStringList cloud_log_paths;
static bool cloud_log_paths_resolved = false;

static void ensureCloudLogPaths()
{
	if(cloud_log_paths_resolved)
		return;
	cloud_log_paths_resolved = true;
	cloud_log_paths.clear();

	const QString exe_dir = QCoreApplication::applicationDirPath();
	if(!exe_dir.isEmpty())
		cloud_log_paths.append(QDir(exe_dir).absoluteFilePath(QStringLiteral("chiaki_cloud.log")));

	const QString appdata_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	if(!appdata_dir.isEmpty())
	{
		QDir().mkpath(appdata_dir);
		cloud_log_paths.append(QDir(appdata_dir).absoluteFilePath(QStringLiteral("chiaki_cloud.log")));
	}
}

static void makeFileWritable(const QString &path)
{
	QFileInfo info(path);
	if(!info.exists())
		return;
	QFile::setPermissions(path,
		info.permissions() | QFileDevice::WriteUser | QFileDevice::WriteGroup | QFileDevice::WriteOther);
}

static bool appendWithStdio(const QString &path, const QByteArray &line)
{
#ifdef _WIN32
	const std::wstring wpath = path.toStdWString();
	FILE *f = _wfopen(wpath.c_str(), L"a");
#else
	const QByteArray native = QFile::encodeName(path);
	FILE *f = fopen(native.constData(), "a");
#endif
	if(!f)
		return false;
	const size_t written = fwrite(line.constData(), 1, static_cast<size_t>(line.size()), f);
	fflush(f);
	fclose(f);
	return written == static_cast<size_t>(line.size());
}

static bool appendCloudLogLineToPath(const QString &path, const QByteArray &line)
{
	if(path.isEmpty())
		return false;

	makeFileWritable(path);

	QFile file(path);
	if(file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
	{
		const qint64 written = file.write(line);
		file.flush();
		if(written == line.size())
			return true;
	}

	return appendWithStdio(path, line);
}

static void writeLocationHintFile()
{
	const QString exe_dir = QCoreApplication::applicationDirPath();
	if(exe_dir.isEmpty())
		return;

	QString content = QStringLiteral("Chiaki cloud log locations:\r\n");
	for(const QString &path : cloud_log_paths)
		content += path + QStringLiteral("\r\n");
	content += QStringLiteral("\r\nDo not create chiaki_cloud.log manually. Delete it if logging stays empty.\r\n");

	const QString hint_path = QDir(exe_dir).absoluteFilePath(QStringLiteral("chiaki_cloud.log.location.txt"));
	QFile hint(hint_path);
	if(hint.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		hint.write(content.toUtf8());
}

static void appendCloudLogLine(const QString &line)
{
	ensureCloudLogPaths();
	const QByteArray bytes = line.toUtf8();
	bool any_written = false;

	for(const QString &path : cloud_log_paths)
	{
		if(appendCloudLogLineToPath(path, bytes))
			any_written = true;
		else
			qCWarning(chiakiGui) << "Cloud log: failed to write to" << path;
	}

	if(!any_written)
		qCWarning(chiakiGui) << "Cloud log: no writable target (exe dir:"
		                     << QCoreApplication::applicationDirPath() << ")";
}

QString CloudLogFilePath()
{
	ensureCloudLogPaths();
	return cloud_log_paths.isEmpty() ? QString() : cloud_log_paths.at(0);
}

QString CloudLogFilePathAlt()
{
	ensureCloudLogPaths();
	return cloud_log_paths.size() > 1 ? cloud_log_paths.at(1) : QString();
}

QStringList CloudLogAllPaths()
{
	ensureCloudLogPaths();
	return cloud_log_paths;
}

void CloudLogInit()
{
	QMutexLocker lock(&cloud_log_mutex);
	if(cloud_log_initialized)
		return;
	cloud_log_initialized = true;

	ensureCloudLogPaths();
	writeLocationHintFile();

	const QString line = QStringLiteral("[%1] [Cloud] [I] Chiaki cloud log started (version %2), exe dir: %3\n")
		.arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
		     QStringLiteral(CHIAKI_VERSION),
		     QCoreApplication::applicationDirPath());

	appendCloudLogLine(line);
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
	{
		cloud_log_initialized = true;
		ensureCloudLogPaths();
		writeLocationHintFile();
	}
	appendCloudLogLine(line);
}

void CloudLogMessage(const char *tag, const char *message)
{
	CloudLogWrite(tag, CHIAKI_LOG_INFO, message);
}

void CloudLogMessage(const QString &tag, const QString &message)
{
	const QByteArray tag_bytes = tag.toUtf8();
	const QByteArray message_bytes = message.toUtf8();
	CloudLogWrite(tag_bytes.constData(), CHIAKI_LOG_INFO, message_bytes.constData());
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
