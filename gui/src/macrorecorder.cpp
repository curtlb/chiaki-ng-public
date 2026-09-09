// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "macrorecorder.h"

#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QtMath>
#include <QStandardPaths>
#include <QMetaObject>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

static QString MacrosDir()
{
	QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
	return base + QStringLiteral("/chiaki/macros");
}

QString MacroRecorder::macrosDirectory()
{
	return MacrosDir();
}

QString MacroRecorder::slotFilePath(int slot, bool cycle)
{
	if(slot < 1 || slot > 12)
		return {};
	QDir d(MacrosDir());
	if(cycle)
		return d.filePath(QStringLiteral("slot_%1_cycle.json").arg(slot, 2, 10, QChar('0')));
	return d.filePath(QStringLiteral("slot_%1.json").arg(slot, 2, 10, QChar('0')));
}

MacroRecorder::MacroRecorder(QObject *parent)
	: QObject(parent)
{
}

void MacroRecorder::playbackCue(bool end)
{
#ifdef Q_OS_WIN
	if(end)
		MessageBeep(MB_ICONASTERISK);
	else
		MessageBeep(0xFFFFFFFF);
#else
	Q_UNUSED(end);
#endif
}

void MacroRecorder::startRecording()
{
	if(playing)
		stopPlayback();
	append_record_path.clear();
	play_then_record_pending = false;
	pending_append_slot = 0;
	samples.clear();
	has_last_recorded = false;
	recording = true;
	record_timer.start();
	emit recordingChanged();
}

