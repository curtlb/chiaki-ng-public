// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_MACRORECORDER_H
#define CHIAKI_MACRORECORDER_H

#include <chiaki/controller.h>

#include <QObject>
#include <QVector>
#include <QElapsedTimer>
#include <QString>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

struct MacroSample
{
	qint64 t_ms;
	ChiakiControllerState state;
};

class MacroRecorder : public QObject
{
	Q_OBJECT
	Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
	Q_PROPERTY(bool playing READ isPlaying NOTIFY playingChanged)

public:
	explicit MacroRecorder(QObject *parent = nullptr);

	bool isRecording() const { return recording; }
	bool isPlaying() const { return playing; }

	void recordIfActive(const ChiakiControllerState *state);
	/** Если playing — подставляет кадр из макроса в state. */
	void mergePlaybackState(ChiakiControllerState *state);

	Q_INVOKABLE void toggleRecording();
	/** Сброс записи без сохранения (например при обрыве сессии). */
	void cancelRecordingWithoutSave();
	Q_INVOKABLE void stopPlayback();
	/** slot 1–12: файл %AppConfig%/chiaki/macros/slot_NN.json */
	Q_INVOKABLE void playSlot(int slot);
	Q_INVOKABLE static QString macrosDirectory();

signals:
	void recordingChanged();
	void playingChanged();
	void playbackFinished();
	void recordSaved(const QString &path);

private:
	void startRecording();
	void stopRecordingSave(const QString &path);
	bool loadFromFile(const QString &path);

	static QJsonObject stateToJson(const ChiakiControllerState &s);
	static bool jsonToState(const QJsonObject &o, ChiakiControllerState *s);
	static bool loadSamples(const QJsonObject &root, QVector<MacroSample> *out);
	static QJsonObject rootToJson(const QVector<MacroSample> &samples, const QString &name);

	bool recording = false;
	bool playing = false;
	QElapsedTimer record_timer;
	QElapsedTimer play_timer;
	QVector<MacroSample> samples;
	int play_index = 0;
	ChiakiControllerState last_recorded_state;
	bool has_last_recorded = false;
};

#endif
