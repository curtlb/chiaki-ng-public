// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloudhttptrace.h"

#include <QCoreApplication>
#include <QCloseEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMutexLocker>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {

class TraceWindow : public QWidget
{
public:
	explicit TraceWindow(CloudHttpTrace *owner, QWidget *parent = nullptr)
		: QWidget(parent), owner(owner)
	{
	}

protected:
	void closeEvent(QCloseEvent *event) override
	{
		owner->handleWindowClosed();
		event->accept();
	}

private:
	CloudHttpTrace *owner;
};

QString bytesToText(const char *data, size_t len)
{
	if(!data || !len)
		return QStringLiteral("(пусто)");
	const QByteArray raw(data, static_cast<int>(len));
	QJsonParseError err;
	const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
	if(err.error == QJsonParseError::NoError && !doc.isNull())
		return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
	return QString::fromUtf8(raw);
}

QString headerValue(const QString &headers, const QString &name)
{
	const QString prefix = name + QLatin1Char(':');
	const QStringList lines = headers.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
	for(const QString &line : lines)
	{
		if(line.startsWith(prefix, Qt::CaseInsensitive))
			return line.mid(prefix.size()).trimmed();
	}
	return QString();
}

QString formatHeaders(const QString &headers)
{
	const QStringList lines = headers.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
	if(lines.isEmpty())
		return QStringLiteral("  (нет)");
	QString out;
	for(const QString &line : lines)
	{
		if(!out.isEmpty())
			out += QLatin1Char('\n');
		out += QStringLiteral("  ") + line;
	}
	return out;
}

QString formatRequest(const ChiakiHttpExchange *ex, bool step)
{
	const QString urlText = ex->url ? QString::fromUtf8(ex->url) : QStringLiteral("(нет url)");
	const QUrl url(urlText);
	const QString headers = ex->request_headers ? QString::fromUtf8(ex->request_headers) : QString();
	QString ua = headerValue(headers, QStringLiteral("User-Agent"));
	QString uaNote;
	if(ua.isEmpty())
	{
		ua = ex->fallback_user_agent ? QString::fromUtf8(ex->fallback_user_agent) : QString();
		uaNote = QStringLiteral(" (отдельного заголовка User-Agent нет, это значение подставляет libcurl)");
	}

	QString queryBlock = QStringLiteral("  (нет)");
	const QUrlQuery query(url);
	if(!query.isEmpty())
	{
		queryBlock.clear();
		const auto items = query.queryItems(QUrl::FullyDecoded);
		for(const auto &item : items)
		{
			if(!queryBlock.isEmpty())
				queryBlock += QLatin1Char('\n');
			queryBlock += QStringLiteral("  %1 = %2").arg(item.first, item.second);
		}
	}

	QString block;
	block += QStringLiteral("──────── ЗАПРОС #%1%2 ────────\n")
		.arg(ex->index)
		.arg(step ? QStringLiteral(" · ПАУЗА, ещё не отправлен") : QString());
	block += QStringLiteral("%1 %2\n\n").arg(ex->method ? QString::fromUtf8(ex->method) : QStringLiteral("GET"), urlText);
	block += QStringLiteral("User-Agent на проводе%1:\n  %2\n\n").arg(uaNote, ua.isEmpty() ? QStringLiteral("(нет)") : ua);
	block += QStringLiteral("Query:\n%1\n\n").arg(queryBlock);
	block += QStringLiteral("Заголовки:\n%1\n\n").arg(formatHeaders(headers));
	block += QStringLiteral("Тело:\n%1\n").arg(bytesToText(ex->request_body, ex->request_body_len));
	return block;
}

