// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#pragma once

#include <chiaki/log.h>

#include <QByteArray>

QString CloudLogFilePath();
void CloudLogMessage(const char *tag, const char *message);
void CloudLogWrite(const char *tag, ChiakiLogLevel level, const char *message);

class CloudChiakiLog
{
public:
	CloudChiakiLog(uint32_t level_mask, const char *tag);
	~CloudChiakiLog() = default;

	ChiakiLog *GetChiakiLog() { return &log; }

private:
	struct Context
	{
		QByteArray tag;
	};

	static void LogCb(ChiakiLogLevel level, const char *msg, void *user);

	Context ctx;
	ChiakiLog log;
};
