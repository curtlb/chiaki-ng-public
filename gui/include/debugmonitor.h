// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#pragma once

#include <chiaki/log.h>

#include <QObject>
#include <QVariantList>
#include <QMutex>
#include <QStringList>

class DebugMonitor : public QObject
{
	Q_OBJECT
	Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
	Q_PROPERTY(QStringList processes READ processes NOTIFY processesChanged)
	Q_PROPERTY(int entryCount READ entryCount NOTIFY entriesChanged)

public:
	static DebugMonitor *instance();

	static void post(const QString &process, const QString &level, const QString &message);
	static void postChiaki(const QString &process, ChiakiLogLevel level, const char *message);

public slots:
	void appendEntry(const QString &process, const QString &level, const QString &message);

	QVariantList entries() const;
	QStringList processes() const;
	int entryCount() const;

	Q_INVOKABLE QVariantList filteredEntries(const QString &process_filter, const QString &min_level) const;
	Q_INVOKABLE void clear();
	Q_INVOKABLE QString copyFiltered(const QString &process_filter, const QString &min_level) const;
	Q_INVOKABLE void copyToClipboard(const QString &text) const;
	Q_INVOKABLE void logStartupInfo();

signals:
	void entriesChanged();
	void processesChanged();
	void entryAppended();

private:
	explicit DebugMonitor(QObject *parent = nullptr);

	void append(const QString &process, const QString &level, const QString &message);

	static int levelRank(const QString &level);
	static QString chiakiLevelName(ChiakiLogLevel level);

	mutable QMutex mutex;
	QVariantList entries_list;
	QStringList process_list;
	static constexpr int kMaxEntries = 4000;
};
