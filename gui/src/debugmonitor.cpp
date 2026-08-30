// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <debugmonitor.h>
#include <sessionlog.h>

#include <QDateTime>
#include <QMetaObject>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QClipboard>

DebugMonitor *DebugMonitor::instance()
{
	static DebugMonitor *inst = new DebugMonitor();
	return inst;
}

DebugMonitor::DebugMonitor(QObject *parent)
	: QObject(parent)
{
}

void DebugMonitor::post(const QString &process, const QString &level, const QString &message)
{
	if(DebugMonitor *mon = instance())
		mon->append(process, level, message);
}

void DebugMonitor::postChiaki(const QString &process, ChiakiLogLevel level, const char *message)
{
	post(process, chiakiLevelName(level), QString::fromUtf8(message));
}

void DebugMonitor::append(const QString &process, const QString &level, const QString &message)
{
	const QString proc = process.isEmpty() ? QStringLiteral("General") : process;
	const QString lvl = level.isEmpty() ? QStringLiteral("Info") : level;
	const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));

	QVariantMap row;
	row.insert(QStringLiteral("timestamp"), ts);
	row.insert(QStringLiteral("process"), proc);
	row.insert(QStringLiteral("level"), lvl);
	row.insert(QStringLiteral("message"), message);

	bool processes_changed = false;
	{
		QMutexLocker lock(&mutex);
		entries_list.append(row);
		while(entries_list.size() > kMaxEntries)
			entries_list.removeFirst();
		if(!process_list.contains(proc))
		{
			process_list.append(proc);
			processes_changed = true;
		}
	}

	if(processes_changed)
		emit processesChanged();
	emit entriesChanged();
	emit entryAppended();
}

QVariantList DebugMonitor::entries() const
{
	QMutexLocker lock(&mutex);
	return entries_list;
}

QStringList DebugMonitor::processes() const
{
	QMutexLocker lock(&mutex);
	return process_list;
}

int DebugMonitor::entryCount() const
{
	QMutexLocker lock(&mutex);
	return entries_list.size();
}

int DebugMonitor::levelRank(const QString &level)
{
	const QString l = level.toLower();
	if(l == QStringLiteral("debug"))
		return 0;
	if(l == QStringLiteral("verbose"))
		return 0;
	if(l == QStringLiteral("info"))
		return 1;
	if(l == QStringLiteral("warning") || l == QStringLiteral("warn"))
		return 2;
	if(l == QStringLiteral("error"))
		return 3;
	return 1;
}

QString DebugMonitor::chiakiLevelName(ChiakiLogLevel level)
{
	switch(level)
	{
	case CHIAKI_LOG_VERBOSE:
		return QStringLiteral("Verbose");
	case CHIAKI_LOG_DEBUG:
		return QStringLiteral("Debug");
	case CHIAKI_LOG_INFO:
		return QStringLiteral("Info");
	case CHIAKI_LOG_WARNING:
		return QStringLiteral("Warning");
	case CHIAKI_LOG_ERROR:
		return QStringLiteral("Error");
	default:
		return QStringLiteral("Info");
	}
}

QVariantList DebugMonitor::filteredEntries(const QString &process_filter, const QString &min_level) const
{
	const int min_rank = levelRank(min_level);
	QVariantList out;
	QMutexLocker lock(&mutex);
	for(const QVariant &v : entries_list)
	{
		const QVariantMap row = v.toMap();
		const QString proc = row.value(QStringLiteral("process")).toString();
		const QString lvl = row.value(QStringLiteral("level")).toString();
		if(!process_filter.isEmpty() && process_filter != QStringLiteral("All") && proc != process_filter)
			continue;
		if(levelRank(lvl) < min_rank)
			continue;
		out.append(row);
	}
	return out;
}

QString DebugMonitor::copyFiltered(const QString &process_filter, const QString &min_level) const
{
	QString text;
	const QVariantList rows = filteredEntries(process_filter, min_level);
	for(const QVariant &v : rows)
	{
		const QVariantMap row = v.toMap();
		text += QStringLiteral("[%1] [%2] [%3] %4\n")
			.arg(row.value(QStringLiteral("timestamp")).toString(),
			     row.value(QStringLiteral("process")).toString(),
			     row.value(QStringLiteral("level")).toString(),
			     row.value(QStringLiteral("message")).toString());
	}
	return text;
}

void DebugMonitor::copyToClipboard(const QString &text) const
{
	if(QClipboard *cb = QGuiApplication::clipboard())
		cb->setText(text);
}

void DebugMonitor::clear()
{
	{
		QMutexLocker lock(&mutex);
		entries_list.clear();
	}
	emit entriesChanged();
}

void DebugMonitor::logStartupInfo()
{
	post(QStringLiteral("System"), QStringLiteral("Info"),
	     QStringLiteral("chiaki-ng %1 started").arg(CHIAKI_VERSION));
	const QString log_dir = GetLogBaseDir();
	post(QStringLiteral("System"), QStringLiteral("Info"),
	     log_dir.isEmpty()
	         ? QStringLiteral("Log directory unavailable (AppData path missing)")
	         : QStringLiteral("Log directory: %1").arg(log_dir));
	const QString npsso_hint = QStringLiteral("NPSSO in settings: check Cloud tab after login");
	post(QStringLiteral("System"), QStringLiteral("Info"), npsso_hint);
}