QString formatResponse(const ChiakiHttpExchange *ex)
{
	QString block;
	if(ex->transport_error && ex->transport_error[0])
	{
		block += QStringLiteral("──────── ОТВЕТ #%1 · транспорт ────────\n").arg(ex->index);
		block += QString::fromUtf8(ex->transport_error);
		block += QLatin1Char('\n');
		return block;
	}
	block += QStringLiteral("──────── ОТВЕТ #%1 · HTTP %2 · %3 мс ────────\n")
		.arg(ex->index)
		.arg(ex->status_code)
		.arg(ex->elapsed_ms);
	if(ex->redirect_url && ex->redirect_url[0])
		block += QStringLiteral("Redirect:\n  %1\n\n").arg(QString::fromUtf8(ex->redirect_url));
	const QString headers = ex->response_headers
		? QString::fromUtf8(ex->response_headers, static_cast<int>(ex->response_headers_len))
		: QString();
	block += QStringLiteral("Заголовки:\n%1\n\n").arg(formatHeaders(headers));
	block += QStringLiteral("Тело:\n%1\n").arg(bytesToText(ex->response_body, ex->response_body_len));
	return block;
}

} // namespace

CloudHttpTrace::ThreadBind::ThreadBind()
{
	CloudHttpTrace::instance()->bindThisThread();
}

CloudHttpTrace::ThreadBind::~ThreadBind()
{
	CloudHttpTrace::instance()->unbindThisThread();
}

CloudHttpTrace *CloudHttpTrace::instance()
{
	static CloudHttpTrace *trace = new CloudHttpTrace(qApp);
	return trace;
}

CloudHttpTrace::CloudHttpTrace(QObject *parent)
	: QObject(parent)
{
	auto *win = new TraceWindow(this);
	win->setAttribute(Qt::WA_DeleteOnClose, false);
	win->setWindowTitle(QStringLiteral("Облачные запросы"));
	win->resize(860, 680);
	win->setStyleSheet(QStringLiteral(
		"QWidget { background: #121418; color: #D6E2EA; }"
		"QPlainTextEdit { background: #0C0E12; color: #E7EEF3; border: 1px solid #2A3340; "
		"font-family: Consolas, 'Cascadia Mono', monospace; font-size: 12px; }"
		"QLabel { color: #D6E2EA; }"
		"QPushButton { background: #1C2836; color: #E7EEF3; border: 1px solid #3D5168; "
		"padding: 8px 16px; border-radius: 4px; }"
		"QPushButton:disabled { color: #6C7A88; border-color: #2A3340; }"));

	log = new QPlainTextEdit(win);
	log->setReadOnly(true);
	log->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	QFont font(QStringLiteral("Consolas"));
	font.setStyleHint(QFont::Monospace);
	font.setPointSize(10);
	log->setFont(font);

	status = new QLabel(win);
	status->setWordWrap(true);

	sendBtn = new QPushButton(QStringLiteral("Отправить"), win);
	abortBtn = new QPushButton(QStringLiteral("Прервать запуск"), win);
	sendBtn->setEnabled(false);
	abortBtn->setEnabled(false);

	auto *buttons = new QHBoxLayout();
	buttons->addWidget(sendBtn);
	buttons->addWidget(abortBtn);
	buttons->addStretch(1);

	auto *layout = new QVBoxLayout(win);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->addWidget(status);
	layout->addWidget(log, 1);
	layout->addLayout(buttons);
	window = win;

	connect(sendBtn, &QPushButton::clicked, this, [this]{ onSend(); });
	connect(abortBtn, &QPushButton::clicked, this, [this]{ onAbort(); });
}

