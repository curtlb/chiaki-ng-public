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
	/** true если идёт циклическое воспроизведение (файл slot_NN_cycle.json). */
	Q_PROPERTY(bool playbackLoop READ isPlaybackLoop NOTIFY playingChanged)

public:
	explicit MacroRecorder(QObject *parent = nullptr);

	bool isRecording() const { return recording; }
	bool isPlaying() const { return playing; }
	bool isPlaybackLoop() const { return playback_loop; }

	void recordIfActive(const ChiakiControllerState *state);
	/** Если playing — подставляет кадр из макроса в state. */
	void mergePlaybackState(ChiakiControllerState *state);

	Q_INVOKABLE void toggleRecording();
	/** Сброс записи без сохранения (например при обрыве сессии). */
	void cancelRecordingWithoutSave();
	Q_INVOKABLE void stopPlayback();
	/** slot 1–12: slot_NN.json один раз; slot_NN_cycle.json — по кругу до повторного нажатия той же клавиши. */
	Q_INVOKABLE void playSlot(int slot);
	/** Один раз проиграть слот, затем сразу дозаписать в slot_NN.json (F10 — сохранить). */
	Q_INVOKABLE void playSlotThenAppend(int slot);
	Q_INVOKABLE static QString macrosDirectory();

	/** Текущее время воспроизведения (ms) относительно начала сегмента. */
	Q_INVOKABLE qint64 playbackElapsedMs() const;
	/** Длительность сегмента воспроизведения (ms). Для cycle возвращает -1. */
	Q_INVOKABLE qint64 playbackDurationMs() const;
	/** Оставшееся время (ms) до окончания сегмента. Для cycle возвращает -1. */
	Q_INVOKABLE qint64 playbackRemainingMs() const;

signals:
	void recordingChanged();
	void playingChanged();
	void playbackFinished();
	void recordSaved(const QString &path);

private slots:
	void finishPlaybackOneShot();

private:
	void startRecording();
	void stopRecordingSave(const QString &path);
	bool loadFromFile(const QString &path);
	static QString slotFilePath(int slot, bool cycle);
	void playbackCue(bool endOfMacroSegment);

	static QJsonObject stateToJson(const ChiakiControllerState &s);
	static bool jsonToState(const QJsonObject &o, ChiakiControllerState *s);
	static bool loadSamples(const QJsonObject &root, QVector<MacroSample> *out);
	static QJsonObject rootToJson(const QVector<MacroSample> &samples, const QString &name);

	bool recording = false;
	bool playing = false;
	bool playback_loop = false;
	int playback_slot = 0;
	bool play_then_record_pending = false;
	int pending_append_slot = 0;
	QString append_record_path;
	qint64 record_time_base = 0;
	QElapsedTimer record_timer;
	QElapsedTimer play_timer;
	QVector<MacroSample> samples;
	int play_index = 0;
	ChiakiControllerState last_recorded_state;
	bool has_last_recorded = false;
};

#endif