void MacroRecorder::stopRecordingSave(const QString &path)
{
	if(!recording)
		return;
	recording = false;
	emit recordingChanged();

	QJsonObject root = rootToJson(samples, QFileInfo(path).baseName());
	QJsonDocument doc(root);
	QDir().mkpath(QFileInfo(path).path());
	QFile f(path);
	if(!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return;
	f.write(doc.toJson(QJsonDocument::Indented));
	f.close();
	append_record_path.clear();
	emit recordSaved(path);
}

void MacroRecorder::toggleRecording()
{
	if(recording)
	{
		QString path;
		if(append_record_path.isEmpty())
		{
			QDir d(MacrosDir());
			d.mkpath(QStringLiteral("."));
			path = d.filePath(QStringLiteral("auto_%1.json").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
		}
		else
			path = append_record_path;
		stopRecordingSave(path);
		append_record_path.clear();
	}
	else
		startRecording();
}

void MacroRecorder::recordIfActive(const ChiakiControllerState *state)
{
	if(!recording)
		return;
	qint64 t = append_record_path.isEmpty() ? record_timer.elapsed() : (record_time_base + record_timer.elapsed());
	if(has_last_recorded && chiaki_controller_state_equals(&last_recorded_state, state))
		return;
	last_recorded_state = *state;
	has_last_recorded = true;
	MacroSample s;
	s.t_ms = t;
	s.state = *state;
	samples.push_back(s);
}

void MacroRecorder::mergePlaybackState(ChiakiControllerState *state)
{
	if(!playing || samples.isEmpty())
		return;
	const qint64 t = play_timer.elapsed();
	while(play_index + 1 < samples.size() && samples[play_index + 1].t_ms <= t)
		play_index++;
	*state = samples[play_index].state;
	if(play_index >= samples.size() - 1 && t > samples.last().t_ms + 200)
	{
		if(playback_loop)
		{
			play_timer.restart();
			play_index = 0;
		}
		else
			QMetaObject::invokeMethod(this, &MacroRecorder::finishPlaybackOneShot, Qt::QueuedConnection);
	}
}

void MacroRecorder::finishPlaybackOneShot()
{
	if(!playing || playback_loop)
		return;
	if(play_then_record_pending && !samples.isEmpty())
	{
		playbackCue(true);
		playing = false;
		playback_loop = false;
		const int slot = pending_append_slot;
		pending_append_slot = 0;
		playback_slot = 0;
		play_index = 0;
		play_then_record_pending = false;
		emit playingChanged();
		emit playbackFinished();

		append_record_path = slotFilePath(slot, false);
		record_time_base = samples.last().t_ms + 1;
		record_timer.restart();
		recording = true;
		has_last_recorded = false;
		chiaki_controller_state_set_idle(&last_recorded_state);
		emit recordingChanged();
		return;
	}
	playbackCue(true);
	stopPlayback();
}

void MacroRecorder::stopPlayback()
{
	if(!playing)
		return;
	playing = false;
	playback_loop = false;
	playback_slot = 0;
	play_then_record_pending = false;
	pending_append_slot = 0;
	play_index = 0;
	emit playingChanged();
	emit playbackFinished();
}

qint64 MacroRecorder::playbackElapsedMs() const
{
	if(!playing)
		return 0;
	return play_timer.elapsed();
}

qint64 MacroRecorder::playbackDurationMs() const
{
	if(!playing || samples.isEmpty())
		return 0;
	if(playback_loop)
		return -1;
	// match mergePlaybackState() finish margin
	return samples.last().t_ms + 200;
}

qint64 MacroRecorder::playbackRemainingMs() const
{
	const qint64 duration = playbackDurationMs();
	if(duration < 0)
		return -1;
	const qint64 elapsed = playbackElapsedMs();
	return qMax<qint64>(0, duration - elapsed);
}

void MacroRecorder::playSlot(int slot)
{
	if(slot < 1 || slot > 12)
		return;

	play_then_record_pending = false;
	pending_append_slot = 0;

	if(playing && playback_loop && playback_slot == slot)
	{
		stopPlayback();
		return;
	}

	QDir d(MacrosDir());
	d.mkpath(QStringLiteral("."));
	const QString cycle_path = slotFilePath(slot, true);
	const QString normal_path = slotFilePath(slot, false);
	const bool has_cycle = QFile::exists(cycle_path);
	const QString path = has_cycle ? cycle_path : normal_path;
	if(!loadFromFile(path))
		return;
	if(recording)
	{
		recording = false;
		emit recordingChanged();
	}
	playback_loop = has_cycle;
	playback_slot = slot;
	playing = true;
	play_index = 0;
	play_timer.start();
	emit playingChanged();
	playbackCue(false);
}

void MacroRecorder::playSlotThenAppend(int slot)
{
	if(slot < 1 || slot > 12)
		return;

	if(playing && playback_loop && playback_slot == slot)
	{
		stopPlayback();
		return;
	}

	if(recording)
		cancelRecordingWithoutSave();

	QDir d(MacrosDir());
	d.mkpath(QStringLiteral("."));
	const QString normal_path = slotFilePath(slot, false);
	const QString cycle_path = slotFilePath(slot, true);
	QString load_path;
	if(QFile::exists(normal_path))
		load_path = normal_path;
	else if(QFile::exists(cycle_path))
		load_path = cycle_path;
	else
		return;
	if(!loadFromFile(load_path))
		return;

	play_then_record_pending = true;
	pending_append_slot = slot;
	append_record_path.clear();
	playback_loop = false;
	playback_slot = slot;
	playing = true;
	play_index = 0;
	play_timer.start();
	emit playingChanged();
	playbackCue(false);
}

bool MacroRecorder::loadFromFile(const QString &path)
{
	QFile f(path);
	if(!f.open(QIODevice::ReadOnly))
		return false;
	QJsonParseError err{};
	QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
	f.close();
	if(err.error != QJsonParseError::NoError || !doc.isObject())
		return false;
	QVector<MacroSample> loaded;
	if(!loadSamples(doc.object(), &loaded) || loaded.isEmpty())
		return false;
	samples = loaded;
	return true;
}

QJsonObject MacroRecorder::stateToJson(const ChiakiControllerState &s)
{
	QJsonObject o;
	o[QStringLiteral("buttons")] = (qint64)s.buttons;
	o[QStringLiteral("l2")] = s.l2_state;
	o[QStringLiteral("r2")] = s.r2_state;
	o[QStringLiteral("lx")] = s.left_x;
	o[QStringLiteral("ly")] = s.left_y;
	o[QStringLiteral("rx")] = s.right_x;
	o[QStringLiteral("ry")] = s.right_y;
	o[QStringLiteral("touch_id_next")] = s.touch_id_next;
	QJsonArray touches;
	for(size_t i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; i++)
	{
		QJsonObject ti;
		ti[QStringLiteral("id")] = s.touches[i].id;
		ti[QStringLiteral("x")] = s.touches[i].x;
		ti[QStringLiteral("y")] = s.touches[i].y;
		touches.append(ti);
	}
	o[QStringLiteral("touches")] = touches;
	QJsonArray gyro;
	gyro.append(s.gyro_x);
	gyro.append(s.gyro_y);
	gyro.append(s.gyro_z);
	o[QStringLiteral("gyro")] = gyro;
	QJsonArray accel;
	accel.append(s.accel_x);
	accel.append(s.accel_y);
	accel.append(s.accel_z);
	o[QStringLiteral("accel")] = accel;
	QJsonArray orient;
	orient.append(s.orient_x);
	orient.append(s.orient_y);
	orient.append(s.orient_z);
	orient.append(s.orient_w);
	o[QStringLiteral("orient")] = orient;
	return o;
}

static bool parseTouches(const QJsonValue &v, ChiakiControllerState *s)
{
	if(!v.isArray())
		return false;
	QJsonArray arr = v.toArray();
	int n = qMin(arr.size(), (int)CHIAKI_CONTROLLER_TOUCHES_MAX);
	for(int i = 0; i < n; i++)
	{
		if(!arr[i].isObject())
			return false;
		QJsonObject ti = arr[i].toObject();
		s->touches[i].id = (int8_t)ti[QStringLiteral("id")].toInt(-1);
		s->touches[i].x = (uint16_t)ti[QStringLiteral("x")].toInt(0);
		s->touches[i].y = (uint16_t)ti[QStringLiteral("y")].toInt(0);
	}
	return true;
}

bool MacroRecorder::jsonToState(const QJsonObject &o, ChiakiControllerState *s)
{
	chiaki_controller_state_set_idle(s);
	if(o.contains(QStringLiteral("btn")))
		s->buttons = (uint32_t)o[QStringLiteral("btn")].toVariant().toULongLong();
	else if(o.contains(QStringLiteral("buttons")))
		s->buttons = (uint32_t)o[QStringLiteral("buttons")].toVariant().toULongLong();
	s->l2_state = (uint8_t)qBound(0, o[QStringLiteral("l2")].toInt(0), 255);
	s->r2_state = (uint8_t)qBound(0, o[QStringLiteral("r2")].toInt(0), 255);
	s->left_x = (int16_t)o[QStringLiteral("lx")].toInt(0);
	s->left_y = (int16_t)o[QStringLiteral("ly")].toInt(0);
	s->right_x = (int16_t)o[QStringLiteral("rx")].toInt(0);
	s->right_y = (int16_t)o[QStringLiteral("ry")].toInt(0);
	s->touch_id_next = (uint8_t)o[QStringLiteral("touch_id_next")].toInt(0);
	if(o.contains(QStringLiteral("touches")) && o[QStringLiteral("touches")].isArray())
		parseTouches(o[QStringLiteral("touches")], s);
	if(o.contains(QStringLiteral("gyro")) && o[QStringLiteral("gyro")].isArray())
	{
		QJsonArray g = o[QStringLiteral("gyro")].toArray();
		if(g.size() >= 3)
		{
			s->gyro_x = (float)g[0].toDouble();
			s->gyro_y = (float)g[1].toDouble();
			s->gyro_z = (float)g[2].toDouble();
		}
	}
	if(o.contains(QStringLiteral("accel")) && o[QStringLiteral("accel")].isArray())
	{
		QJsonArray a = o[QStringLiteral("accel")].toArray();
		if(a.size() >= 3)
		{
			s->accel_x = (float)a[0].toDouble();
			s->accel_y = (float)a[1].toDouble();
			s->accel_z = (float)a[2].toDouble();
		}
	}
	if(o.contains(QStringLiteral("orient")) && o[QStringLiteral("orient")].isArray())
	{
		QJsonArray or_ = o[QStringLiteral("orient")].toArray();
		if(or_.size() >= 4)
		{
			s->orient_x = (float)or_[0].toDouble();
			s->orient_y = (float)or_[1].toDouble();
			s->orient_z = (float)or_[2].toDouble();
			s->orient_w = (float)or_[3].toDouble();
		}
	}
	return true;
}

void MacroRecorder::cancelRecordingWithoutSave()
{
	if(!recording)
		return;
	recording = false;
	append_record_path.clear();
	play_then_record_pending = false;
	pending_append_slot = 0;
	samples.clear();
	has_last_recorded = false;
	emit recordingChanged();
}

bool MacroRecorder::loadSamples(const QJsonObject &root, QVector<MacroSample> *out)
{
	out->clear();
	QJsonArray arr;
	if(root.contains(QStringLiteral("samples")))
		arr = root[QStringLiteral("samples")].toArray();
	else
		return false;
	for(const QJsonValue &v : arr)
	{
		if(!v.isObject())
			return false;
		QJsonObject o = v.toObject();
		MacroSample ms;
		ms.t_ms = o[QStringLiteral("t")].toVariant().toLongLong();
		if(!jsonToState(o, &ms.state))
			return false;
		out->push_back(ms);
	}
	return !out->isEmpty();
}

QJsonObject MacroRecorder::rootToJson(const QVector<MacroSample> &samples, const QString &name)
{
	QJsonObject root;
	root[QStringLiteral("version")] = 2;
	root[QStringLiteral("name")] = name;
	QJsonArray arr;
	for(const MacroSample &ms : samples)
	{
		QJsonObject o = stateToJson(ms.state);
		o[QStringLiteral("t")] = ms.t_ms;
		arr.append(o);
	}
	root[QStringLiteral("samples")] = arr;
	return root;
}
