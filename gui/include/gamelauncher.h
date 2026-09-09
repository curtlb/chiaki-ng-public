// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_GAMELAUNCHER_H
#define CHIAKI_GAMELAUNCHER_H

#include <QObject>
#include <QTimer>
#include <QString>
#include <functional>
#include <vector>
#include <memory>
#include <chiaki/session.h>

class StreamSession;

class GameLauncher : public QObject
{
	Q_OBJECT

private:
	struct Action {
		std::function<void(std::function<void()>)> execute;
	};

	StreamSession *session;
	QString game_name;
	ChiakiLog *log;
	qint64 start_timestamp;
	std::vector<Action> actions;
	size_t current_action_index;
	std::shared_ptr<bool> is_shutting_down;
	bool is_completed;

	bool shouldAbort() const { return *is_shutting_down; }
	void scheduleSafe(int msec, std::function<void()> callback);
	void pressButtonAndContinue(ChiakiControllerButton button, const char *action_name, int press_duration, int pause_after, std::function<void()> next);
	void holdAnalogStickAndContinue(int16_t left_x, int16_t left_y, const char *action_name, int hold_duration, int pause_after, std::function<void()> next);
	void sendKeyboardTextAndContinue(const QString &text, int pause_after, std::function<void()> next);
	void logAction(const char *action);

	void runAll(std::vector<Action> action_list);
	void runNextAction();
	void onComplete();

private slots:
	void onConnectedChanged();

signals:
	void automationCompleted();

public:
	explicit GameLauncher(StreamSession *session, const QString &game_name, QObject *parent = nullptr);
	~GameLauncher();

	void start();
	bool isCompleted() const { return is_completed; }
};

#endif // CHIAKI_GAMELAUNCHER_H
