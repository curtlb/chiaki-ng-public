// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CLOUDHTTPTRACE_H
#define CLOUDHTTPTRACE_H

#include <chiaki/httptrace.h>

#include <QObject>
#include <QMutex>
#include <QWaitCondition>
#include <atomic>

class QPlainTextEdit;
class QLabel;
class QPushButton;
class QWidget;

// Separate console window for the HTTP traffic of one cloud-game launch.
// Installed only on the provisioning thread. Step mode blocks that thread
// until the user presses "Отправить" or aborts.
class CloudHttpTrace : public QObject
{
	Q_OBJECT

public:
	static CloudHttpTrace *instance();

	void openForLaunch(const QString &gameId, bool stepMode);
	void bindThisThread();
	void unbindThisThread();
	void handleWindowClosed();
	bool launchWasCancelled() const { return cancel_rest.load(); }

	class ThreadBind
	{
	public:
		ThreadBind();
		~ThreadBind();
		ThreadBind(const ThreadBind &) = delete;
		ThreadBind &operator=(const ThreadBind &) = delete;
	};

private:
	explicit CloudHttpTrace(QObject *parent = nullptr);
	static bool inspectThunk(void *user, const ChiakiHttpExchange *exchange);
	bool onExchange(const ChiakiHttpExchange *exchange);
	void post(const QString &block);
	void showPending(const QString &block, int index);
	void scrollToEnd();
	void onSend();
	void onAbort();

	QWidget *window = nullptr;
	QPlainTextEdit *log = nullptr;
	QLabel *status = nullptr;
	QPushButton *sendBtn = nullptr;
	QPushButton *abortBtn = nullptr;
	QMutex mu;
	QWaitCondition cv;
	bool waiting = false;
	bool send_ok = false;
	std::atomic<bool> step_mode{false};
	std::atomic<bool> cancel_rest{false};
};

#endif