void CloudHttpTrace::openForLaunch(const QString &gameId, bool stepMode)
{
	{
		QMutexLocker lock(&mu);
		waiting = false;
		send_ok = false;
		cv.wakeAll();
	}
	cancel_rest = false;
	// The trace window was a debug view of Sony/Gaikai HTTP. Do not show it,
	// and do not pause a request on a button the user cannot press.
	(void)stepMode;
	step_mode = false;
	log->clear();
	const QString mode = stepMode
		? QStringLiteral("Пошаговый режим: каждый запрос показан целиком и уходит только по кнопке «Отправить».")
		: QStringLiteral("Живой журнал: запросы уходят сразу и тут же дописываются вместе с ответом.");
	log->appendPlainText(QStringLiteral(
		"=== Запуск %1 ===\n%2\n"
		"В окне видны заголовки, User-Agent, query и тело, включая npsso и ключи сессии. Это локальный журнал.\n"
		"UDP-пинг датацентров сюда не попадает: это не HTTP.\n"
		"Файл chiaki_cloud.log по-прежнему маскирует секреты.\n")
		.arg(gameId.isEmpty() ? QStringLiteral("(без id)") : gameId, mode));
	status->setText(stepMode
		? QStringLiteral("Ждём первый запрос. Он встанет на паузу.")
		: QStringLiteral("Журнал открыт. Запросы уходят без паузы."));
	sendBtn->setVisible(stepMode);
	abortBtn->setVisible(stepMode);
	sendBtn->setEnabled(false);
	abortBtn->setEnabled(false);
}

void CloudHttpTrace::bindThisThread()
{
	chiaki_http_set_thread_inspector(&CloudHttpTrace::inspectThunk, this);
}

void CloudHttpTrace::unbindThisThread()
{
	chiaki_http_set_thread_inspector(NULL, NULL);
}

bool CloudHttpTrace::inspectThunk(void *user, const ChiakiHttpExchange *exchange)
{
	return static_cast<CloudHttpTrace *>(user)->onExchange(exchange);
}

bool CloudHttpTrace::onExchange(const ChiakiHttpExchange *exchange)
{
	if(exchange->phase == 0)
	{
		if(cancel_rest.load())
			return false;
		const QString block = formatRequest(exchange, step_mode.load());
		if(!step_mode.load())
		{
			post(block);
			return true;
		}
		{
			QMutexLocker lock(&mu);
			waiting = true;
			send_ok = false;
		}
		const int index = exchange->index;
		QMetaObject::invokeMethod(this, [this, block, index]{ showPending(block, index); }, Qt::QueuedConnection);
		QMutexLocker lock(&mu);
		while(waiting)
			cv.wait(&mu);
		return send_ok;
	}
	post(formatResponse(exchange));
	return true;
}

void CloudHttpTrace::post(const QString &block)
{
	QMetaObject::invokeMethod(this, [this, block]{
		log->appendPlainText(block);
		scrollToEnd();
		if(!step_mode.load())
			status->setText(QStringLiteral("Запрос ушёл сразу. Ответ дописан ниже."));
		else if(!waiting)
			status->setText(QStringLiteral("Ответ получен. Следующий запрос снова встанет на паузу."));
	}, Qt::QueuedConnection);
}

void CloudHttpTrace::showPending(const QString &block, int index)
{
	log->appendPlainText(block);
	scrollToEnd();
	status->setText(QStringLiteral("Запрос %1 готов и стоит на паузе.").arg(index));
	sendBtn->setEnabled(true);
	abortBtn->setEnabled(true);
}

void CloudHttpTrace::scrollToEnd()
{
	QScrollBar *bar = log->verticalScrollBar();
	bar->setValue(bar->maximum());
}

void CloudHttpTrace::onSend()
{
	sendBtn->setEnabled(false);
	log->appendPlainText(QStringLiteral("→ отправлено\n"));
	status->setText(QStringLiteral("Запрос ушёл, ждём ответ..."));
	QMutexLocker lock(&mu);
	waiting = false;
	send_ok = true;
	cv.wakeAll();
}

void CloudHttpTrace::onAbort()
{
	cancel_rest = true;
	sendBtn->setEnabled(false);
	abortBtn->setEnabled(false);
	log->appendPlainText(QStringLiteral("■ запуск прерван, оставшиеся запросы не отправляются\n"));
	status->setText(QStringLiteral("Запуск прерван."));
	QMutexLocker lock(&mu);
	waiting = false;
	send_ok = false;
	cv.wakeAll();
}

void CloudHttpTrace::handleWindowClosed()
{
	if(step_mode.load())
		onAbort();
}
