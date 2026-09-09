#include "qmlbackend.h"
#include "qmlsettings.h"
#include "qmlmainwindow.h"
#include "streamsession.h"
#include "macrorecorder.h"
#include "controllermanager.h"
#include "psnaccountid.h"
#include "psntoken.h"
#include "systemdinhibit.h"
#include "crashreporter.h"
#include "cloudlog.h"
#include "cloudbillingclient.h"
#include "chiaki/remote/holepunch.h"
#ifdef Q_OS_MACOS
#include "macWakeSleep.h"
#elif defined(Q_OS_WINDOWS)
#include "windowsWakeSleep.h"
#endif
#if CHIAKI_GUI_ENABLE_STEAM_SHORTCUT
#include "steamtools.h"
#endif

#ifdef CHIAKI_HAVE_WEBENGINE
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QWebEngineClientHints>
#endif
#include <QWebEngineCookieStore>
#endif
#include <QUrlQuery>
#include <QtGlobal>
#include <QGuiApplication>
#include <QPixmap>
#include <QImageReader>
#include <QProcessEnvironment>
#include <QDesktopServices>
#include <QTimer>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QTemporaryFile>
#include <QNetworkCookie>
#include <QJsonDocument>
#include <QJsonObject>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QTextCodec>
#else
#include <QStringDecoder>
#endif


Q_DECLARE_LOGGING_CATEGORY(chiakiGui)

static void syncFourCloudEmailFromJwtDecode(Settings *settings, QmlSettings *qmlSettings,
	CloudCatalogBackend *catalog, const QJsonObject &decodeObj)
{
	if(!settings)
		return;
	QString email = decodeObj.value(QStringLiteral("Email")).toString().trimmed();
	if(email.isEmpty())
		email = decodeObj.value(QStringLiteral("email")).toString().trimmed();
	if(email.isEmpty())
		email = decodeObj.value(QStringLiteral("User")).toString().trimmed();
	if(email.isEmpty())
		email = decodeObj.value(QStringLiteral("user")).toString().trimmed();
	if(email.isEmpty())
		email = decodeObj.value(QStringLiteral("Login")).toString().trimmed();
	if(email.isEmpty())
		email = decodeObj.value(QStringLiteral("login")).toString().trimmed();
	if(email.isEmpty())
		return;
	email = email.toLower();
	if(settings->GetFourCloudEmail().compare(email, Qt::CaseInsensitive) == 0
		&& !settings->GetFourCloudEmail().isEmpty())
		return;
	settings->SetFourCloudEmail(email);
	if(qmlSettings)
		qmlSettings->refreshFourCloudEmail();
	if(catalog)
		catalog->invalidateCache();
	qCInfo(chiakiGui) << "Synced 4cloud billing email from JWT/session:" << email;
}

static void ResizeWindowForStream(QmlMainWindow *window, Settings *settings, unsigned int width, unsigned int height)
{
	if(!window || window->windowState() == Qt::WindowFullScreen)
		return;

	if(settings->GetWindowType() == WindowType::CustomResolution)
	{
		window->resize(settings->GetCustomResolutionWidth(), settings->GetCustomResolutionHeight());
		window->setMaximumSize(QSize(settings->GetCustomResolutionWidth(), settings->GetCustomResolutionHeight()));
	}
	else if(settings->GetWindowType() == WindowType::AdjustableResolution)
	{
		window->normalTime();
		if(!settings->GetStreamGeometry().isEmpty())
			window->setGeometry(settings->GetStreamGeometry());
	}
	else
	{
		window->resize((int)width, (int)height);
	}
}

// Парсит ответ status_console.php (тело ответа). Не смотрим на HTTP код. Пробуем UTF-8 и Windows-1251.
static QString parseFourcloudStatusBody(const QByteArray &body)
{
	QByteArray trimmedBody = body.trimmed();
	// Убираем UTF-8 BOM, если есть (PHP может отдавать с BOM)
	if (trimmedBody.startsWith("\xEF\xBB\xBF"))
		trimmedBody = trimmedBody.mid(3);
	// Берём первую строку — статус должен быть в начале (защита от HTML/Notice в ответе)
	int firstLineEnd = trimmedBody.indexOf('\n');
	if (firstLineEnd > 0)
		trimmedBody = trimmedBody.left(firstLineEnd);
	trimmedBody = trimmedBody.trimmed();

	QString text = QString::fromUtf8(trimmedBody);
	auto hasKeyword = [&text]() {
		return text.contains(QStringLiteral("Онлайн")) || text.contains(QStringLiteral("Спит")) || text.contains(QStringLiteral("Оффлайн"));
	};
	if (!hasKeyword()) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
		QTextCodec *codec = QTextCodec::codecForName("Windows-1251");
		if (codec)
			text = codec->toUnicode(trimmedBody);
#else
		QStringDecoder dec("Windows-1251");
		if (dec.isValid()) {
			QString decoded = dec.decode(trimmedBody);
			text = decoded;
		}
#endif
		text = text.trimmed();
	}
	QString result;
	if (text.contains(QStringLiteral("Онлайн")))
		result = QStringLiteral("ready");
	else if (text.contains(QStringLiteral("Спит")))
		result = QStringLiteral("standby");
	else if (text.contains(QStringLiteral("Оффлайн")))
		result = QStringLiteral("unknown");
	qCInfo(chiakiGui) << "[4cloud parse] body size:" << body.size()
		<< "firstLine:" << QString::fromUtf8(trimmedBody.left(80)).replace(QChar('\r'), QChar(' ')).replace(QChar('\n'), QChar(' '))
		<< "parsed:" << (result.isEmpty() ? "fail" : result);
	return result;
}

#define PSN_DEVICES_TRIES 2
#define MAX_PSN_RECONNECT_TRIES 6
#define PSN_INTERNET_WAIT_SECONDS 5
#define WAKEUP_PSN_IGNORE_SECONDS 10
#define WAKEUP_WAIT_SECONDS 25
static QMutex chiaki_log_mutex;
static ChiakiLog *chiaki_log_ctx = nullptr;
static QtMessageHandler qt_msg_handler = nullptr;

static void msg_handler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    QMutexLocker lock(&chiaki_log_mutex);
    if (!chiaki_log_ctx) {
        qt_msg_handler(type, context, msg);
        return;
    }
    ChiakiLogLevel chiaki_level;
    switch (type) {
    case QtDebugMsg:
        chiaki_level = CHIAKI_LOG_DEBUG;
        break;
    case QtInfoMsg:
        chiaki_level = CHIAKI_LOG_INFO;
        break;
    case QtWarningMsg:
        chiaki_level = CHIAKI_LOG_WARNING;
        break;
    case QtCriticalMsg:
        chiaki_level = CHIAKI_LOG_ERROR;
        break;
    case QtFatalMsg:
        chiaki_level = CHIAKI_LOG_ERROR;
        // Отправляем отчет о fatal ошибке
        {
            QString stackTrace = QString("File: %1, Line: %2, Function: %3")
                .arg(context.file ? context.file : "unknown")
                .arg(context.line)
                .arg(context.function ? context.function : "unknown");
            CrashReporter::SendReport("qt_fatal", msg, stackTrace);
        }
        break;
    }
    chiaki_log(chiaki_log_ctx, chiaki_level, "%s", qPrintable(msg));
}

QmlRegist::QmlRegist(const ChiakiRegistInfo &regist_info, uint32_t log_mask, QObject *parent)
    : QObject(parent)
{
    chiaki_log_init(&chiaki_log, log_mask, &QmlRegist::log_cb, this);
    chiaki_regist_start(&chiaki_regist, &chiaki_log, &regist_info, &QmlRegist::regist_cb, this);
}

void QmlRegist::log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
    chiaki_log_cb_print(level, msg, nullptr);
    auto r = static_cast<QmlRegist*>(user);
    QMetaObject::invokeMethod(r, std::bind(&QmlRegist::log, r, level, QString::fromUtf8(msg)), Qt::QueuedConnection);
}

void QmlRegist::regist_cb(ChiakiRegistEvent *event, void *user)
{
    auto r = static_cast<QmlRegist*>(user);
    switch (event->type) {
    case CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS:
        QMetaObject::invokeMethod(r, std::bind(&QmlRegist::success, r, *event->registered_host), Qt::QueuedConnection);
        QMetaObject::invokeMethod(r, &QObject::deleteLater, Qt::QueuedConnection);
        break;
    case CHIAKI_REGIST_EVENT_TYPE_FINISHED_FAILED:
        QMetaObject::invokeMethod(r, &QmlRegist::failed, Qt::QueuedConnection);
        QMetaObject::invokeMethod(r, &QObject::deleteLater, Qt::QueuedConnection);
        break;
    default:
        break;
    }
}

QmlBackend::QmlBackend(Settings *settings, QmlMainWindow *window)
    : QObject(window)
    , settings(settings)
    , settings_qml(new QmlSettings(settings, this))
    , window(window)
{
    qt_msg_handler = qInstallMessageHandler(msg_handler);

    if (settings)
        settings->ClearPersistedNpssoTokens();

    const char *uri = "org.streetpea.chiaking";
    qmlRegisterSingletonInstance(uri, 1, 0, "Chiaki", this);
    qmlRegisterUncreatableType<QmlMainWindow>(uri, 1, 0, "ChiakiWindow", {});
    qmlRegisterUncreatableType<QmlSettings>(uri, 1, 0, "ChiakiSettings", {});
    qmlRegisterUncreatableType<StreamSession>(uri, 1, 0, "ChiakiSession", {});
    qmlRegisterUncreatableType<MacroRecorder>(uri, 1, 0, "ChiakiMacroRecorder", QStringLiteral("Use session.macroRecorder"));

    QObject *frame_obj = new QObject();
    frame_thread = new QThread(frame_obj);
    frame_thread->setObjectName("frame");
    frame_thread->start();
    frame_obj->moveToThread(frame_thread);

    PsnConnectionWorker *worker = new PsnConnectionWorker;
    worker->moveToThread(&psn_connection_thread);
    connect(&psn_connection_thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(this, &QmlBackend::psnConnect, worker, &PsnConnectionWorker::ConnectPsnConnection);
    connect(worker, &PsnConnectionWorker::resultReady, this, &QmlBackend::checkPsnConnection);
    connect(&psn_hosts_watcher, &QFutureWatcher<void>::finished, [this]{ this->updating_psn_hosts = false; });
    psn_connection_thread.start();

    setConnectState(PsnConnectState::NotStarted);
    connect(settings_qml, &QmlSettings::audioVolumeChanged, this, &QmlBackend::updateAudioVolume);
    connect(settings_qml, &QmlSettings::placeboChanged, window, &QmlMainWindow::updatePlacebo);
    connect(settings_qml, &QmlSettings::streamMenuEnabledChanged, this, &QmlBackend::updateStreamShortcut);
    connect(settings_qml, &QmlSettings::streamMenuShortcut1Changed, this, &QmlBackend::updateStreamShortcut);
    connect(settings_qml, &QmlSettings::streamMenuShortcut2Changed, this, &QmlBackend::updateStreamShortcut);
    connect(settings_qml, &QmlSettings::streamMenuShortcut3Changed, this, &QmlBackend::updateStreamShortcut);
    connect(settings_qml, &QmlSettings::streamMenuShortcut4Changed, this, &QmlBackend::updateStreamShortcut);
    connect(settings, &Settings::RegisteredHostsUpdated, this, &QmlBackend::hostsChanged);
    connect(settings, &Settings::HiddenHostsUpdated, this, &QmlBackend::hiddenHostsChanged);
    connect(settings, &Settings::ManualHostsUpdated, this, &QmlBackend::hostsChanged);
    connect(settings, &Settings::CurrentProfileChanged, this, &QmlBackend::profileChanged);

    cloud_streaming_backend = new CloudStreamingBackend(settings, this);
    cloud_catalog_backend = new CloudCatalogBackend(settings, this);
    CloudLogInit();

    connect(settings_qml, &QmlSettings::cloudStoreLocaleChanged, this, [this]() {
        cloud_catalog_backend->invalidateCache();
    });
    connect(settings, &Settings::NpssoTokenChanged, this, [this]() {
        const QString token = this->settings->GetNpssoToken();
        CloudLogMessage(QStringLiteral("Settings"),
            token.isEmpty() ? QStringLiteral("NPSSO token cleared")
                            : QStringLiteral("NPSSO token updated (length %1)").arg(token.length()));
        cloud_catalog_backend->invalidateCache();
    });
    connect(cloud_streaming_backend, &CloudStreamingBackend::sessionCreated, this,
            [this, window](StreamSession *session_to_register) {
        qInfo() << "QmlBackend: Registering cloud streaming session";

        if (session) {
            qWarning() << "QmlBackend: Closing existing session before registering new cloud session";
            chiaki_log_mutex.lock();
            chiaki_log_ctx = nullptr;
            chiaki_log_mutex.unlock();
            session->deleteLater();
        }

        session = session_to_register;
        setCloudSessionReconnecting(false);

        chiaki_log_mutex.lock();
        chiaki_log_ctx = session->GetChiakiLog();
        chiaki_log_mutex.unlock();

        const QPointer<StreamSession> bound_session(session_to_register);
        connect(session, &StreamSession::FfmpegFrameAvailable, frame_thread->parent(), [this, window, bound_session]() {
            if (!bound_session || session != bound_session)
                return;
            ChiakiFfmpegDecoder *decoder = bound_session->GetFfmpegDecoder();
            if (!decoder) {
                qCCritical(chiakiGui) << "Session has no FFmpeg decoder";
                return;
            }
            int32_t frames_lost;
            AVFrame *frame = chiaki_ffmpeg_decoder_pull_frame(decoder, &frames_lost);
            if (!frame)
                return;

            bound_session->ApplyDisplayCrop(frame);

            static const QSet<int> zero_copy_formats = {
                AV_PIX_FMT_VULKAN,
#ifdef Q_OS_LINUX
                AV_PIX_FMT_VAAPI,
#endif
            };
            if (frame->hw_frames_ctx && (!zero_copy_formats.contains(frame->format) || disable_zero_copy)) {
                AVFrame *sw_frame = av_frame_alloc();
                if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0) {
                    qCWarning(chiakiGui) << "Failed to transfer frame from hardware";
                    av_frame_unref(frame);
                    av_frame_free(&sw_frame);
                    return;
                }
                av_frame_copy_props(sw_frame, frame);
                av_frame_unref(frame);
                frame = sw_frame;
                bound_session->ApplyDisplayCrop(frame);
            }
            QMetaObject::invokeMethod(window, std::bind(&QmlMainWindow::presentFrame, window, frame, frames_lost));
        });

        connect(session, &StreamSession::SessionQuit, this, [this](ChiakiQuitReason reason, const QString &reason_str) {
            if (cloud_session_reconnect_pending) {
                cloud_session_reconnect_pending = false;
                chiaki_log_mutex.lock();
                chiaki_log_ctx = nullptr;
                chiaki_log_mutex.unlock();
                session->disconnect(this);
                session->deleteLater();
                session = nullptr;
                emit sessionChanged(session);
                updateStreamShortcut();
                if (cloud_streaming_backend) {
                    QTimer::singleShot(3000, cloud_streaming_backend, [this]() {
                        if (cloud_streaming_backend)
                            cloud_streaming_backend->reconnectCurrentSession();
                    });
                }
                return;
            }

            if (chiaki_quit_reason_is_error(reason)) {
                QString m = tr("Chiaki Session has quit") + ":\n" + chiaki_quit_reason_string(reason);
                if (!reason_str.isEmpty())
                    m += "\n" + tr("Reason") + ": \"" + reason_str + "\"";
                emit sessionError(tr("Session has quit"), m);
            }

            if (cloud_streaming_backend)
                cloud_streaming_backend->notifyStreamStopped();

            chiaki_log_mutex.lock();
            chiaki_log_ctx = nullptr;
            chiaki_log_mutex.unlock();

            session->deleteLater();
            session = nullptr;
            emit sessionChanged(session);
            updateStreamShortcut();
            // Console Date_exp polling is unrelated to cloud sessions; calling it
            // after every cloud quit logged out cloud-only users (API "Error").
            if (this->settings && this->settings->GetConsoleCatalogAccess()) {
                startSubscriptionExpiryTimer();
                ensureFourcloudPolling();
            }

            sleep_inhibit->release();
            setDiscoveryEnabled(true);
        });

        connect(session, &StreamSession::ConnectedChanged, this, [this]() {
            if (session->IsConnected())
                setDiscoveryEnabled(false);
        });

        emit sessionChanged(session);
        updateStreamShortcut();

        bool fullscreen = session->GetFullscreen();
        bool zoom = session->GetZoom();
        bool stretch = session->GetStretch();
        if (zoom)
            window->setVideoMode(QmlMainWindow::VideoMode::Zoom);
        else if (stretch)
            window->setVideoMode(QmlMainWindow::VideoMode::Stretch);
        if (fullscreen || zoom || stretch)
            window->fullscreenTime();

        const auto &profile = session->GetChiakiSession()->connect_info.video_profile;
        ResizeWindowForStream(window, this->settings, profile.width, profile.height);

        sleep_inhibit->inhibit();
    });

    connect(&discovery_manager, &DiscoveryManager::HostsUpdated, this, &QmlBackend::updateDiscoveryHosts);
    discovery_manager.SetSettings(settings);
    setDiscoveryEnabled(false);
    connect(ControllerManager::GetInstance(), &ControllerManager::AvailableControllersUpdated, this, &QmlBackend::updateControllers);
    connect(settings_qml, &QmlSettings::allowJoystickBackgroundEventsChanged, this, &QmlBackend::setAllowJoystickBackgroundEvents);
    connect(window, &QmlMainWindow::activeChanged, this, &QmlBackend::setIsAppActive);
    setAllowJoystickBackgroundEvents();
    setIsAppActive();
    ControllerManager::GetInstance()->SetIsAppActive(window->isActive());
    updateControllers();
    updateControllerMappings();
    connect(settings, &Settings::ControllerMappingsUpdated, this, &QmlBackend::updateControllerMappings);
    connect(this, &QmlBackend::controllersChanged, this, &QmlBackend::updateControllerMappings);
    auto_connect_mac = settings->GetAutoConnectHost().GetServerMAC();
    auto_connect_nickname = settings->GetAutoConnectHost().GetServerNickname();
    psn_auto_connect_timer = new QTimer(this);
    psn_auto_connect_timer->setSingleShot(true);
    psn_reconnect_tries = 0;
    psn_reconnect_timer = new QTimer(this);
    wakeup_start_timer = new QTimer(this);
    wakeup_start_timer->setSingleShot(true);
    wakeup_repeat_timer = new QTimer(this);
    wakeup_repeat_timer->setInterval(10000);
    connect(wakeup_repeat_timer, &QTimer::timeout, this, [this]() {
        if (wakeup_start && !wakeup_host_addr.isEmpty())
            sendWakeup(wakeup_host_addr, wakeup_regist_key, wakeup_ps5);
    });
    fourcloud_state_timer = new QTimer(this);
    fourcloud_state_timer->setInterval(15000);
    connect(fourcloud_state_timer, &QTimer::timeout, this, &QmlBackend::fetchFourcloudState);
    subscription_expiry_timer = new QTimer(this);
    subscription_expiry_timer->setInterval(60000); // 1 раз в минуту
    connect(subscription_expiry_timer, &QTimer::timeout, this, &QmlBackend::fetchSubscriptionExpiry);
    if(autoConnect() && !auto_connect_nickname.isEmpty())
    {
        connect(psn_auto_connect_timer, &QTimer::timeout, this, [this]
        {
            int i = 0;
            for (const auto &host : std::as_const(psn_hosts))
            {
                if(host.GetName() == auto_connect_nickname)
                {
                    int index = discovery_manager.GetHosts().size() + this->settings->GetManualHosts().size() + i;
                    connectToHost(index);
                    return;
                }
                i++;
            }
            qCWarning(chiakiGui) << "Couldn't find PSN host with the requested nickname: " << auto_connect_nickname;
        });
        psn_auto_connect_timer->start(PSN_INTERNET_WAIT_SECONDS * 1000);
    }
    connect(psn_reconnect_timer, &QTimer::timeout, this, [this]{
        QString refresh = this->settings->GetPsnRefreshToken();
        if(refresh.isEmpty())
        {
            qCWarning(chiakiGui) << "No refresh token found, can't refresh PSN token to use PSN remote connection";
            psn_reconnect_tries = 0;
            resume_session = false;
            psn_reconnect_timer->stop();
            setConnectState(PsnConnectState::ConnectFailed);
            return;
        }
        PSNToken *psnToken = new PSNToken(this->settings, this);
        connect(psnToken, &PSNToken::PSNTokenError, this, [this](const QString &error) {
            qCWarning(chiakiGui) << "Internet is currently down...waiting 5 seconds" << error;
            psn_reconnect_tries++;
            if(psn_reconnect_tries < MAX_PSN_RECONNECT_TRIES)
                return;
            else
            {
                resume_session = false;
                psn_reconnect_tries = 0;
                psn_reconnect_timer->stop();
                setConnectState(PsnConnectState::ConnectFailed);
            }
        });
        connect(psnToken, &PSNToken::UnauthorizedError, this, &QmlBackend::psnCredsExpired);
        connect(psnToken, &PSNToken::PSNTokenSuccess, this, [this]() {
            qCWarning(chiakiGui) << "PSN Remote Connection Tokens Refreshed. Internet is back up";
            resume_session = false;
            psn_reconnect_tries = 0;
            psn_reconnect_timer->stop();
            createSession(session_info);
        });
        connect(psnToken, &PSNToken::Finished, psnToken, &QObject::deleteLater);
        QString refresh_token = this->settings->GetPsnRefreshToken();
        psnToken->RefreshPsnToken(std::move(refresh_token));
    });
    connect(wakeup_start_timer, &QTimer::timeout, this, [this]
    {
        wakeup_repeat_timer->stop();
        wakeup_host_addr.clear();
        wakeup_regist_key.clear();
        wakeup_nickname.clear();
        wakeup_start = false;
        emit wakeupStartFailed();
    });
    psn_auto_connect_timer->start(PSN_INTERNET_WAIT_SECONDS * 1000);
    sleep_inhibit = new SystemdInhibit(QGuiApplication::applicationName(), tr("Remote Play session"), "sleep", "delay", this);
    connect(sleep_inhibit, &SystemdInhibit::sleep, this, &QmlBackend::goToSleep);
    connect(sleep_inhibit, &SystemdInhibit::resume, this, &QmlBackend::resumeFromSleep);
    connect(ControllerManager::GetInstance(), &ControllerManager::ControllerMoved, sleep_inhibit, &SystemdInhibit::simulateUserActivity);
#ifdef Q_OS_MACOS
    mac_wake_sleep = new MacWakeSleep(this);
    connect(mac_wake_sleep, &MacWakeSleep::wokeUp, this, &QmlBackend::resumeFromSleep);
    connect(ControllerManager::GetInstance(), &ControllerManager::ControllerMoved, mac_wake_sleep, &MacWakeSleep::simulateUserActivity);
#elif defined(Q_OS_WINDOWS)
    windows_wake_sleep = new WindowsWakeSleep(this);
    connect(windows_wake_sleep, &WindowsWakeSleep::wokeUp, this, &QmlBackend::resumeFromSleep);
    connect(windows_wake_sleep, &WindowsWakeSleep::sleeping, this, &QmlBackend::goToSleep);
#endif
    refreshPsnToken();
}

QmlBackend::~QmlBackend()
{
    if(session)
    {
        chiaki_log_mutex.lock();
        chiaki_log_ctx = nullptr;
        chiaki_log_mutex.unlock();
        session->deleteLater();
        session = nullptr;
    }
#ifdef CHIAKI_HAVE_WEBENGINE
    if(request_interceptor)
        request_interceptor->deleteLater();
#endif
    frame_thread->quit();
    frame_thread->wait();
    frame_thread->parent()->deleteLater();
    psn_connection_thread.quit();
    psn_connection_thread.wait();
}

QmlMainWindow *QmlBackend::qmlWindow() const
{
    return window;
}

QmlSettings *QmlBackend::qmlSettings() const
{
    return settings_qml;
}

CloudStreamingBackend *QmlBackend::cloudStreaming() const
{
    return cloud_streaming_backend;
}

CloudCatalogBackend *QmlBackend::cloudCatalog() const
{
    return cloud_catalog_backend;
}

QString QmlBackend::cloudLogPath() const
{
    return CloudLogFilePath();
}

QString QmlBackend::cloudLogPathAlt() const
{
    return CloudLogFilePathAlt();
}

bool QmlBackend::cloudSteamShortcutEnabled() const
{
#if CHIAKI_GUI_ENABLE_STEAM_SHORTCUT
    static const bool steam_installed = [] {
        auto noop = [](const QString &) {};
        SteamTools steam(noop, noop, QString());
        return steam.steamExists();
    }();
    return steam_installed;
#else
    return false;
#endif
}

StreamSession *QmlBackend::qmlSession() const
{
    return session;
}

void QmlBackend::updateAudioVolume()
{
    if(session)
        session->SetAudioVolume(settings->GetAudioVolume());
}

void QmlBackend::goToSleep()
{
    qCInfo(chiakiGui) << "About to sleep";
    if (session) {
        if (this->settings->GetSuspendAction() == SuspendAction::Sleep)
            session->GoToBed();
        session->Stop();
        if(!session_info.duid.isEmpty())
            psnCancel(true);
        resume_session = true;
    }
}
void QmlBackend::resumeFromSleep()
{
#ifdef Q_OS_WINDOWS
    if(windows_wake_sleep->getWakeState() == WindowsWakeState::AboutToSleep)
    {
        windows_wake_sleep->setWakeState(WindowsWakeState::Awake);
        return;
    }
    windows_wake_sleep->setWakeState(WindowsWakeState::Awake);
#endif
    if (resume_session) {
        qCInfo(chiakiGui) << "Resuming session...";
        resume_session = false;
        if(session_info.duid.isEmpty())
        {
            bool resume_zoom = session_info.zoom;
            bool resume_stretch = session_info.stretch;
            StreamSessionConnectInfo resume_info(
                session_info.settings,
                session_info.target,
                session_info.host,
                session_info.nickname,
                session_info.regist_key,
                session_info.morning,
                session_info.initial_login_pin,
                session_info.duid,
                session_info.auto_regist,
                session_info.fullscreen,
                resume_zoom,
                resume_stretch
            );
            resume_info.custom_port_base = session_info.custom_port_base;
            createSession(resume_info);
        }
        else
        {
            emit showPsnView();
            setConnectState(PsnConnectState::WaitingForInternet);
            psn_reconnect_timer->start(PSN_INTERNET_WAIT_SECONDS * 1000);
        }
    }
}

QList<QmlController*> QmlBackend::qmlControllers() const
{
    return controllers.values();
}

void QmlBackend::profileChanged()
{
    QString profile = settings->GetCurrentProfile();
    Settings *settings_copy = new Settings(profile);
    if(settings_allocd)
        settings->deleteLater();
    settings_allocd = true;
    settings = settings_copy;
    emit hostsChanged();
    updateControllerMappings();
    connect(settings, &Settings::RegisteredHostsUpdated, this, &QmlBackend::hostsChanged);
    connect(settings, &Settings::HiddenHostsUpdated, this, &QmlBackend::hiddenHostsChanged);
    connect(settings, &Settings::ManualHostsUpdated, this, &QmlBackend::hostsChanged);
    connect(settings, &Settings::CurrentProfileChanged, this, &QmlBackend::profileChanged);
    connect(settings, &Settings::ControllerMappingsUpdated, this, &QmlBackend::updateControllerMappings);
    settings_qml->setSettings(settings);
    discovery_manager.SetSettings(settings);
    window->setSettings(settings);
    if(cloud_catalog_backend)
    {
        cloud_catalog_backend->setSettings(settings);
        cloud_catalog_backend->invalidateCache();
    }
    if(cloud_streaming_backend)
        cloud_streaming_backend->setSettings(settings);
    setDiscoveryEnabled(true);

    auto_connect_mac = settings->GetAutoConnectHost().GetServerMAC();
    auto_connect_nickname = settings->GetAutoConnectHost().GetServerNickname();
    psn_reconnect_timer->deleteLater();
    psn_auto_connect_timer->deleteLater();
    psn_auto_connect_timer = new QTimer(this);
    psn_auto_connect_timer->setSingleShot(true);
    psn_reconnect_tries = 0;
    psn_reconnect_timer = new QTimer(this);
    if(autoConnect() && !auto_connect_nickname.isEmpty())
    {
        connect(psn_auto_connect_timer, &QTimer::timeout, this, [this]
        {
            int i = 0;
            for (const auto &host : std::as_const(psn_hosts))
            {
                if(host.GetName() == auto_connect_nickname)
                {
                    int index = discovery_manager.GetHosts().size() + this->settings->GetManualHosts().size() + i;
                    connectToHost(index);
                    return;
                }
                i++;
            }
            qCWarning(chiakiGui) << "Couldn't find PSN host with the requested nickname: " << auto_connect_nickname;
        });
        psn_auto_connect_timer->start(PSN_INTERNET_WAIT_SECONDS * 1000);
    }
    connect(psn_reconnect_timer, &QTimer::timeout, this, [this]{
        QString refresh = this->settings->GetPsnRefreshToken();
        if(refresh.isEmpty())
        {
            qCWarning(chiakiGui) << "No refresh token found, can't refresh PSN token to use PSN remote connection";
            psn_reconnect_tries = 0;
            resume_session = false;
            psn_reconnect_timer->stop();
            setConnectState(PsnConnectState::ConnectFailed);
            return;
        }
        PSNToken *psnToken = new PSNToken(this->settings, this);
        connect(psnToken, &PSNToken::PSNTokenError, this, [this](const QString &error) {
            qCWarning(chiakiGui) << "Internet is currently down...waiting 5 seconds" << error;
            psn_reconnect_tries++;
            if(psn_reconnect_tries < MAX_PSN_RECONNECT_TRIES)
                return;
            else
            {
                resume_session = false;
                psn_reconnect_tries = 0;
                psn_reconnect_timer->stop();
                setConnectState(PsnConnectState::ConnectFailed);
            }
        });
        connect(psnToken, &PSNToken::UnauthorizedError, this, &QmlBackend::psnCredsExpired);
        connect(psnToken, &PSNToken::PSNTokenSuccess, this, []() {
            qCWarning(chiakiGui) << "PSN Remote Connection Tokens Refreshed. Internet is back up";
        });
        connect(psnToken, &PSNToken::PSNTokenSuccess, this, [this]() {
            resume_session = false;
            psn_reconnect_tries = 0;
            psn_reconnect_timer->stop();
            createSession(session_info);
        });
        connect(psnToken, &PSNToken::Finished, psnToken, &QObject::deleteLater);
        QString refresh_token = this->settings->GetPsnRefreshToken();
        psnToken->RefreshPsnToken(std::move(refresh_token));
    });
    sleep_inhibit->deleteLater();
    sleep_inhibit = new SystemdInhibit(QGuiApplication::applicationName(), tr("Remote Play session"), "sleep", "delay", this);
    connect(sleep_inhibit, &SystemdInhibit::sleep, this, &QmlBackend::goToSleep);
    connect(sleep_inhibit, &SystemdInhibit::resume, this, &QmlBackend::resumeFromSleep);
    connect(ControllerManager::GetInstance(), &ControllerManager::ControllerMoved, sleep_inhibit, &SystemdInhibit::simulateUserActivity);
#ifdef Q_OS_MACOS
    mac_wake_sleep->deleteLater();
    mac_wake_sleep = new MacWakeSleep(this);
    connect(mac_wake_sleep, &MacWakeSleep::wokeUp, this, &QmlBackend::resumeFromSleep);
    connect(ControllerManager::GetInstance(), &ControllerManager::ControllerMoved, mac_wake_sleep, &MacWakeSleep::simulateUserActivity);
#elif defined(Q_OS_WINDOWS)
    windows_wake_sleep->deleteLater();
    windows_wake_sleep = new WindowsWakeSleep(this);
    connect(windows_wake_sleep, &WindowsWakeSleep::wokeUp, this, &QmlBackend::resumeFromSleep);
    connect(windows_wake_sleep, &WindowsWakeSleep::sleeping, this, &QmlBackend::goToSleep);
#endif
    refreshPsnToken();
    emit hostsChanged();
    emit hiddenHostsChanged();
}

bool QmlBackend::discoveryEnabled() const
{
    return discovery_manager.GetActive();
}

void QmlBackend::setDiscoveryEnabled(bool enabled)
{
    discovery_manager.SetActive(enabled);
    emit discoveryEnabledChanged();
}

QmlBackend::PsnConnectState QmlBackend::connectState() const
{
    return psn_connect_state;
}

void QmlBackend::checkNickname(QString nickname)
{
    if(!session)
        return;
    if(!settings->GetNicknameRegisteredHostRegistered(nickname))
    {
        emit error(tr("PS4 Console Unregistered"), tr("Can't proceed...please register your PS4 console locally"));
        session->Stop();
    }
}

void QmlBackend::setConnectState(PsnConnectState connect_state)
{
    psn_connect_state = connect_state;
    emit connectStateChanged();
}

QVariantList QmlBackend::hosts() const
{
    // При открытии списка хостов запускаем опрос статуса 4cloud, если есть NPS4 и таймер ещё не запущен
    if (!settings->GetManualHosts().isEmpty() && !settings->GetNps4().isEmpty()
        && fourcloud_state_timer && !fourcloud_state_timer->isActive()) {
        QMetaObject::invokeMethod(const_cast<QmlBackend *>(this), "ensureFourcloudPolling", Qt::QueuedConnection);
    }
    QVariantList out;
    QList<QString> discovered_nicknames;
    QList<ManualHost> discovered_manual_hosts;
    size_t registered_discovered_ps4s = 0;
    auto manual_hosts = settings->GetManualHosts();
    for (const auto &host : discovery_manager.GetHosts()) {
        QVariantMap m;
        HostMAC host_mac = host.GetHostMAC();
        bool registered = settings->GetRegisteredHostRegistered(host_mac);
        bool hidden = settings->GetHiddenHostHidden(host_mac);
        if(registered && hidden)
        {
            settings->RemoveHiddenHost(host_mac);
            bool hidden = false;
        }
        // Update hidden host nickname if it's changed
        if(hidden)
        {
            auto hidden_host = settings->GetHiddenHost(host_mac);
            if(hidden_host.GetNickname() != host.host_name)
            {
                hidden_host.SetNickname(host.host_name);
                settings->RemoveHiddenHost(host_mac);
                settings->AddHiddenHost(hidden_host);
            }
        }
        m["discovered"] = true;
        bool manual = false;
        for(int i = 0; i < manual_hosts.length(); i++)
        {
            const auto &manual_host = manual_hosts.at(i);
            if(manual_host.GetRegistered() && manual_host.GetMAC() == host_mac && manual_host.GetHost() == host.host_addr)
            {
                manual = true;
                discovered_manual_hosts.append(manual_host);
            }
        }
        m["manual"] = manual;
        m["name"] = host.host_name;
        QString duid = "";
        if(!registered)
        {
            if(psn_nickname_hosts.contains(host.host_name))
                duid = psn_nickname_hosts.value(host.host_name).GetDuid();
            else if(!host.ps5)
                duid =  psn_nickname_hosts.value(QString("Main PS4 Console")).GetDuid();
        }
        m["duid"] = duid;
        m["address"] = host.host_addr;
        m["ps5"] = host.ps5;
        m["mac"] = host_mac.ToString();
        m["state"] = chiaki_discovery_host_state_string(host.state);
        m["app"] = host.running_app_name;
        m["titleId"] = host.running_app_titleid;
        m["registered"] = registered;
        m["display"] = hidden ? false : true;
        discovered_nicknames.append(host.host_name);
        out.append(m);
        if(!host.ps5 && registered)
            registered_discovered_ps4s++;
    }
    QString jwt_psn = settings->GetJwtPsn();
    for (const auto &host : settings->GetManualHosts()) {
        QVariantMap m;
        m["discovered"] = false;
        m["manual"] = true;
        m["name"] = host.GetHost();
        m["duid"] = "";
        m["address"] = host.GetHost();
        m["state"] = "unknown";
        if (!settings->GetNps4().isEmpty()) {
            // Пока нет ответа от API 4cloud, не показываем "Оффлайн" — показываем "Проверка…"
            if (fourcloud_state_cache.isEmpty())
                m["state"] = QStringLiteral("checking");
            else if (fourcloud_state_retrying && (fourcloud_state_cache == QStringLiteral("unknown")))
                m["state"] = QStringLiteral("checking");  // повторная проверка, не показываем оффлайн
            else if (!fourcloud_state_cache.isEmpty())
                m["state"] = fourcloud_state_cache;
        }
        m["registered"] = false;
        m["display"] = discovered_manual_hosts.contains(host) ? false : true;
        if (host.GetRegistered() && settings->GetRegisteredHostRegistered(host.GetMAC())) {
            auto registered = settings->GetRegisteredHost(host.GetMAC());
            m["registered"] = true;
            m["ps5"] = chiaki_target_is_ps5(registered.GetTarget());
            m["mac"] = registered.GetServerMAC().ToString();
        }
        if (!jwt_psn.isEmpty())
            m["name"] = jwt_psn;
        else if (m["registered"].toBool())
            m["name"] = settings->GetRegisteredHost(host.GetMAC()).GetServerNickname();
        out.append(m);
    }
    if(registered_discovered_ps4s >= settings->GetPS4RegisteredHostsRegistered())
        discovered_nicknames.append(QString("Main PS4 Console"));
    for (const auto &host : psn_hosts) {
        QVariantMap m;
        // Only list PSN remote hosts that aren't discovered locally
        bool discovered = false;
        for (int i = 0; i < discovered_nicknames.size(); ++i)
        {
            if (discovered_nicknames.at(i) == host.GetName())
                discovered = true;
        }
        for (int i = 0; i < waking_sleeping_nicknames.size(); ++i)
        {
            if (waking_sleeping_nicknames.at(i) == host.GetName())
                discovered = true;
        }
        if(discovered)
            continue;
        m["discovered"] = false;
        m["manual"] = false;
        m["display"] = true;
        m["name"] = host.GetName();
        m["duid"] = host.GetDuid();
        m["address"] = "";
        m["registered"] = true;
        m["ps5"] = host.IsPS5();
        out.append(m);
    }
    return out;
}

bool QmlBackend::autoConnect() const
{
    return auto_connect_mac.GetValue();
}

void QmlBackend::psnCancel(bool stop_thread)
{
    session->CancelPsnConnection(stop_thread);
}

void QmlBackend::checkPsnConnection(const ChiakiErrorCode &err)
{
    switch(err)
    {
        case CHIAKI_ERR_SUCCESS:
            setConnectState(PsnConnectState::LinkingConsole);
            psnSessionStart();
            break;
        case CHIAKI_ERR_HOST_DOWN:
            setConnectState(PsnConnectState::ConnectFailedStart);
            if(session)
            {
                chiaki_log_mutex.lock();
                chiaki_log_ctx = nullptr;
                chiaki_log_mutex.unlock();
                session->deleteLater();
                session = nullptr;
                setDiscoveryEnabled(true);
                startSubscriptionExpiryTimer();
            }
            break;
        case CHIAKI_ERR_HOST_UNREACH:
            setConnectState(PsnConnectState::ConnectFailedConsoleUnreachable);
            if(session)
            {
                chiaki_log_mutex.lock();
                chiaki_log_ctx = nullptr;
                chiaki_log_mutex.unlock();
                session->deleteLater();
                session = nullptr;
                setDiscoveryEnabled(true);
                startSubscriptionExpiryTimer();
            }
            break;
        default:
            setConnectState(PsnConnectState::ConnectFailed);
            if(session)
            {
                chiaki_log_mutex.lock();
                chiaki_log_ctx = nullptr;
                chiaki_log_mutex.unlock();
                session->deleteLater();
                session = nullptr;
                setDiscoveryEnabled(true);
                startSubscriptionExpiryTimer();
            }
            break;
    }
}

void QmlBackend::psnSessionStart()
{
    try {
        session->Start();
    } catch (const Exception &e) {
        CrashReporter::SendExceptionReport("Exception", e.what());
        chiaki_log_mutex.lock();
        chiaki_log_ctx = nullptr;
        chiaki_log_mutex.unlock();
        session->deleteLater();
        session = nullptr;
        emit error(tr("Stream failed"), tr("Failed to start Stream Session: %1").arg(e.what()));
        return;
    }

    sleep_inhibit->inhibit();
}

void QmlBackend::createSession(const StreamSessionConnectInfo &connect_info)
{
    if (autoConnect()) {
        auto_connect_mac = {};
        emit autoConnectChanged();
    }

    if (session) {
        qCWarning(chiakiGui) << "Another session is already active";
        return;
    }

    // Не опрашивать статус консоли и подписку во время стрима — они могут блокировать трансляцию
    if (fourcloud_state_timer && fourcloud_state_timer->isActive())
        fourcloud_state_timer->stop();
    if (subscription_expiry_timer && subscription_expiry_timer->isActive())
        subscription_expiry_timer->stop();

    session_info = connect_info;
    QStringList availableDecoders = settings_qml->availableDecoders();
    if(session_info.hw_decoder == "auto")
    {
        session_info.hw_decoder = QString();
#if defined(Q_OS_LINUX)
        if(availableDecoders.contains("vulkan"))
        {
            qCInfo(chiakiGui) << "Auto hw decoder selecting vulkan";
            session_info.hw_decoder = "vulkan";
        }
        else if(availableDecoders.contains("vaapi"))
        {
            qCInfo(chiakiGui) << "Auto hw decoder selecting vaapi";
            session_info.hw_decoder = "vaapi";
        }
#elif defined(Q_OS_WIN)
        if(availableDecoders.contains("vulkan"))
        {
            qCInfo(chiakiGui) << "Auto hw decoder selecting vulkan";
            session_info.hw_decoder = "vulkan";
        }
        else if(availableDecoders.contains("d3d11va"))
        {
            qCInfo(chiakiGui) << "Auto hw decoder selecting d3d11va";
            session_info.hw_decoder = "d3d11va";
        }
#elif defined(Q_OS_MACOS)
        if(availableDecoders.contains("videotoolbox"))
        {
            qCInfo(chiakiGui) << "Auto hw decoder selecting videotoolbox";
            session_info.hw_decoder = "videotoolbox";
        }
#endif
    }
    if (session_info.hw_decoder == "vulkan") {
#if defined(Q_OS_LINUX)
        if(qEnvironmentVariableIsSet("APPIMAGE") && (qEnvironmentVariableIsSet("SteamDeck") || qEnvironmentVariable("DESKTOP_SESSION").contains("steamos")))
        {
            qCInfo(chiakiGui) << "Auto hw decoder falling back to vaapi because radv has a bug with vulkan hw decode in SteamOS 3.6";
            session_info.hw_decoder = "vaapi";
        }
        else
        {
#endif
            session_info.hw_device_ctx = window->vulkanHwDeviceCtx();
            if (!session_info.hw_device_ctx)
            {
                session_info.hw_decoder.clear();
                qCInfo(chiakiGui) << "vulkan video decoding not supported by your gpu driver, retrying other hw video decoders";
#if defined(Q_OS_LINUX)
                if(availableDecoders.contains("vaapi"))
                {
                    qCInfo(chiakiGui) << "Falling back to vaapi";
                    session_info.hw_decoder = "vaapi";
                }
#elif defined(Q_OS_WIN)
                if(availableDecoders.contains("d3d11va"))
                {
                    qCInfo(chiakiGui) << "Falling back to d3d11va";
                    session_info.hw_decoder = "d3d11va";
                }
#endif
            }
            if(session_info.hw_decoder.isEmpty())
                qCInfo(chiakiGui) << "Falling back to software decoder";
#if defined(Q_OS_LINUX)
        }
#endif
    }

    try {
        session = new StreamSession(session_info, this);
    } catch (const Exception &e) {
        CrashReporter::SendExceptionReport("Exception", e.what());
        emit error(tr("Stream failed"), tr("Failed to initialize Stream Session: %1").arg(e.what()));
        return;
    }

    connect(session, &StreamSession::FfmpegFrameAvailable, frame_thread->parent(), [this]() {
        ChiakiFfmpegDecoder *decoder = session->GetFfmpegDecoder();
        if (!decoder) {
            qCCritical(chiakiGui) << "Session has no FFmpeg decoder";
            return;
        }
        int32_t frames_lost;
        AVFrame *frame = chiaki_ffmpeg_decoder_pull_frame(decoder, &frames_lost);
        if (!frame)
            return;

        session->ApplyDisplayCrop(frame);

        static const QSet<int> zero_copy_formats = {
            AV_PIX_FMT_VULKAN,
#ifdef Q_OS_LINUX
            AV_PIX_FMT_VAAPI,
#endif
        };
        if (frame->hw_frames_ctx && (!zero_copy_formats.contains(frame->format) || disable_zero_copy)) {
            AVFrame *sw_frame = av_frame_alloc();
            if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0) {
                qCWarning(chiakiGui) << "Failed to transfer frame from hardware";
                av_frame_unref(frame);
                av_frame_free(&sw_frame);
                return;
            }
            av_frame_copy_props(sw_frame, frame);
            av_frame_unref(frame);
            frame = sw_frame;
            session->ApplyDisplayCrop(frame);
        }
        QMetaObject::invokeMethod(window, std::bind(&QmlMainWindow::presentFrame, window, frame, frames_lost));
    });

    connect(session, &StreamSession::SessionQuit, this, [this](ChiakiQuitReason reason, const QString &reason_str) {
        if (chiaki_quit_reason_is_error(reason)) {
            QString m = tr("Chiaki Session has quit") + ":\n" + chiaki_quit_reason_string(reason);
            if (!reason_str.isEmpty())
                m += "\n" + tr("Reason") + ": \"" + reason_str + "\"";
            emit sessionError(tr("Session has quit"), m);
        }

        chiaki_log_mutex.lock();
        chiaki_log_ctx = nullptr;
        chiaki_log_mutex.unlock();

        session->deleteLater();
        session = nullptr;
        emit sessionChanged(session);
        updateStreamShortcut();
        startSubscriptionExpiryTimer();
        // Сразу запускаем опрос статуса 4cloud, чтобы обновить «Онлайн/Спит/Оффлайн» на главном экране
        ensureFourcloudPolling();

        sleep_inhibit->release();
        setDiscoveryEnabled(true);
#ifdef Q_OS_WINDOWS
        qCInfo(chiakiGui) << "Checking sleep state: ";
        if(windows_wake_sleep->getWakeState() == WindowsWakeState::Awake)
            QTimer::singleShot(2000, this, &QmlBackend::resumeFromSleep);
        else
            windows_wake_sleep->setWakeState(WindowsWakeState::Sleeping);
#endif
    });

    connect(session, &StreamSession::LoginPINRequested, this, [this, connect_info](bool incorrect) {
        if (!connect_info.initial_login_pin.isEmpty() && incorrect == false)
            session->SetLoginPIN(connect_info.initial_login_pin);
        else
            emit sessionPinDialogRequested();
    });

    connect(session, &StreamSession::DataHolepunchProgress, this, [this](bool finished) {
        if(finished)
        {
            setConnectState(PsnConnectState::DataConnectionFinished);
            emit sessionChanged(session);
        }
        else
            setConnectState(PsnConnectState::DataConnectionStart);
    });

    connect(session, &StreamSession::NicknameReceived, this, &QmlBackend::checkNickname);

    connect(session, &StreamSession::AutoRegistSucceeded, this, &QmlBackend::finishAutoRegister);

    connect(session, &StreamSession::ConnectedChanged, this, [this]() {
        if (session->IsConnected())
            setDiscoveryEnabled(false);
    });

    if (window->windowState() != Qt::WindowFullScreen)
    {
        ResizeWindowForStream(window, settings, connect_info.video_profile.width, connect_info.video_profile.height);
    }

    chiaki_log_mutex.lock();
    chiaki_log_ctx = session->GetChiakiLog();
    chiaki_log_mutex.unlock();

    if(connect_info.duid.isEmpty())
    {
        // Сразу подключаемся без ожидания READY (wake-up уже отправлен при need_wakeup)
        try {
            session->Start();
        } catch (const Exception &e) {
            CrashReporter::SendExceptionReport("Exception", e.what());
            emit error(tr("Stream failed"), tr("Failed to start Stream Session: %1").arg(e.what()));
            chiaki_log_mutex.lock();
            chiaki_log_ctx = nullptr;
            chiaki_log_mutex.unlock();
            session->deleteLater();
            session = nullptr;
            updateStreamShortcut();
            startSubscriptionExpiryTimer();
            return;
        }
        emit sessionChanged(session);
        updateStreamShortcut();
        sleep_inhibit->inhibit();
    }
    else
    {
        setDiscoveryEnabled(false);
        emit showPsnView();
        if(session_info.auto_regist)
            setConnectState(PsnConnectState::RegisteringConsole);
        else
            setConnectState(PsnConnectState::InitiatingConnection);
        emit psnConnect(session, session_info.duid, chiaki_target_is_ps5(session_info.target));
    }
}

bool QmlBackend::closeRequested()
{
    if (!session)
        return true;

    bool stop = true;
    bool sleep = false;
    if (session->IsConnected()) {
        switch (settings->GetDisconnectAction()) {
        case DisconnectAction::Ask:
            stop = false;
            emit sessionStopDialogRequested();
            break;
        case DisconnectAction::AlwaysSleep:
            sleep = true;
            break;
        default:
            break;
        }
    }

    if (stop)
        stopSession(sleep);

    return false;
}

void QmlBackend::deleteHost(int index)
{
    auto server = displayServerAt(index);
    auto id = server.manual_host.GetID();
    if (!server.valid || (id < 0))
        return;
    settings->RemoveManualHost(id);
}

void QmlBackend::wakeUpHost(int index, QString nickname)
{
    auto server = displayServerAt(index);
    if (!server.valid)
        return;
    if (!nickname.isEmpty())
    {
        waking_sleeping_nicknames.append(nickname);
        QTimer::singleShot(WAKEUP_PSN_IGNORE_SECONDS * 1000, [this, nickname]{
            waking_sleeping_nicknames.removeOne(nickname);
            emit hostsChanged();
        });
    }
    sendWakeup(server);
}

void QmlBackend::setConsolePin(int index, QString console_pin)
{
    auto server = displayServerAt(index);
    if (!server.valid)
        return;
    server.registered_host.SetConsolePin(server.registered_host, std::move(console_pin));
    settings->AddRegisteredHost(server.registered_host);
}

void QmlBackend::addManualHost(int index, const QString &address)
{
    HostMAC hmac;
    QList<RegisteredHost> registered_hosts = settings->GetRegisteredHosts();
    bool registered = (index >= 0 && (index < registered_hosts.length()));
    if (registered)
        hmac = registered_hosts.at(index).GetServerMAC();
    ManualHost host(-1, address, registered, hmac);
    settings->SetManualHost(host);
}

void QmlBackend::hideHost(const QString &mac_string, const QString &host_nickname)
{
    QByteArray mac_array = QByteArray::fromHex(mac_string.toUtf8());
    if (mac_array.size() != 6)
    {
        qCCritical(chiakiGui) << "Invalid host mac:" << mac_string.toUtf8();
        qCCritical(chiakiGui) << "Aborting hidden host creation because mac string couldn't be converted to a valid host mac!";
        qCCritical(chiakiGui) << "Received an array of unexpected size. Expected: 6 bytes, Received:" << mac_array.size() << "bytes";
        return;
    }
    HostMAC mac((const uint8_t *)mac_array.constData());
    HiddenHost hidden_host(mac, host_nickname);
    settings->AddHiddenHost(hidden_host);
    emit hostsChanged();
}

void QmlBackend::unhideHost(const QString &mac_string)
{
    QByteArray mac_array = QByteArray::fromHex(mac_string.toUtf8());
    const char *mac_ptr = mac_array.constData();
    if (strlen(mac_ptr) != 6)
    {
        qCCritical(chiakiGui) << " Aborting hidden host creation because mac string couldn't be converted to a valid host mac!";
        return;
    }
    HostMAC mac((const uint8_t *)mac_ptr);
    settings->RemoveHiddenHost(mac);
    emit hostsChanged();
}

QVariantList QmlBackend::hiddenHosts() const
{
    QVariantList out;
    for (const auto &host : settings->GetHiddenHosts()) {
        QVariantMap m;
        m["name"] = host.GetNickname();
        m["mac"] = host.GetMAC().ToString();
        out.append(m);
    }
    return out;
}


bool QmlBackend::registerHost(const QString &host, const QString &psn_id, const QString &pin, const QString &cpin, bool broadcast, int target, const QJSValue &callback)
{
    ChiakiRegistInfo info = {};
    QByteArray hostb = host.toUtf8();
    info.host = hostb.constData();
    info.target = static_cast<ChiakiTarget>(target);
    info.broadcast = broadcast;
    info.pin = (uint32_t)pin.toULong();
    info.console_pin = (uint32_t)cpin.toULong();
    info.custom_port_base = settings->GetJwtPort();
    info.holepunch_info = nullptr;
    info.rudp = nullptr;
    QByteArray psn_idb;
    if (target == CHIAKI_TARGET_PS4_8) {
        psn_idb = psn_id.toUtf8();
        info.psn_online_id = psn_idb.constData();
    } else {
        QByteArray account_id = QByteArray::fromBase64(psn_id.toUtf8());
        if (account_id.size() != CHIAKI_PSN_ACCOUNT_ID_SIZE) {
            emit error(tr("Invalid Account-ID"), tr("The PSN Account-ID must be exactly %1 bytes encoded as base64.").arg(CHIAKI_PSN_ACCOUNT_ID_SIZE));
            return false;
        }
        info.psn_online_id = nullptr;
        memcpy(info.psn_account_id, account_id.constData(), CHIAKI_PSN_ACCOUNT_ID_SIZE);
    }
    auto regist = new QmlRegist(info, settings->GetLogLevelMask(), this);
    connect(regist, &QmlRegist::log, this, [callback](ChiakiLogLevel level, QString msg) {
        QJSValue cb = callback;
        if (cb.isCallable())
            cb.call({QString("[%1] %2").arg(chiaki_log_level_char(level)).arg(msg), true, false});
    });
    connect(regist, &QmlRegist::failed, this, [this, callback]() {
        QJSValue cb = callback;
        if (cb.isCallable())
            cb.call({QString(), false, true});

        regist_dialog_server = {};
    });
    connect(regist, &QmlRegist::success, this, [this, host, callback](const RegisteredHost &rhost) {
        QJSValue cb = callback;
        if (cb.isCallable())
            cb.call({QString(), true, true});

        settings->AddRegisteredHost(rhost);
        if(regist_dialog_server.discovered == false)
        {
            ManualHost manual_host = regist_dialog_server.manual_host;
            if(manual_host.GetHost().isEmpty())
                manual_host.SetHost(host);
            manual_host.Register(rhost);
            settings->SetManualHost(manual_host);
        }
    });
    return true;
}

void QmlBackend::autoRegister()
{
    const auto &server = regist_dialog_server;
    resume_session = false;
    StreamSessionConnectInfo info(
            settings,
            server.discovery_host.target,
            QString(),
            QString(),
            QByteArray(),
            QByteArray(),
            0,
            server.duid,
            true,
            false,
            false,
            false);
    info.custom_port_base = settings->GetJwtPort();

    QString expiry_s = settings->GetPsnAuthTokenExpiry();
    QString refresh = settings->GetPsnRefreshToken();
    if(expiry_s.isEmpty() || refresh.isEmpty())
        return;
    QDateTime expiry = QDateTime::fromString(expiry_s, settings->GetTimeFormat());
    // give 1 minute buffer
    QDateTime now = QDateTime::currentDateTime().addSecs(60);
    if(now.secsTo(expiry) < 1)
    {
        PSNToken *psnToken = new PSNToken(settings, this);
        connect(psnToken, &PSNToken::PSNTokenError, this, [this](const QString &error) {
            qCWarning(chiakiGui) << "Could not refresh token. Automatic PSN Connection Unavailable!" << error;
        });
        connect(psnToken, &PSNToken::UnauthorizedError, this, &QmlBackend::psnCredsExpired);
        connect(psnToken, &PSNToken::PSNTokenSuccess, this, []() {
            qCWarning(chiakiGui) << "PSN Remote Connection Tokens Refreshed.";
        });
        connect(psnToken, &PSNToken::PSNTokenSuccess, this, [this, info]() {
            createSession(info);
        });
        connect(psnToken, &PSNToken::Finished, psnToken, &QObject::deleteLater);
        QString refresh_token = settings->GetPsnRefreshToken();
        psnToken->RefreshPsnToken(std::move(refresh_token));
    }
    else
        createSession(info);
}

void QmlBackend::finishAutoRegister(const ChiakiRegisteredHost &host)
{
    QString nickname(host.server_nickname);
    if(!regist_dialog_server.discovery_host.ps5 && regist_dialog_server.discovery_host.host_name != nickname)
    {
        emit error(tr("PS4 Console Not Main"), tr("Can't proceed...%1 is not your main PS4 console in PSN").arg(regist_dialog_server.discovery_host.host_name));
        return;
    }
    settings->AddRegisteredHost(host);
    setConnectState(PsnConnectState::RegistrationFinished);
    updatePsnHosts();
}

#ifdef CHIAKI_HAVE_WEBENGINE
void QmlBackend::clearCookies(QQuickWebEngineProfile *profile)
{
    auto cookieStore = profile->cookieStore();
    cookieStore->deleteAllCookies();
}

void QmlBackend::setWebEngineHints(QQuickWebEngineProfile *profile)
{
    QDate starting_release_date(2025, 02, 18);
    QDate today = QDate::currentDate();
    auto daysSinceStart = starting_release_date.daysTo(today);
    qint64 versionsSinceStart = daysSinceStart / 28;
    qint64 release = 133 + versionsSinceStart;
    QString chrome_version = QString::number(release);
    auto userAgent = profile->httpUserAgent();
    userAgent = userAgent.replace(QRegularExpression(" \\bQtWebEngine[^ ]*\\b"), "");
    userAgent = userAgent.replace(QRegularExpression("\\bChrome[^ ]*\\b"), QString("Chrome/%1.0.0.0").arg(chrome_version));
#ifdef Q_OS_WINDOWS
    userAgent = userAgent.replace("Windows NT 6.2", "Windows NT 10.0");
    userAgent += QString(" Edg/%1.0.0.0").arg(chrome_version);
#endif
    profile->setHttpUserAgent(userAgent);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    auto hints = profile->clientHints();
    hints->setFullVersion(QString("%1.0.0.0").arg(chrome_version));
    QMap<QString, QVariant> fullVersionList;
    fullVersionList.insert("Not A(Brand", "99.0.0.0");
#ifdef Q_OS_WINDOWS
    fullVersionList.insert("Microsoft Edge", QString("%1.0.0.0").arg(chrome_version));
#else
    fullVersionList.insert("Google Chrome", QString("%1.0.0.0").arg(chrome_version));
#endif
    fullVersionList.insert("Chromium", QString("%1.0.0.0").arg(chrome_version));
    hints->setFullVersionList(fullVersionList);
#endif
    request_interceptor = new SecUaRequestInterceptor(chrome_version);
    profile->setUrlRequestInterceptor(request_interceptor);
}
#endif

void QmlBackend::connectToHost(int index, QString nickname)
{
    window->setWindowAdjustable(false);
    auto server = displayServerAt(index);
    if (!server.valid)
        return;

    if (!server.registered) {
        regist_dialog_server = server;
        emit registDialogRequested(server.GetHostAddr(), server.IsPS5(), server.duid);
        return;
    }

    // Для manual/4cloud хоста QML может не передать nickname — берём из зарегистрированного хоста
    if (nickname.isEmpty() && server.registered)
        nickname = server.registered_host.GetServerNickname();

    QString nps4 = settings->GetNps4();
    if (nps4.isEmpty()) {
        bool need_wakeup = (server.discovered && server.discovery_host.state == CHIAKI_DISCOVERY_HOST_STATE_STANDBY)
                        || (settings->GetJwtPort() != 0 && (!server.discovered || server.discovery_host.state == CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN));
        qCInfo(chiakiGui) << "[4cloud connect] no NPS4, need_wakeup from discovery:" << need_wakeup;
        continueConnectToHost(index, nickname, need_wakeup);
        return;
    }

    // Запрос статуса консоли через API 4cloud (Спит / Онлайн / Оффлайн)
    if (!network_manager)
        network_manager = new QNetworkAccessManager(this);
    QUrl statusUrl("https://api.4cloud.pro/status_console.php");
    QUrlQuery statusQuery;
    statusQuery.addQueryItem("NPS4", nps4);
    statusUrl.setQuery(statusQuery);
    qCInfo(chiakiGui) << "[4cloud status] request URL:" << statusUrl.toString();
    QNetworkRequest statusRequest(statusUrl);
    QNetworkReply *statusReply = network_manager->get(statusRequest);
    connect(statusReply, &QNetworkReply::finished, this, [this, statusReply, index, nickname]() {
        auto server = displayServerAt(index);
        if (!server.valid) {
            statusReply->deleteLater();
            return;
        }
        int httpCode = statusReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        auto replyError = statusReply->error();
        QByteArray body = statusReply->readAll();
        statusReply->deleteLater();
        qCInfo(chiakiGui) << "[4cloud status] response httpCode:" << httpCode
            << "error:" << replyError
            << "bodySize:" << body.size();
        QString resolved_nickname = nickname;
        if (resolved_nickname.isEmpty() && server.registered)
            resolved_nickname = server.registered_host.GetServerNickname();
        QString status = parseFourcloudStatusBody(body);
        bool need_wakeup = (status == QStringLiteral("standby") || status == QStringLiteral("unknown"));
        if (status.isEmpty()) {
            need_wakeup = (settings->GetJwtPort() != 0
                           && (!server.discovered || server.discovery_host.state == CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN))
                         || (server.discovered && server.discovery_host.state == CHIAKI_DISCOVERY_HOST_STATE_STANDBY);
        }
        qCInfo(chiakiGui) << "[4cloud status] status:" << (status.isEmpty() ? "parse_failed" : status)
            << "need_wakeup:" << need_wakeup << "-> continueConnectToHost";
        continueConnectToHost(index, resolved_nickname, need_wakeup);
    });
}

void QmlBackend::continueConnectToHost(int index, QString nickname, bool need_wakeup)
{
    auto server = displayServerAt(index);
    if (!server.valid)
        return;

    if (nickname.isEmpty() && server.registered)
        nickname = server.registered_host.GetServerNickname();

    qCInfo(chiakiGui) << "[4cloud connect] need_wakeup:" << need_wakeup << "nickname:" << nickname;
    if (need_wakeup)
    {
        if(!sendWakeup(server))
        {
            qCWarning(chiakiGui) << "Couldn't wakeup server";
            return;
        }
        qCInfo(chiakiGui) << "[4cloud connect] wakeup sent, proceeding to createSession";
        waking_sleeping_nicknames.append(nickname);
        QTimer::singleShot(WAKEUP_PSN_IGNORE_SECONDS * 1000, [this, nickname]{
            waking_sleeping_nicknames.removeOne(nickname);
            emit hostsChanged();
        });
    }

    bool fullscreen = false, zoom = false, stretch = false;
    switch (settings->GetWindowType()) {
    case WindowType::SelectedResolution:
        break;
    case WindowType::CustomResolution:
        break;
    case WindowType::AdjustableResolution:
        break;
    case WindowType::Fullscreen:
        fullscreen = true;
        break;
    case WindowType::Zoom:
        zoom = true;
        break;
    case WindowType::Stretch:
        stretch = true;
        break;
    default:
        break;
    }
    emit windowTypeUpdated(settings->GetWindowType());

    resume_session = false;
    if(server.duid.isEmpty())
    {
        QString host = server.GetHostAddr();
        StreamSessionConnectInfo info(
                settings,
                server.registered_host.GetTarget(),
                std::move(host),
                std::move(nickname),
                server.registered_host.GetRPRegistKey(),
                server.registered_host.GetRPKey(),
                server.registered_host.GetConsolePin(),
                server.duid,
                false,
                fullscreen,
                zoom,
                stretch);
        info.custom_port_base = settings->GetJwtPort();
        createSession(info);
    }
    else
    {
        StreamSessionConnectInfo info(
                settings,
                server.psn_host.GetTarget(),
                QString(),
                QString(),
                QByteArray(),
                QByteArray(),
                server.registered_host.GetConsolePin(),
                server.duid,
                false,
                fullscreen,
                zoom,
                stretch);
        info.custom_port_base = settings->GetJwtPort();

        QString expiry_s = settings->GetPsnAuthTokenExpiry();
        QString refresh = settings->GetPsnRefreshToken();
        if(expiry_s.isEmpty() || refresh.isEmpty())
            return;
        QDateTime expiry = QDateTime::fromString(expiry_s, settings->GetTimeFormat());
        // give 1 minute buffer
        QDateTime now = QDateTime::currentDateTime().addSecs(60);
        if(now.secsTo(expiry) < 1)
        {
            PSNToken *psnToken = new PSNToken(settings, this);
            connect(psnToken, &PSNToken::PSNTokenError, this, [this](const QString &error) {
                qCWarning(chiakiGui) << "Could not refresh token. Automatic PSN Connection Unavailable!" << error;
            });
            connect(psnToken, &PSNToken::UnauthorizedError, this, &QmlBackend::psnCredsExpired);
            connect(psnToken, &PSNToken::PSNTokenSuccess, this, []() {
                qCWarning(chiakiGui) << "PSN Remote Connection Tokens Refreshed.";
            });
            connect(psnToken, &PSNToken::PSNTokenSuccess, this, [this, info]() {
                createSession(info);
            });
                connect(psnToken, &PSNToken::Finished, psnToken, &QObject::deleteLater);
            QString refresh_token = settings->GetPsnRefreshToken();
            psnToken->RefreshPsnToken(std::move(refresh_token));
        }
        else
            createSession(info);
    }
}

void QmlBackend::stopSession(bool sleep)
{
    if (!session)
        return;

    if (session->IsCloudStreaming() && cloud_streaming_backend)
        cloud_streaming_backend->notifyStreamStopped();

    if (!session_info.nickname.isEmpty())
    {
        waking_sleeping_nicknames.append(session_info.nickname);
        QTimer::singleShot(WAKEUP_PSN_IGNORE_SECONDS * 1000, [this]{
            waking_sleeping_nicknames.removeOne(session_info.nickname);
            emit hostsChanged();
        });
    }

    if (sleep)
        session->GoToBed();

    session->Stop();
}

void QmlBackend::reconnectCloudSession()
{
    if (!session || !session->IsCloudStreaming())
        return;
    if (!session->ConsumeCloudSettingsPendingReconnect())
        return;

    cloud_session_reconnect_pending = true;
    setCloudSessionReconnecting(true);
    CloudLogMessage(QStringLiteral("Session"), QStringLiteral("stopping cloud stream to apply new settings"));
    session->Stop();
}

void QmlBackend::setCloudSessionReconnecting(bool reconnecting)
{
    if (cloud_session_reconnecting == reconnecting)
        return;
    cloud_session_reconnecting = reconnecting;
    emit cloudSessionReconnectingChanged();
}

void QmlBackend::sessionGoHome()
{
    if (!session)
        return;

    session->GoHome();
}

void QmlBackend::enterPin(const QString &pin)
{
    qCInfo(chiakiGui) << "Set login pin " << pin;
    if (session)
        session->SetLoginPIN(pin);
}

QUrl QmlBackend::psnLoginUrl() const
{
    size_t duid_size = CHIAKI_DUID_STR_SIZE;
    char duid[duid_size];
    chiaki_holepunch_generate_client_device_uid(duid, &duid_size);
    return QUrl(PSNAuth::LOGIN_URL + "duid=" + QString(duid) + "&");
}

bool QmlBackend::handlePsnLoginRedirect(const QUrl &url)
{
    if (!url.toString().startsWith(QString::fromStdString(PSNAuth::REDIRECT_PAGE)))
    {
        emit psnLoginAccountIdError(QString("Redirect URL invalid does not start with:\n") + QString::fromStdString(PSNAuth::REDIRECT_PAGE));
        return false;
    }

    const QString code = QUrlQuery(url).queryItemValue("code");
    if (code.isEmpty()) {
        qCWarning(chiakiGui) << "Invalid code from redirect url";
        emit psnLoginAccountIdError("Redirect URL invalid");
        return false;
    }
    PSNAccountID *psnId = new PSNAccountID(settings, this);
    connect(psnId, &PSNAccountID::AccountIDResponse, this, [this, psnId](const QString &accountId) {
        emit psnLoginAccountIdDone(accountId);
    });
    connect(psnId, &PSNAccountID::AccountIDResponse, this, &QmlBackend::updatePsnHosts);
    connect(psnId, &PSNAccountID::AccountIDError, [this](const QString &error) {
        qCWarning(chiakiGui) << "Could not retrieve psn token or account Id!" << error;
        emit psnLoginAccountIdError(error);
    });
    connect(psnId, &PSNAccountID::Finished, psnId, &QObject::deleteLater);
    psnId->GetPsnAccountId(code);
    emit psnTokenChanged();
    return true;
}

void QmlBackend::stopAutoConnect()
{
    auto_connect_mac = {};
    if(!wakeup_nickname.isEmpty())
    {
        wakeup_start_timer->stop();
        wakeup_repeat_timer->stop();
        wakeup_host_addr.clear();
        wakeup_regist_key.clear();
        wakeup_nickname.clear();
        if(wakeup_start)
        {
            wakeup_start = false;
            chiaki_log_mutex.lock();
            chiaki_log_ctx = nullptr;
            chiaki_log_mutex.unlock();

            session->deleteLater();
            session = nullptr;
            startSubscriptionExpiryTimer();
        }
    }
    emit autoConnectChanged();
}

QmlBackend::DisplayServer QmlBackend::displayServerAt(int index) const
{
    if (index < 0)
        return {};
    auto discovered = discovery_manager.GetHosts();
    auto manual = settings->GetManualHosts();
    if (index < discovered.size()) {
        DisplayServer server;
        server.valid = true;
        server.discovered = true;
        server.discovery_host = discovered.at(index);
        auto host_mac = server.discovery_host.GetHostMAC();
        server.registered = settings->GetRegisteredHostRegistered(host_mac);
        QString duid = "";
        if(!server.registered)
        {
            if(psn_nickname_hosts.contains(server.discovery_host.host_name))
                duid = psn_nickname_hosts.value(server.discovery_host.host_name).GetDuid();
            else if(!server.discovery_host.ps5)
                duid =  psn_nickname_hosts.value(QString("Main PS4 Console")).GetDuid();
        }
        server.duid = std::move(duid);
        if (server.registered)
            server.registered_host = settings->GetRegisteredHost(host_mac);
        for (int i = 0; i < manual.size(); i++)
        {
            const auto &manual_host = manual.at(i);
            if(manual_host.GetRegistered() && manual_host.GetMAC() == host_mac && manual_host.GetHost() == server.discovery_host.host_addr)
            {
                server.manual_host = std::move(manual_host);
                break;
            }
        }
        return server;
    }
    index -= discovered.size();
    if (index < manual.size()) {
        DisplayServer server;
        server.valid = true;
        server.discovered = false;
        server.manual_host = manual.at(index);
        server.registered = false;
        server.duid = QString();
        if (server.manual_host.GetRegistered() && settings->GetRegisteredHostRegistered(server.manual_host.GetMAC())) {
            server.registered = true;
            server.registered_host = settings->GetRegisteredHost(server.manual_host.GetMAC());
        }
        return server;
    }
    index -= manual.size();
    if (index < psn_hosts.count())
    {
        DisplayServer server;

        QMapIterator<QString, PsnHost> i(psn_hosts);
        size_t j = 0;
        while (i.hasNext())
        {
            i.next();
            PsnHost psn_host = i.value();
            bool hidden = false;
            for (const auto &host : discovery_manager.GetHosts())
            {
                if(host.host_name == psn_host.GetName())
                    hidden = true;
            }
            if(hidden)
                continue;
            if(j == index)
            {
                server.valid = true;
                server.discovered = false;
                server.psn_host = std::move(psn_host);
                server.duid = i.key();
                server.registered = true;
                server.registered_host = settings->GetNicknameRegisteredHost(server.psn_host.GetName());
                return server;
            }
            j++;
        }
        return {};
    }
    return {};
}

bool QmlBackend::sendWakeup(const DisplayServer &server)
{
    if (!server.registered)
        return false;
    return sendWakeup(server.GetHostAddr(), server.registered_host.GetRPRegistKey(), server.IsPS5());
}

bool QmlBackend::sendWakeup(const QString &host, const QByteArray &regist_key, bool ps5)
{
    try {
        uint16_t jwt_port = settings->GetJwtPort();
        uint16_t wakeup_port = (jwt_port == 0) ? 0 : (ps5 ? jwt_port : (jwt_port >= 4000 ? static_cast<uint16_t>(jwt_port - 4000) : 0));
        qCInfo(chiakiGui) << "[4cloud wakeup] host:" << host << "port:" << wakeup_port << "ps5:" << ps5;
        discovery_manager.SendWakeup(host, regist_key, ps5, wakeup_port);
        return true;
    } catch (const Exception &e) {
        CrashReporter::SendExceptionReport("Exception", e.what());
        emit error(tr("Wakeup failed"), tr("Failed to send Wakeup packet:\n%1").arg(e.what()));
        return false;
    }
}

void QmlBackend::fetchFourcloudState()
{
    QString nps4 = settings->GetNps4();
    if (nps4.isEmpty()) {
        clearFourcloudState();
        return;
    }
    if (!network_manager)
        network_manager = new QNetworkAccessManager(this);
    QUrl statusUrl("https://api.4cloud.pro/status_console.php");
    QUrlQuery statusQuery;
    statusQuery.addQueryItem("NPS4", nps4);
    statusUrl.setQuery(statusQuery);
    qCInfo(chiakiGui) << "[4cloud state poll] request URL:" << statusUrl.toString();
    QNetworkRequest statusRequest(statusUrl);
    QNetworkReply *reply = network_manager->get(statusRequest);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (settings->GetNps4().isEmpty()) {
            reply->deleteLater();
            clearFourcloudState();
            return;
        }
        int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        auto err = reply->error();
        QByteArray body = reply->readAll();
        reply->deleteLater();
        qCInfo(chiakiGui) << "[4cloud state poll] response httpCode:" << httpCode << "error:" << err << "bodySize:" << body.size();
        QString status = parseFourcloudStatusBody(body);
        if (status == QStringLiteral("unknown")) {
            if (!fourcloud_state_retrying) {
                fourcloud_state_retrying = true;
                qCInfo(chiakiGui) << "[4cloud state poll] got offline, scheduling retry in 2.5s";
                QTimer::singleShot(2500, this, [this]() { fetchFourcloudState(); });
                emit hostsChanged();  // показать «Проверка…»
                reply->deleteLater();
                return;
            }
        } else {
            fourcloud_state_retrying = false;
        }
        fourcloud_state_cache = status;
        qCInfo(chiakiGui) << "[4cloud state poll] cache set to:" << (status.isEmpty() ? "empty" : status);
        emit hostsChanged();
    });
}

void QmlBackend::ensureFourcloudPolling()
{
    resolveCloudBillingIdentity();
    if (settings->GetNps4().isEmpty())
        return;
    if (!fourcloud_state_timer || fourcloud_state_timer->isActive())
        return;
    fetchFourcloudState();
    fourcloud_state_timer->start(15000);
}

void QmlBackend::resolveCloudBillingIdentity()
{
    if (!settings || !settings->GetCloudBillingEnabled())
        return;
    const QString email = settings->GetFourCloudEmail().trimmed();
    const QString host = settings->GetCloudBillingHost();
    if (email.isEmpty() || host.isEmpty())
        return;
    if (settings->GetCloudBillingUserId() > 0)
        return;

    const quint16 port = settings->GetCloudBillingPort();
    auto *watcher = new QFutureWatcher<CloudBillingClient::Result>(this);
    connect(watcher, &QFutureWatcher<CloudBillingClient::Result>::finished, this, [this, watcher]() {
        const CloudBillingClient::Result who = watcher->result();
        watcher->deleteLater();
        if (!who.ok || !settings)
            return;
        const qint64 uid = who.data.value(QStringLiteral("user_id")).toVariant().toLongLong();
        if (uid <= 0)
            return;
        settings->SetCloudBillingUserId(uid);
        if (settings_qml)
            settings_qml->refreshCloudBillingUserId();
        qCInfo(chiakiGui) << "Cloud billing UID resolved:" << uid;
    });
    watcher->setFuture(QtConcurrent::run([host, port, email]() {
        return CloudBillingClient::whoami(host, port, email);
    }));
}

void QmlBackend::logoutFourcloud()
{
    if (settings) {
        settings->SetJwtToken("");
        settings->SetJwtPort(0);
        settings->SetNps4("");
        settings->SetSubscriptionExpiryDate("");
        settings->SetCloudBillingUserId(0);
        settings->SetFourCloudEmail("");
        clearAuthEntitlements();
    }
    if (settings_qml) {
        settings_qml->refreshCloudBillingUserId();
        settings_qml->refreshFourCloudEmail();
    }
    if (cloud_catalog_backend)
        cloud_catalog_backend->invalidateCache();
    clearFourcloudState();
    emit jwtTokenExpired();
}

bool QmlBackend::showConsoleCatalogTab() const
{
    return settings && settings->GetConsoleCatalogAccess();
}

bool QmlBackend::showCloudGamesTab() const
{
    return settings && settings->GetCloudGamesAccess();
}

void QmlBackend::clearAuthEntitlements()
{
    if (!settings)
        return;
    const bool changed = settings->GetConsoleCatalogAccess() || settings->GetCloudGamesAccess();
    settings->SetConsoleCatalogAccess(false);
    settings->SetCloudGamesAccess(false);
    if (changed)
        emit authEntitlementsChanged();
}

void QmlBackend::applyAuthSession(const QJsonObject &session, bool from_login)
{
    const QString jwt = session.value(QStringLiteral("jwt")).toString().trimmed();
    if (jwt.isEmpty()) {
        if (from_login)
            emit authenticationError(QStringLiteral("Ответ не содержит JWT токена"));
        else {
            settings->SetJwtToken("");
            settings->SetJwtPort(0);
            settings->SetNps4("");
            clearAuthEntitlements();
            clearFourcloudState();
            emit jwtTokenExpired();
        }
        return;
    }

    const bool console_access = session.value(QStringLiteral("console_access")).toBool();
    const bool cloud_access = session.value(QStringLiteral("cloud_access")).toBool();
    if (!console_access && !cloud_access) {
        settings->SetJwtToken("");
        settings->SetJwtPort(0);
        settings->SetNps4("");
        settings->SetSubscriptionExpiryDate("");
        clearAuthEntitlements();
        clearFourcloudState();
        if (from_login)
            emit authenticationError(QStringLiteral("Нет активной подписки"));
        else
            emit subscriptionExpired(QStringLiteral("Нет активной подписки"));
        return;
    }

    settings->SetJwtToken(jwt);
    settings->SetConsoleCatalogAccess(console_access);
    settings->SetCloudGamesAccess(cloud_access);
    emit authEntitlementsChanged();

    QJsonObject decodeObj = session.value(QStringLiteral("decode")).toObject();
    if (decodeObj.isEmpty())
        decodeObj = session;

    syncFourCloudEmailFromJwtDecode(settings, settings_qml, cloud_catalog_backend, decodeObj);

    QString email = session.value(QStringLiteral("email")).toString().trimmed();
    if (email.isEmpty())
        email = decodeObj.value(QStringLiteral("Email")).toString().trimmed();
    if (email.isEmpty())
        email = decodeObj.value(QStringLiteral("email")).toString().trimmed();
    if (!email.isEmpty()) {
        email = email.toLower();
        settings->SetFourCloudEmail(email);
        if (settings_qml)
            settings_qml->refreshFourCloudEmail();
        if (cloud_catalog_backend)
            cloud_catalog_backend->invalidateCache();
    }

    int portVal = session.value(QStringLiteral("Port")).toInt(0);
    if (portVal <= 0)
        portVal = decodeObj.value(QStringLiteral("Port")).toInt(0);
    settings->SetJwtPort((portVal > 0 && portVal <= 65535) ? static_cast<uint16_t>(portVal) : 0);
    if (settings->GetJwtPort() != 0)
        discovery_manager.RefreshManualServices();

    QString nps4 = session.value(QStringLiteral("NP")).toString().trimmed();
    if (nps4.isEmpty())
        nps4 = session.value(QStringLiteral("NPS4")).toString().trimmed();
    if (nps4.isEmpty())
        nps4 = decodeObj.value(QStringLiteral("NP")).toString().trimmed();
    if (nps4.isEmpty())
        nps4 = decodeObj.value(QStringLiteral("NPS4")).toString().trimmed();
    settings->SetNps4(console_access ? nps4 : QString());

    QString jwt_psn = session.value(QStringLiteral("PSN")).toString().trimmed();
    if (jwt_psn.isEmpty())
        jwt_psn = decodeObj.value(QStringLiteral("PSN")).toString().trimmed();
    settings->SetJwtPsn(console_access ? jwt_psn : QString());

    QString dateExp = session.value(QStringLiteral("Date_exp")).toString().trimmed();
    if (dateExp.isEmpty())
        dateExp = decodeObj.value(QStringLiteral("Date_exp")).toString().trimmed();
    if (console_access && !dateExp.isEmpty() && dateExp.toLower() != QLatin1String("null"))
        settings->SetSubscriptionExpiryDate(dateExp);
    else
        settings->SetSubscriptionExpiryDate(QString());

    if (console_access && !settings->GetNps4().isEmpty()) {
        fetchFourcloudState();
        if (fourcloud_state_timer)
            fourcloud_state_timer->start(15000);
    } else {
        clearFourcloudState();
    }

    fetchYandexIamByJwt(jwt);
    if (console_access)
        startSubscriptionExpiryTimer();
    else if (subscription_expiry_timer && subscription_expiry_timer->isActive())
        subscription_expiry_timer->stop();

    auto finishOk = [this, from_login]() {
        resolveCloudBillingIdentity();
        if (from_login) {
            qCInfo(chiakiGui) << "Authentication successful via UDP auth"
                              << "console=" << settings->GetConsoleCatalogAccess()
                              << "cloud=" << settings->GetCloudGamesAccess()
                              << "email=" << settings->GetFourCloudEmail();
            emit authenticationSuccess();
        } else {
            qCInfo(chiakiGui) << "JWT session valid via UDP auth"
                              << "console=" << settings->GetConsoleCatalogAccess()
                              << "cloud=" << settings->GetCloudGamesAccess()
                              << "email=" << settings->GetFourCloudEmail();
            emit jwtTokenValid();
        }
    };

    QString chiaki_url = session.value(QStringLiteral("chiaki_url")).toString().trimmed();
    if (chiaki_url.isEmpty())
        chiaki_url = decodeObj.value(QStringLiteral("chiaki_url")).toString().trimmed();

    if (!from_login || !console_access || chiaki_url.isEmpty()) {
        finishOk();
        return;
    }

    QString lastLoadedNps4 = settings->GetLastLoadedNps4();
    QString currentNps4 = settings->GetNps4();
    bool consoleChanged = !currentNps4.isEmpty()
        && (lastLoadedNps4.isEmpty() || (currentNps4 != lastLoadedNps4));
    bool needLoadConfig = chiaki_url != settings->GetLastLoadedChiakiConfigUrl() || consoleChanged;
    if (!needLoadConfig) {
        finishOk();
        return;
    }

    if (!network_manager)
        network_manager = new QNetworkAccessManager(this);

    QString jwtToRestore = settings->GetJwtToken();
    uint16_t portToRestore = settings->GetJwtPort();
    QString nps4ToRestore = settings->GetNps4();
    QString jwtPsnToRestore = settings->GetJwtPsn();
    QString subscriptionExpiryToRestore = settings->GetSubscriptionExpiryDate();
    QString emailToRestore = settings->GetFourCloudEmail();
    const bool consoleRestore = settings->GetConsoleCatalogAccess();
    const bool cloudRestore = settings->GetCloudGamesAccess();

    qCInfo(chiakiGui) << "Loading chiaki config from:" << chiaki_url;
    QNetworkRequest configRequest{QUrl(chiaki_url)};
    QNetworkReply *configReply = network_manager->get(configRequest);
    connect(configReply, &QNetworkReply::finished, this, [this, configReply, chiaki_url, jwtToRestore,
            portToRestore, nps4ToRestore, jwtPsnToRestore, subscriptionExpiryToRestore,
            emailToRestore, consoleRestore, cloudRestore, finishOk]() {
        configReply->deleteLater();
        if (configReply->error() != QNetworkReply::NoError) {
            qCWarning(chiakiGui) << "Failed to download chiaki config:" << configReply->errorString();
            finishOk();
            return;
        }
        QByteArray configData = configReply->readAll();
        QTemporaryFile tempFile;
        if (!tempFile.open()) {
            qCWarning(chiakiGui) << "Failed to create temp file for config";
            finishOk();
            return;
        }
        tempFile.write(configData);
        tempFile.flush();
        settings->ImportSettings(tempFile.fileName());
        settings->SetLastLoadedChiakiConfigUrl(chiaki_url);
        settings->SetLastLoadedNps4(nps4ToRestore);
        settings->SetJwtToken(jwtToRestore);
        settings->SetJwtPort(portToRestore);
        settings->SetNps4(nps4ToRestore);
        settings->SetJwtPsn(jwtPsnToRestore);
        settings->SetSubscriptionExpiryDate(subscriptionExpiryToRestore);
        settings->SetConsoleCatalogAccess(consoleRestore);
        settings->SetCloudGamesAccess(cloudRestore);
        // ImportSettings() clears QSettings — restore 4cloud identity after import.
        settings->SetFourCloudEmail(emailToRestore);
        settings->SetCloudBillingUserId(0);
        if (settings_qml) {
            settings_qml->refreshFourCloudEmail();
            settings_qml->refreshCloudBillingUserId();
        }
        settings->SetHardwareDecoder("d3d11va");
        emit authEntitlementsChanged();
        qCInfo(chiakiGui) << "Chiaki config imported successfully, email restored:" << emailToRestore;
        finishOk();
    });
}

void QmlBackend::fetchYandexIamByJwt(const QString &jwt)
{
    if (jwt.isEmpty())
        return;
    if (!network_manager)
        network_manager = new QNetworkAccessManager(this);
    QUrl url("https://4cloud.pro/api.php");
    QUrlQuery query;
    query.addQueryItem("method", "get-iam-token-by-jwt");
    query.addQueryItem("jwt", jwt);
    url.setQuery(query);
    qCInfo(chiakiGui) << "Yandex IAM by JWT request (translator)";
    QNetworkRequest request(url);
    QNetworkReply *reply = network_manager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        QByteArray data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError && data.isEmpty()) {
            qCInfo(chiakiGui) << "Yandex IAM by JWT: network error, skipping";
            return;
        }
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            qCInfo(chiakiGui) << "Yandex IAM by JWT: parse error, skipping";
            return;
        }
        QJsonObject obj = doc.object();
        if (obj.contains("error") && !obj.value("error").toString().trimmed().isEmpty()) {
            qCInfo(chiakiGui) << "Yandex IAM by JWT: API error, skipping";
            return;
        }
        QString status = obj.value("Status").toString();
        if (status.isEmpty())
            status = obj.value("status").toString();
        if (status.toLower() != "success") {
            qCInfo(chiakiGui) << "Yandex IAM by JWT: status not success, skipping";
            return;
        }
        QString iamToken = obj.value("Key").toString();
        QString folderId = obj.value("Folder").toString();
        if (iamToken.isEmpty() || folderId.isEmpty()) {
            qCInfo(chiakiGui) << "Yandex IAM by JWT: missing Key/Folder, skipping";
            return;
        }
        settings->SetYandexIamToken(iamToken);
        settings->SetYandexFolderId(folderId);
        qCInfo(chiakiGui) << "Yandex IAM by JWT: token updated for translator";
    });
}

void QmlBackend::clearFourcloudState()
{
    fourcloud_state_cache.clear();
    fourcloud_state_retrying = false;
    if (settings)
        settings->SetJwtPsn("");
    if (fourcloud_state_timer && fourcloud_state_timer->isActive())
        fourcloud_state_timer->stop();
    if (subscription_expiry_timer && subscription_expiry_timer->isActive())
        subscription_expiry_timer->stop();
    subscription_time_remaining.clear();
    emit subscriptionTimeRemainingChanged();
    emit hostsChanged();
}

void QmlBackend::fetchSubscriptionExpiry()
{
    QString jwt = settings->GetJwtToken();
    if (jwt.isEmpty()) {
        subscription_time_remaining.clear();
        emit subscriptionTimeRemainingChanged();
        return;
    }
    if (!network_manager)
        network_manager = new QNetworkAccessManager(this);
    QUrl url("https://4cloud.pro/api.php");
    QUrlQuery q;
    q.addQueryItem("method", "get-date-exp-jwt");
    q.addQueryItem("jwt", jwt);
    url.setQuery(q);
    QNetworkRequest req(url);
    QNetworkReply *reply = network_manager->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (settings->GetJwtToken().isEmpty())
            return;
        QByteArray body = reply->readAll();
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error != QJsonParseError::NoError || !doc.isArray()) {
            subscription_time_remaining.clear();
            emit subscriptionTimeRemainingChanged();
            return;
        }
        QJsonArray arr = doc.array();
        if (arr.isEmpty()) {
            subscription_time_remaining.clear();
            emit subscriptionTimeRemainingChanged();
            return;
        }
        QJsonObject item = arr[0].toObject();
        QString dateStr = item.value("Date").toString().trimmed();
        if (dateStr.compare("Error", Qt::CaseInsensitive) == 0) {
            // Нет console Date_exp — для cloud-only это нормально, JWT не трогаем.
            subscription_time_remaining.clear();
            emit subscriptionTimeRemainingChanged();
            if (settings->GetCloudGamesAccess())
                return;
            settings->SetJwtToken("");
            settings->SetJwtPort(0);
            settings->SetNps4("");
            clearFourcloudState();
            emit subscriptionExpired("Нет активной подписки");
            emit jwtTokenExpired();
            return;
        }
        QString nowStr = item.value("Now").toString().trimmed();
        if (nowStr.isEmpty()) {
            subscription_time_remaining.clear();
            emit subscriptionTimeRemainingChanged();
            return;
        }
        // Date: "31.03.2026 23:00", Now: "2026-03-09 14:36:43"
        QDateTime expiry = QDateTime::fromString(dateStr, "dd.MM.yyyy HH:mm");
        QDateTime now = QDateTime::fromString(nowStr, "yyyy-MM-dd HH:mm:ss");
        if (!expiry.isValid() || !now.isValid()) {
            subscription_time_remaining.clear();
            emit subscriptionTimeRemainingChanged();
            return;
        }
        if (now >= expiry) {
            subscription_time_remaining.clear();
            emit subscriptionTimeRemainingChanged();
            if (settings->GetCloudGamesAccess()) {
                // Console rental expired, but cloud access remains — keep JWT.
                settings->SetJwtPort(0);
                settings->SetNps4("");
                settings->SetSubscriptionExpiryDate("");
                clearFourcloudState();
                return;
            }
            settings->SetJwtToken("");
            settings->SetJwtPort(0); settings->SetNps4(""); clearFourcloudState();
            emit subscriptionExpired("Срок подписки истёк");
            emit jwtTokenExpired();
            return;
        }
        qint64 secs = now.secsTo(expiry);
        int days = static_cast<int>(secs / 86400);
        int hours = static_cast<int>((secs % 86400) / 3600);
        int mins = static_cast<int>((secs % 3600) / 60);
        QStringList parts;
        if (days > 0)
            parts << QString::number(days) + " дн.";
        if (hours > 0)
            parts << QString::number(hours) + " ч.";
        if (mins > 0 || parts.isEmpty())
            parts << QString::number(mins) + " мин.";
        subscription_time_remaining = parts.join(" ");
        emit subscriptionTimeRemainingChanged();
    });
}

void QmlBackend::startSubscriptionExpiryTimer()
{
    if (settings->GetJwtToken().isEmpty())
        return;
    if (subscription_expiry_timer) {
        subscription_expiry_timer->start(60000);
        fetchSubscriptionExpiry();
    }
}

void QmlBackend::setAllowJoystickBackgroundEvents()
{
    ControllerManager::GetInstance()->SetAllowJoystickBackgroundEvents(settings->GetAllowJoystickBackgroundEvents());
}

void QmlBackend::setIsAppActive()
{
    ControllerManager::GetInstance()->SetIsAppActive(window->isActive());
}

uint32_t QmlBackend::getStreamShortcut() const
{
    if(!settings->GetStreamMenuEnabled())
        return 0;
	uint32_t stream_menu_shortcut1 = settings->GetStreamMenuShortcut1();
	if(stream_menu_shortcut1 > 0)
		stream_menu_shortcut1 = 1 << (stream_menu_shortcut1 - 1);
	uint32_t stream_menu_shortcut2 = settings->GetStreamMenuShortcut2();
	if(stream_menu_shortcut2 > 0)
		stream_menu_shortcut2 = 1 << (stream_menu_shortcut2 - 1);
	uint32_t stream_menu_shortcut3 = settings->GetStreamMenuShortcut3();
	if(stream_menu_shortcut3 > 0)
		stream_menu_shortcut3 = 1 << (stream_menu_shortcut3 - 1);
	uint32_t stream_menu_shortcut4 = settings->GetStreamMenuShortcut4();
	if(stream_menu_shortcut4 > 0)
		stream_menu_shortcut4 = 1 << (stream_menu_shortcut4 - 1);
    uint32_t shortcut = stream_menu_shortcut1 | stream_menu_shortcut2 | stream_menu_shortcut3 | stream_menu_shortcut4;
    return shortcut;
}

void QmlBackend::updateStreamShortcut()
{
    // While streaming, fullscreen chord is handled in StreamSession to avoid
    // double-toggle with synthetic F11 from QmlController.
    uint32_t shortcut = session ? 0u : getStreamShortcut();
    for (const auto &controller : std::as_const(controllers)) {
       controller->setEscapeShortcut(shortcut);
    }
}

void QmlBackend::updateControllers()
{
    bool changed = false;
    QMap<QString,QString> controller_mappings = settings->GetControllerMappings();
    for (auto it = controllers.begin(); it != controllers.end();) {
        if (ControllerManager::GetInstance()->GetAvailableControllers().contains(it.key())) {
            it++;
            continue;
        }
        if(it.key() == controller_mapping_id)
        {
            controller_mapping_controller = nullptr;
            controllerMappingQuit();
        }
        QString vidpid = it.value()->GetVIDPID();
        QString guid = it.value()->GetGUID();
        it.value()->deleteLater();
        it = controllers.erase(it);
        changed = true;
    }
    uint32_t stream_shortcut = getStreamShortcut();
    for (auto id : ControllerManager::GetInstance()->GetAvailableControllers()) {
        if (controllers.contains(id))
            continue;
        auto controller = ControllerManager::GetInstance()->OpenController(id);
        if (!controller)
            continue;
        controllers[id] = new QmlController(controller, stream_shortcut ,window, this);
        QString vidpid = controller->GetVIDPIDString();
        QString guid = controller->GetGUIDString();
        QStringList existing_vidpid;
        if(controller_guids_to_update.contains(vidpid))
            existing_vidpid.append(controller_guids_to_update.value(vidpid));
        existing_vidpid.append(guid);
        controller_guids_to_update.insert(vidpid, existing_vidpid);
        // replace old guid with new vid/pid
        if(controller_mappings.contains(guid))
        {
            QString old_mapping = controller_mappings.value(guid);
            qsizetype guid_string_length = old_mapping.indexOf(",") + 1;
            old_mapping.remove(0, guid_string_length);
            settings->RemoveControllerMapping(guid);
            settings->SetControllerMapping(vidpid, old_mapping);
            qCInfo(chiakiGui) << "Migrated controller mapping from platform-specific GUID: " << guid << " to platform-agnostic VID/PID: " << vidpid;
        }
        controller_guids_to_update.insert(vidpid, existing_vidpid);
        connect(controller, &Controller::UpdatingControllerMapping, this, &QmlBackend::controllerMappingUpdate);
        connect(controller, &Controller::NewButtonMapping, this, &QmlBackend::controllerMappingChangeButton);
        changed = true;
    }
    if (changed)
        emit controllersChanged();
}

void QmlBackend::setControllerMappingDefaultMapping(bool is_default_mapping)
{
    if(controller_mapping_default_mapping != is_default_mapping)
    {
        controller_mapping_default_mapping = is_default_mapping;
        emit controllerMappingDefaultMappingChanged();
    }
}

void QmlBackend::setControllerMappingAltered(bool altered)
{
    if(controller_mapping_altered != altered)
    {
        controller_mapping_altered = altered;
        emit controllerMappingAlteredChanged();
    }
}

void QmlBackend::setControllerMappingInProgress(bool is_in_progress)
{
    if(controller_mapping_in_progress != is_in_progress)
    {
        controller_mapping_in_progress = is_in_progress;
        emit controllerMappingInProgressChanged();
    }
}

void QmlBackend::setEnableAnalogStickMapping(bool enabled)
{
    if(enable_analog_stick_mapping != enabled)
    {
        if(controller_mapping_in_progress && controller_mapping_controller)
            controller_mapping_controller->EnableAnalogStickMapping(enabled);
        enable_analog_stick_mapping = enabled;
        emit enableAnalogStickMappingChanged();
    }
}

void QmlBackend::setShowPingTimeoutDialog(bool show)
{
    if(show_ping_timeout_dialog != show)
    {
        show_ping_timeout_dialog = show;
        emit showPingTimeoutDialogChanged();
    }
}

void QmlBackend::setShowAuthorizationFailedDialog(bool show)
{
    if(show_authorization_failed_dialog != show)
    {
        show_authorization_failed_dialog = show;
        emit showAuthorizationFailedDialogChanged();
    }
}

void QmlBackend::setShowPSPlusSubscriptionDialog(bool show)
{
    if(show_ps_plus_subscription_dialog != show)
    {
        show_ps_plus_subscription_dialog = show;
        emit showPSPlusSubscriptionDialogChanged();
    }
}

void QmlBackend::setShowAccountPrivacySettingsDialog(bool show)
{
    if(show_account_privacy_settings_dialog != show)
    {
        show_account_privacy_settings_dialog = show;
        emit showAccountPrivacySettingsDialogChanged();
    }
}

void QmlBackend::setAccountPrivacyUpgradeUrl(const QString &url)
{
    if(account_privacy_upgrade_url != url)
    {
        account_privacy_upgrade_url = url;
        emit accountPrivacyUpgradeUrlChanged();
    }
}

QVariantList QmlBackend::currentControllerMapping() const
{
    QVariantList out;
    if(!controller_mapping_in_progress)
        return out;
	QMap<int, QStringList> result =
	{
		{CHIAKI_CONTROLLER_BUTTON_CROSS     , controller_mapping_controller_mappings.contains("a") ? controller_mapping_controller_mappings.value("a") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_MOON      , controller_mapping_controller_mappings.contains("b") ? controller_mapping_controller_mappings.value("b") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_BOX       , controller_mapping_controller_mappings.contains("x") ? controller_mapping_controller_mappings.value("x") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_PYRAMID   , controller_mapping_controller_mappings.contains("y") ? controller_mapping_controller_mappings.value("y") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT , controller_mapping_controller_mappings.contains("dpleft") ? controller_mapping_controller_mappings.value("dpleft") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT, controller_mapping_controller_mappings.contains("dpright") ? controller_mapping_controller_mappings.value("dpright") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_DPAD_UP   , controller_mapping_controller_mappings.contains("dpup") ? controller_mapping_controller_mappings.value("dpup") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN , controller_mapping_controller_mappings.contains("dpdown") ? controller_mapping_controller_mappings.value("dpdown") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_L1        , controller_mapping_controller_mappings.contains("leftshoulder") ? controller_mapping_controller_mappings.value("leftshoulder") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_R1        , controller_mapping_controller_mappings.contains("rightshoulder") ? controller_mapping_controller_mappings.value("rightshoulder") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_L3        , controller_mapping_controller_mappings.contains("leftstick") ? controller_mapping_controller_mappings.value("leftstick") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_R3        , controller_mapping_controller_mappings.contains("rightstick") ? controller_mapping_controller_mappings.value("rightstick") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_OPTIONS   , controller_mapping_controller_mappings.contains("start") ? controller_mapping_controller_mappings.value("start") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_SHARE     , controller_mapping_controller_mappings.contains("back") ? controller_mapping_controller_mappings.value("back") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_TOUCHPAD  , controller_mapping_controller_mappings.contains("touchpad") ? controller_mapping_controller_mappings.value("touchpad") : QStringList()},
		{CHIAKI_CONTROLLER_BUTTON_PS        , controller_mapping_controller_mappings.contains("guide") ? controller_mapping_controller_mappings.value("guide") : QStringList()},
		{CHIAKI_CONTROLLER_ANALOG_BUTTON_L2 , controller_mapping_controller_mappings.contains("lefttrigger") ? controller_mapping_controller_mappings.value("lefttrigger") : QStringList()},
		{CHIAKI_CONTROLLER_ANALOG_BUTTON_R2 , controller_mapping_controller_mappings.contains("righttrigger") ? controller_mapping_controller_mappings.value("righttrigger") : QStringList()},
		{static_cast<int>(ControllerButtonExt::ANALOG_STICK_LEFT_X)   , controller_mapping_controller_mappings.contains("leftx") ? controller_mapping_controller_mappings.value("leftx") : QStringList()},
		{static_cast<int>(ControllerButtonExt::ANALOG_STICK_LEFT_Y)   , controller_mapping_controller_mappings.contains("lefty") ? controller_mapping_controller_mappings.value("lefty") : QStringList()},
		{static_cast<int>(ControllerButtonExt::ANALOG_STICK_RIGHT_X)  , controller_mapping_controller_mappings.contains("rightx") ? controller_mapping_controller_mappings.value("rightx") : QStringList()},
		{static_cast<int>(ControllerButtonExt::ANALOG_STICK_RIGHT_Y)  , controller_mapping_controller_mappings.contains("righty") ? controller_mapping_controller_mappings.value("righty") : QStringList()},
        {static_cast<int>(ControllerButtonExt::MISC1)   ,      controller_mapping_controller_mappings.contains("misc1") ? controller_mapping_controller_mappings.value("misc1") : QStringList()},
	};

    for (auto it = result.cbegin(); it != result.cend(); ++it) {
        QVariantMap m;
        m["buttonName"] = Settings::GetChiakiControllerButtonName(it.key());
        m["buttonValue"] = it.key();
        m["physicalButton"] = it.value();
        out.append(m);
    }
    return out;
}

void QmlBackend::updateControllerMappings()
{
    if(SDL_WasInit(SDL_INIT_GAMECONTROLLER)==0)
        return;
    QMap<QString,QString> controller_mappings = settings->GetControllerMappings();
    QStringList mapping_vidpids = controller_mappings.keys();
    for(int i=0; i<mapping_vidpids.length(); i++)
    {
        QString vidpid = mapping_vidpids.at(i);
        if(!controller_guids_to_update.contains(vidpid))
            continue;
        QStringList guids_to_update = controller_guids_to_update.value(vidpid);
        for(int j=0; j<guids_to_update.length(); j++)
        {
            QString guid = guids_to_update.at(j);
            if(!controller_mapping_original_controller_mappings.contains(guid))
            {
                const SDL_JoystickGUID real_guid = SDL_JoystickGetGUIDFromString(guid.toUtf8().constData());
                const char *mapping = SDL_GameControllerMappingForGUID(real_guid);
                QString original_controller_mapping(mapping);
                SDL_free((char *)mapping);
                if(original_controller_mapping.isEmpty())
                {
                    qCWarning(chiakiGui) << "Error retrieving controller mapping of GUID " << guid << "with error: " << SDL_GetError();
                    return;
                }
                controller_mapping_original_controller_mappings.insert(guid, original_controller_mapping);
            }
            QString controller_mapping_to_add = controller_mappings.value(vidpid);
            controller_mapping_to_add.prepend(QString("%1,").arg(guid));
            int result = SDL_GameControllerAddMapping(controller_mapping_to_add.toUtf8().constData());
            switch(result)
            {
                case -1:
                    qCWarning(chiakiGui) << "Error setting controller mapping for guid: " << guid << " with error: " << SDL_GetError();
                    break;
                case 0:
                    qCInfo(chiakiGui) << "Updated controller mapping for guid: " << guid;
                    break;
                case 1:
                    qCInfo(chiakiGui) << "Added controller mapping for guid: " << guid;
                    break;
                default:
                    qCInfo(chiakiGui) << "Unidentified problem mapping for guid: " << guid;
                    break;
            }
        }
        controller_guids_to_update.remove(vidpid);
    }
}

void QmlBackend::creatingControllerMapping(bool creating_controller_mapping)
{
    ControllerManager::GetInstance()->creatingControllerMapping(creating_controller_mapping);
}

void QmlBackend::controllerMappingChangeButton(QString button)
{
    if(!controller_mapping_in_progress || !controller_mapping_controller)
        return;
    emit controllerMappingButtonSelected(std::move(button));
}

void QmlBackend::updateButton(int chiaki_button, QString physical_button, int new_index)
{
	QMap<int, QString> button_map =
	{
		{CHIAKI_CONTROLLER_BUTTON_CROSS     , "a"},
		{CHIAKI_CONTROLLER_BUTTON_MOON      , "b"},
		{CHIAKI_CONTROLLER_BUTTON_BOX       , "x"},
		{CHIAKI_CONTROLLER_BUTTON_PYRAMID   , "y"},
		{CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT , "dpleft"},
		{CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT, "dpright"},
		{CHIAKI_CONTROLLER_BUTTON_DPAD_UP   , "dpup"},
		{CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN , "dpdown"},
		{CHIAKI_CONTROLLER_BUTTON_L1        , "leftshoulder"},
		{CHIAKI_CONTROLLER_BUTTON_R1        , "rightshoulder"},
		{CHIAKI_CONTROLLER_BUTTON_L3        , "leftstick"},
		{CHIAKI_CONTROLLER_BUTTON_R3        , "rightstick"},
		{CHIAKI_CONTROLLER_BUTTON_OPTIONS   , "start"},
		{CHIAKI_CONTROLLER_BUTTON_SHARE     , "back"},
		{CHIAKI_CONTROLLER_BUTTON_TOUCHPAD  , "touchpad"},
		{CHIAKI_CONTROLLER_BUTTON_PS        , "guide"},
		{CHIAKI_CONTROLLER_ANALOG_BUTTON_L2 , "lefttrigger"},
		{CHIAKI_CONTROLLER_ANALOG_BUTTON_R2 , "righttrigger"},
		{static_cast<int>(ControllerButtonExt::ANALOG_STICK_LEFT_X)   , "leftx"},
		{static_cast<int>(ControllerButtonExt::ANALOG_STICK_LEFT_Y)   , "lefty"},
		{static_cast<int>(ControllerButtonExt::ANALOG_STICK_RIGHT_X)  , "rightx"},
		{static_cast<int>(ControllerButtonExt::ANALOG_STICK_RIGHT_Y)  , "righty"},
        {static_cast<int>(ControllerButtonExt::MISC1)   , "misc1"},
	};
    QString button = button_map.value(chiaki_button);
    QString old_mapping = controller_mapping_physical_button_mappings.contains(physical_button) ? controller_mapping_physical_button_mappings.value(physical_button) : QString();
    if(button == old_mapping)
        return;
    if(!old_mapping.isEmpty())
    {
        QStringList old_mapping_buttons = controller_mapping_controller_mappings.value(old_mapping);
        if(old_mapping_buttons.length() > 1)
        {
            old_mapping_buttons.removeOne(physical_button);
            controller_mapping_controller_mappings.insert(old_mapping, old_mapping_buttons);
        }
        else
            controller_mapping_controller_mappings.remove(old_mapping);
    }
    QStringList new_mapping_buttons = controller_mapping_controller_mappings.value(button);
    if(new_mapping_buttons.length() > new_index)
    {
        controller_mapping_physical_button_mappings.insert(new_mapping_buttons.at(new_index), QString());
        new_mapping_buttons.remove(new_index);
    }
    if(new_index == 0)
        new_mapping_buttons.prepend(physical_button);
    else
        new_mapping_buttons.append(physical_button);
    controller_mapping_controller_mappings.insert(button, new_mapping_buttons);
    controller_mapping_physical_button_mappings.insert(physical_button, button);
    if(controller_mapping_controller_mappings == controller_mapping_applied_controller_mappings)
        setControllerMappingAltered(false);
    else
        setControllerMappingAltered(true);
    emit currentControllerMappingChanged();
}

void QmlBackend::controllerMappingUpdate(Controller *controller)
{
    if(SDL_WasInit(SDL_INIT_GAMECONTROLLER)==0)
        return;
    if(controller_mapping_in_progress)
        return;
    controller_mapping_controller = controller;
    if(controller->IsSteamVirtual())
    {
        controllerMappingQuit();
        emit controllerMappingSteamControllerSelected();
        return;
    }
    controller_mapping_id = controller->GetDeviceID();
    const char *mapping = SDL_GameControllerMapping(controller->GetController());
    QString original_controller_mapping(mapping);
    qCInfo(chiakiGui) << "Original controller mapping: " << original_controller_mapping;
    SDL_free((char *)mapping);
    if(original_controller_mapping.isEmpty())
    {
        qCWarning(chiakiGui) << "Error retrieving controller mapping " << SDL_GetError();
        controller_mapping_id = -1;
        controller_mapping_controller = nullptr;
        return;
    }
	QStringList mapping_results = original_controller_mapping.split(u',');
    if(mapping_results.length() < 2)
    {
        qCWarning(chiakiGui) << "Received invalid Mapping List";
        controller_mapping_id = -1;
        controller_mapping_controller = nullptr;
        return;
    }
	controller_mapping_controller_guid = mapping_results.takeFirst();
    controller_mapping_controller_vid_pid = controller_mapping_controller->GetVIDPIDString();
    if(!controller_mapping_original_controller_mappings.contains(controller_mapping_controller_guid))
    {
        setControllerMappingDefaultMapping(true);
        controller_mapping_original_controller_mappings.insert(controller_mapping_controller_guid, original_controller_mapping);
    }
    controller_mapping_controller->EnableAnalogStickMapping(enable_analog_stick_mapping);
	controller_mapping_controller_type = mapping_results.takeFirst();
    if(controller_mapping_controller_type == "*")
    {
        controller_mapping_controller_type = controller_mapping_controller->GetType();
        if(controller_mapping_controller_type.isEmpty())
            controller_mapping_controller_type = QString("Unidentified Controller");
    }
    for(int i=0; i<mapping_results.length(); i++)
    {
        QString individual_mapping = mapping_results.at(i);
        QStringList individual_mapping_list = individual_mapping.split(u':');
        if(individual_mapping_list.length() < 2)
            continue;
        QString key = individual_mapping_list.takeFirst();
        if(controller_mapping_controller_mappings.contains(key))
        {
            auto update_list = controller_mapping_controller_mappings.value(key);
            individual_mapping_list = update_list + individual_mapping_list;
        }
        controller_mapping_controller_mappings.insert(key, individual_mapping_list);
        // only add buttons to physical mapping list
        if(key != "crc" && key != "platform" && key != "type" && key != "hint" && !key.startsWith("sdk"))
        {
            for(int j = 0; j < individual_mapping_list.length(); j++)
                controller_mapping_physical_button_mappings.insert(individual_mapping_list[j], key);
        }
    }

    controller_mapping_applied_controller_mappings = controller_mapping_controller_mappings;
    emit currentControllerTypeChanged();
    emit currentControllerMappingChanged();
    setControllerMappingInProgress(true);
}

void QmlBackend::controllerMappingSelectButton()
{
    if(controller_mapping_in_progress && controller_mapping_controller)
        controller_mapping_controller->IsUpdatingMappingButton(true);
}

void QmlBackend::controllerMappingReset()
{
    if(!controller_mapping_original_controller_mappings.contains(controller_mapping_controller_guid) || SDL_WasInit(SDL_INIT_GAMECONTROLLER)==0 || !settings->GetControllerMappings().keys().contains(controller_mapping_controller_vid_pid))
    {
        controllerMappingQuit();
        return;
    }
    settings->RemoveControllerMapping(controller_mapping_controller_vid_pid);
    int result = SDL_GameControllerAddMapping(controller_mapping_original_controller_mappings.value(controller_mapping_controller_guid).toUtf8().constData());
    switch(result)
    {
        case -1:
            qCWarning(chiakiGui) << "Error setting controller mapping for guid: " << controller_mapping_controller_guid << " with error: " << SDL_GetError();
            break;
        case 0:
            qCInfo(chiakiGui) << "Updated controller mapping for guid: " << controller_mapping_controller_guid;
            break;
        case 1:
            qCInfo(chiakiGui) << "Added controller mapping for guid: " << controller_mapping_controller_guid;
            break;
        default:
            qCInfo(chiakiGui) << "Unidentified problem mapping for guid: " << controller_mapping_controller_guid;
            break;
    }
    controllerMappingQuit();
}

void QmlBackend::controllerMappingQuit()
{
    if(controller_mapping_in_progress && controller_mapping_controller)
        controller_mapping_controller->IsUpdatingMappingButton(false);
    else
        creatingControllerMapping(false);
    setEnableAnalogStickMapping(false);
    controller_mapping_controller = nullptr;
    setControllerMappingDefaultMapping(false);
    setControllerMappingAltered(false);
    controller_mapping_id = -1;
    controller_mapping_controller_guid.clear();
    controller_mapping_controller_vid_pid.clear();
    controller_mapping_controller_type.clear();
    controller_mapping_controller_mappings.clear();
    controller_mapping_applied_controller_mappings.clear();
    controller_mapping_physical_button_mappings.clear();
    setControllerMappingInProgress(false);
    emit currentControllerTypeChanged();
    emit currentControllerMappingChanged();
}

void QmlBackend::controllerMappingButtonQuit()
{
    if(controller_mapping_in_progress && controller_mapping_controller)
        controller_mapping_controller->IsUpdatingMappingButton(false);
}

void QmlBackend::controllerMappingApply()
{
    QString new_controller_mapping = controller_mapping_controller_type;
    QMapIterator<QString, QStringList> i(controller_mapping_controller_mappings);
    while (i.hasNext()) {
        i.next();
        const auto &physical_buttons = i.value();
        for(int j = 0; j < physical_buttons.length(); j++)
            new_controller_mapping += "," + (i.key() + ":" + physical_buttons.at(j));
    }
    // if user actually reset to default manually, reset mapping, else add
    if(new_controller_mapping == controller_mapping_original_controller_mappings.value(controller_mapping_controller_guid))
    {
        settings->RemoveControllerMapping(controller_mapping_controller_vid_pid);
        int result = SDL_GameControllerAddMapping(controller_mapping_original_controller_mappings.value(controller_mapping_controller_guid).toUtf8().constData());
        switch(result)
        {
            case -1:
                qCWarning(chiakiGui) << "Error setting controller mapping for guid: " << controller_mapping_controller_guid << " with error: " << SDL_GetError();
                break;
            case 0:
                qCInfo(chiakiGui) << "Updated controller mapping for guid: " << controller_mapping_controller_guid;
                break;
            case 1:
                qCInfo(chiakiGui) << "Added controller mapping for guid: " << controller_mapping_controller_guid;
                break;
            default:
                qCInfo(chiakiGui) << "Unidentified problem mapping for guid: " << controller_mapping_controller_guid;
                break;
        }
    }
    else
    {
        QStringList existing_vidpid;
        if(controller_guids_to_update.contains(controller_mapping_controller_vid_pid))
            existing_vidpid.append(controller_guids_to_update.value(controller_mapping_controller_vid_pid));
        existing_vidpid.append(controller_mapping_controller_guid);
        controller_guids_to_update.insert(controller_mapping_controller_vid_pid, existing_vidpid);
        settings->SetControllerMapping(controller_mapping_controller_vid_pid, new_controller_mapping);
    }
}

void QmlBackend::updateDiscoveryHosts()
{
    if (autoConnect()) {
        const int hosts_count = discovery_manager.GetHosts().count();
        for (int i = 0; i < hosts_count; ++i) {
            if (discovery_manager.GetHosts().at(i).GetHostMAC() != auto_connect_mac)
                continue;
            psn_auto_connect_timer->stop();
            connectToHost(i, discovery_manager.GetHosts().at(i).host_name);
            break;
        }
    }
    emit hostsChanged();
}

#if CHIAKI_GUI_ENABLE_STEAM_SHORTCUT
QString QmlBackend::getExecutable() {
#if defined(Q_OS_LINUX)
    //Check for flatpak
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString flatpakId = env.value("FLATPAK_ID");
    QString appImagePath = env.value("APPIMAGE");
    if (!flatpakId.isEmpty()) {
        return QString("flatpak");
    }
    if (!appImagePath.isEmpty())
        return appImagePath;
#endif
    return QCoreApplication::applicationFilePath();
}

void QmlBackend::createSteamShortcut(QString shortcutName, QString launchOptions, const QJSValue &callback, QString steamDir)
{
    QJSValue cb = callback;
    QString controller_layout_workshop_id = "3049833406";
    QMap<QString, const QPixmap*> artwork;
    auto landscape = QPixmap(":/icons/steam_landscape.png");
    auto portrait = QPixmap(":/icons/steam_portrait.png");
    QImageReader reader;
    reader.setAllocationLimit(512);
    reader.setFileName(":/icons/steam_hero.png");
    auto hero = QPixmap::fromImageReader(&reader);
    auto icon = QPixmap(":/icons/steam_icon.png");
    auto logo = QPixmap(":/icons/steam_logo.png");
    artwork.insert("landscape", &landscape);
    artwork.insert("portrait", &portrait);
    artwork.insert("hero", &hero);
    artwork.insert("icon", &icon);
    artwork.insert("logo", &logo);
    
    auto infoLambda = [callback](const QString &infoMessage) {
        QJSValue icb = callback;
        if (icb.isCallable())
            icb.call({infoMessage, true, false});
    };

    auto errorLambda = [callback](const QString &errorMessage) {
        QJSValue icb = callback;
        if (icb.isCallable())
            icb.call({errorMessage, false, true});
    };
    SteamTools* steam_tools = new SteamTools(infoLambda, errorLambda, steamDir);
    bool steamExists = steam_tools->steamExists();
    if(!steamExists)
    {
        if (cb.isCallable())
            cb.call({QString("[E] Steam does not exist, cannot create Steam Shortcut"), false, true});
        delete steam_tools;
        return;
    }

    QString executable = getExecutable();
    if(executable == "flatpak")
    {
        const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QString flatpakId = env.value("FLATPAK_ID");
        launchOptions.prepend(QString("run %1 ").arg(flatpakId));
    }
    SteamShortcutEntry newShortcut = steam_tools->buildShortcutEntry(std::move(shortcutName), std::move(executable), std::move(launchOptions), std::move(artwork));

    QVector<SteamShortcutEntry> shortcuts = steam_tools->parseShortcuts();
    bool found = false;
    for (auto& map : shortcuts) {
        if (map.getExe() == newShortcut.getExe() && map.getLaunchOptions() == newShortcut.getLaunchOptions()) {
            // Replace the entire map with the new one
            
            if (cb.isCallable())
                cb.call({QString("[I] Updating Steam entry"), true, false});
            map = newShortcut;
            found = true;
            break;  // Stop iterating once a match is found
        }
    }

   //If we didn't find it to update, let's add it to the end
    if (!found) {
        if (cb.isCallable())
            cb.call({QString("[I] Adding Steam entry ") + QString(newShortcut.getAppName().toStdString().c_str()), false, true});
        shortcuts.append(newShortcut);
    }
    steam_tools->updateShortcuts(std::move(shortcuts));
    steam_tools->updateControllerConfig(newShortcut.getAppName(), std::move(controller_layout_workshop_id));
    if (!found)
    {
        if (cb.isCallable())
            cb.call({QString("[I] Added Steam entry: ") + QString(newShortcut.getAppName().toStdString().c_str()), true, true});
    }
    else
    {
        if (cb.isCallable())
            cb.call({QString("[I] Updated Steam entry: ") + QString(newShortcut.getAppName().toStdString().c_str()), true, true});
    }
    delete steam_tools;
}
#endif

QString QmlBackend::openPsnLink()
{
    QUrl url = psnLoginUrl();
    if(QDesktopServices::openUrl(url) && (qEnvironmentVariable("XDG_CURRENT_DESKTOP") != "gamescope"))
    {
        qCWarning(chiakiGui) << "Launched browser.";
        return QString();
    }
    else
    {
        qCWarning(chiakiGui) << "Could not launch browser.";
        return QString(url.toEncoded());
    }
}

void QmlBackend::openNpssoPage()
{
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://ca.account.sony.com/api/v1/ssocookie")));
}

QString QmlBackend::openPlaceboOptionsLink()
{
    QUrl url = QUrl("https://libplacebo.org/options/");
    if(QDesktopServices::openUrl(url) && (qEnvironmentVariable("XDG_CURRENT_DESKTOP") != "gamescope"))
    {
        qCWarning(chiakiGui) << "Launched browser.";
        return QString();
    }
    else
    {
        qCWarning(chiakiGui) << "Could not launch browser.";
        return QString(url.toEncoded());
    }
}

bool QmlBackend::checkPsnRedirectURL(const QUrl &url) const
{
    return url.toString().startsWith(QString::fromStdString(PSNAuth::REDIRECT_PAGE));
}

void QmlBackend::initPsnAuth(const QUrl &url, const QJSValue &callback)
{
    const QJSValue cb = callback;
    if (!url.toString().startsWith(QString::fromStdString(PSNAuth::REDIRECT_PAGE)))
    {
        if (cb.isCallable())
            cb.call({QString("[E] Invalid URL: Please make sure you have copy and pasted the URL correctly."), false, true});
        return;
    }
    const QString code = QUrlQuery(url).queryItemValue("code");
    if (code.isEmpty())
    {
        if (cb.isCallable())
            cb.call({QString("[E] Invalid code from redirect url."), false, true});
        return;
    }
    PSNAccountID *psnId = new PSNAccountID(settings, this);
    connect(psnId, &PSNAccountID::AccountIDResponse, this, &QmlBackend::updatePsnHosts);
    connect(psnId, &PSNAccountID::AccountIDError, this, [cb](const QString &error) {
        if (cb.isCallable())
            cb.call({error, false, true});
    });
    connect(psnId, &PSNAccountID::AccountIDResponse, this, [cb]() {
        if (cb.isCallable())
            cb.call({QString("[I] PSN Remote Connection Tokens Generated."), true, true});
    });
    connect(psnId, &PSNAccountID::Finished, psnId, &QObject::deleteLater);
    psnId->GetPsnAccountId(code);
    emit psnTokenChanged();
}

void QmlBackend::refreshAuth()
{
    PSNToken *psnToken = new PSNToken(settings, this);
    connect(psnToken, &PSNToken::PSNTokenError, this, [this](const QString &error) {
        qCWarning(chiakiGui) << "Could not refresh token. Automatic PSN Connection Unavailable!" << error;
    });
    connect(psnToken, &PSNToken::UnauthorizedError, this, &QmlBackend::psnCredsExpired);
    connect(psnToken, &PSNToken::PSNTokenSuccess, this, []() {
        qCWarning(chiakiGui) << "PSN Remote Connection Tokens Refreshed.";
    });
    connect(psnToken, &PSNToken::PSNTokenSuccess, this, &QmlBackend::updatePsnHosts);
    connect(psnToken, &PSNToken::Finished, psnToken, &QObject::deleteLater);
    QString refresh_token = settings->GetPsnRefreshToken();
    psnToken->RefreshPsnToken(std::move(refresh_token));
}

void QmlBackend::updatePsnHosts()
{
    if(updating_psn_hosts)
    {
        qCInfo(chiakiGui) << "Already updating psn hosts, skipping...";
        return;
    }
    psn_hosts_future = QtConcurrent::run(&QmlBackend::updatePsnHostsThread, this);
    psn_hosts_watcher.setFuture(psn_hosts_future);
    updating_psn_hosts = true;
}

void QmlBackend::updatePsnHostsThread()
{
    QString psn_token = settings->GetPsnAuthToken();
    if(psn_token.isEmpty())
        return;

    ChiakiHolepunchDeviceInfo *device_info_ps5 = nullptr;
    size_t num_devices_ps5 = 0;
    ChiakiLog backend_log;
    chiaki_log_init(&backend_log, settings->GetLogLevelMask(), chiaki_log_cb_print, nullptr);
    for(int i = 0; i < PSN_DEVICES_TRIES; i++)
    {
        ChiakiErrorCode err = chiaki_holepunch_list_devices(psn_token.toUtf8().constData(), CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5, &device_info_ps5, &num_devices_ps5, &backend_log);
        if (err != CHIAKI_ERR_SUCCESS)
        {
            if(PSN_DEVICES_TRIES - i > 1)
            {
                qCWarning(chiakiGui) << "Failed to get PS5 devices trying again";
                continue;
            }
            else
            {
                qCWarning(chiakiGui) << "Failed to get PS5 devices after max tries: " << PSN_DEVICES_TRIES;
                num_devices_ps5 = 0;
            }
        }
        break;
    }
    for (size_t i = 0; i < num_devices_ps5; i++)
    {
        ChiakiHolepunchDeviceInfo dev = device_info_ps5[i];
        // skip devices that don't have remote play enabled
        if(!dev.remoteplay_enabled)
            continue;
        QByteArray duid_bytes(reinterpret_cast<char*>(dev.device_uid), sizeof(dev.device_uid));
        QString duid = QString(duid_bytes.toHex());
        QString name = QString(dev.device_name);
        bool ps5 = true;
        PsnHost psn_host(duid, name, ps5);
        if(!psn_nickname_hosts.contains(name))
            psn_nickname_hosts.insert(name, psn_host);
	    if(!psn_hosts.contains(duid) && settings->GetNicknameRegisteredHostRegistered(name))
		    psn_hosts.insert(duid, psn_host);
    }
    QByteArray duid_bytes(32, 'A');
    QString duid = QString(duid_bytes.toHex());
    QString name = QString("Main PS4 Console");
    bool ps5 = false;
    PsnHost psn_host(duid, name, ps5);
    if(!psn_nickname_hosts.contains(name))
        psn_nickname_hosts.insert(name, psn_host);
    if(!psn_hosts.contains(duid) && (settings->GetPS4RegisteredHostsRegistered() > 0))
        psn_hosts.insert(duid, psn_host);

    emit hostsChanged();
    qCInfo(chiakiGui) << "Updated PSN hosts";
    if(device_info_ps5)
        chiaki_holepunch_free_device_list(&device_info_ps5);
}

void QmlBackend::refreshPsnToken()
{
    QString expiry_s = settings->GetPsnAuthTokenExpiry();
    QString refresh = settings->GetPsnRefreshToken();
    if(expiry_s.isEmpty() || refresh.isEmpty())
        return;
    QDateTime expiry = QDateTime::fromString(expiry_s, settings->GetTimeFormat());
    // give 1 minute buffer
    QDateTime now = QDateTime::currentDateTime().addSecs(60);
    if (now >= expiry)
        refreshAuth();
    else
        updatePsnHosts();
}

void QmlBackend::startAutoConfig(const QString &login, const QString &password)
{
    qCInfo(chiakiGui) << "";
    qCInfo(chiakiGui) << "============================================";
    qCInfo(chiakiGui) << "=== AUTO CONFIG START ===";
    qCInfo(chiakiGui) << "============================================";
    qCInfo(chiakiGui) << "Login:" << login;
    qCInfo(chiakiGui) << "";
    
    emit autoConfigStatus("Начало автоматической настройки...");
    
    if (!network_manager) {
        network_manager = new QNetworkAccessManager(this);
        qCInfo(chiakiGui) << "✓ Created QNetworkAccessManager";
    }
    
    emit autoConfigStatus("Шаг 1/5: Получение токена конфигурации...");
    
    // Формируем URL ТОЧНО ТАК ЖЕ как в QmlSettings::authorizeYandex
    QUrl url("https://4cloud.pro/api.php");
    QUrlQuery query;
    query.addQueryItem("method", "get-conf-token");
    query.addQueryItem("login", login);
    query.addQueryItem("Password", password);
    url.setQuery(query);
    
    qCInfo(chiakiGui) << "AutoConfig request URL:" << url.toString();
    
    QNetworkRequest request(url);
    // НЕ устанавливаем Content-Type для GET-запроса
    
    QNetworkReply *reply = network_manager->get(request);
    
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        QByteArray responseData = reply->readAll();
        
        qCInfo(chiakiGui) << "AutoConfig response - Status:" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                         << "Body length:" << responseData.length();
        reply->deleteLater();
        
        // ВАЖНО: Сервер 4cloud.pro возвращает 404, но с валидным JSON в теле!
        // Поэтому игнорируем ошибку если есть данные для парсинга
        if (reply->error() != QNetworkReply::NoError && responseData.isEmpty()) {
            QString errorMsg = QString("Ошибка сети: %1").arg(reply->errorString());
            qCWarning(chiakiGui) << "AutoConfig error:" << errorMsg;
            emit autoConfigError(errorMsg);
            return;
        }
        
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(responseData, &parseError);
        
        if (parseError.error != QJsonParseError::NoError) {
            QString errorMsg = QString("Ошибка парсинга ответа: %1").arg(parseError.errorString());
            qCWarning(chiakiGui) << "AutoConfig parse error:" << errorMsg;
            emit autoConfigError(errorMsg);
            return;
        }
        
        QJsonObject obj = doc.object();
        
        // Проверяем статус ответа
        QString status = obj.value("status").toString();
        if (status != "success") {
            QString errorMsg = obj.value("message").toString();
            if (errorMsg.isEmpty()) {
                errorMsg = "Неверный логин или пароль";
            }
            qCWarning(chiakiGui) << "AutoConfig auth failed:" << errorMsg;
            emit autoConfigError(errorMsg);
            return;
        }
        
        // Извлекаем данные
        QString configUrl = obj.value("ChiakiConfig").toString();
        QString dateExp = obj.value("Date_exp").toString();
        QString jwt = obj.value("jwt").toString();
        
        if (configUrl.isEmpty() || jwt.isEmpty()) {
            QString errorMsg = "Ответ не содержит необходимых данных";
            qCWarning(chiakiGui) << "AutoConfig error:" << errorMsg;
            emit autoConfigError(errorMsg);
            return;
        }
        
        qCInfo(chiakiGui) << "AutoConfig: Token received, expiry:" << dateExp;
        emit autoConfigStatus("✓ Токен получен (срок: " + dateExp + ")");
        
        // Шаг 2: token-confnewuser (СНАЧАЛА все API вызовы, ПОТОМ загрузка конфига)
        emit autoConfigStatus("Шаг 2/6: Настройка пользователя (1/4)...");
        qCInfo(chiakiGui) << "";
        qCInfo(chiakiGui) << "→ Request: token-confnewuser";
        
        QUrl url2("https://4cloud.pro/api.php");
        QUrlQuery query2;
        query2.addQueryItem("method", "token-confnewuser");
        query2.addQueryItem("jwt", jwt);
        url2.setQuery(query2);
        
        QNetworkRequest request2(url2);
        QNetworkReply *reply2 = network_manager->get(request2);
        
        connect(reply2, &QNetworkReply::finished, this, [this, reply2, jwt, configUrl]() {
                qCInfo(chiakiGui) << "";
                qCInfo(chiakiGui) << "← RESPONSE: token-confnewuser";
                reply2->deleteLater();
                
                if (reply2->error() != QNetworkReply::NoError) {
                    qCWarning(chiakiGui) << "✗ Error:" << reply2->errorString();
                    emit autoConfigError("Ошибка confnewuser: " + reply2->errorString());
                    return;
                }
                
                QByteArray data2 = reply2->readAll();
                qCInfo(chiakiGui) << "Response:" << data2;
                
                QJsonDocument doc2 = QJsonDocument::fromJson(data2);
                QJsonObject obj2 = doc2.object();
                
                QString status2 = obj2.value("Status").toString();
                QString message2 = obj2.value("Message").toString();
                qCInfo(chiakiGui) << "Status:" << status2 << "Message:" << message2;
                
                if (status2 != "Success") {
                    qCWarning(chiakiGui) << "✗ Failed, status:" << status2;
                    emit autoConfigError("confnewuser: " + message2);
                    return;
                }
                
                qCInfo(chiakiGui) << "✓ Step 1/3 OK:" << message2;
                emit autoConfigStatus("✓ Шаг 1/3: " + message2);
                
                // Шаг 3: token-checkanddeleteexistingip
                emit autoConfigStatus("Шаг 3/6: Настройка пользователя (2/4)...");
                qCInfo(chiakiGui) << "";
                qCInfo(chiakiGui) << "→ Request: token-checkanddeleteexistingip";
                
                QUrl url3("https://4cloud.pro/api.php");
                QUrlQuery query3;
                query3.addQueryItem("method", "token-checkanddeleteexistingip");
                query3.addQueryItem("jwt", jwt);
                url3.setQuery(query3);
                
                QNetworkRequest request3(url3);
                QNetworkReply *reply3 = network_manager->get(request3);
                
                connect(reply3, &QNetworkReply::finished, this, [this, reply3, jwt, configUrl]() {
                    qCInfo(chiakiGui) << "";
                    qCInfo(chiakiGui) << "← RESPONSE: token-checkanddeleteexistingip";
                    reply3->deleteLater();
                    
                    if (reply3->error() != QNetworkReply::NoError) {
                        qCWarning(chiakiGui) << "✗ Error:" << reply3->errorString();
                        emit autoConfigError("Ошибка checkanddeleteexistingip: " + reply3->errorString());
                        return;
                    }
                    
                    QByteArray data3 = reply3->readAll();
                    qCInfo(chiakiGui) << "Response:" << data3;
                    
                    QJsonDocument doc3 = QJsonDocument::fromJson(data3);
                    QJsonObject obj3 = doc3.object();
                    
                    QString status3 = obj3.value("Status").toString();
                    if (status3 != "Success") {
                        qCWarning(chiakiGui) << "✗ Failed, status:" << status3;
                        emit autoConfigError("checkanddeleteexistingip: неудача");
                        return;
                    }
                    
                    qCInfo(chiakiGui) << "✓ Step 2/3 OK";
                    emit autoConfigStatus("✓ Шаг 2/3: IP проверен");
                    
                    // Шаг 4: token-confuserip
                    emit autoConfigStatus("Шаг 4/6: Настройка IP (3/4)...");
                    qCInfo(chiakiGui) << "";
                    qCInfo(chiakiGui) << "→ Request: token-confuserip";
                    
                    QUrl url4("https://4cloud.pro/api.php");
                    QUrlQuery query4;
                    query4.addQueryItem("method", "token-confuserip");
                    query4.addQueryItem("jwt", jwt);
                    url4.setQuery(query4);
                    
                    QNetworkRequest request4(url4);
                    QNetworkReply *reply4 = network_manager->get(request4);
                    
                    connect(reply4, &QNetworkReply::finished, this, [this, reply4, jwt, configUrl]() {
                        qCInfo(chiakiGui) << "";
                        qCInfo(chiakiGui) << "← RESPONSE: token-confuserip";
                        reply4->deleteLater();
                        
                        if (reply4->error() != QNetworkReply::NoError) {
                            qCWarning(chiakiGui) << "✗ Error:" << reply4->errorString();
                            emit autoConfigError("Ошибка confuserip: " + reply4->errorString());
                            return;
                        }
                        
                        QByteArray data4 = reply4->readAll();
                        qCInfo(chiakiGui) << "Response:" << data4;
                        
                        QJsonDocument doc4 = QJsonDocument::fromJson(data4);
                        QJsonObject obj4 = doc4.object();
                        
                        QString status4 = obj4.value("Status").toString();
                        QString userIP = obj4.value("UserIP").toString();
                        QString cdnIP = obj4.value("CDNIP").toString();
                        
                        qCInfo(chiakiGui) << "Status:" << status4 << "UserIP:" << userIP << "CDNIP:" << cdnIP;
                        
                        if (status4 != "Success") {
                            qCWarning(chiakiGui) << "✗ Failed, status:" << status4;
                            emit autoConfigError("confuserip: неудача");
                            return;
                        }
                        
                        qCInfo(chiakiGui) << "✓ Step 3/3 OK - IP:" << userIP;
                        emit autoConfigStatus("✓ Шаг 3/3: IP настроен (" + userIP + ")");
                        
                        // Шаг 5: Финальная проверка: token-checkdirectconf
                        emit autoConfigStatus("Шаг 5/6: Проверка прямого подключения...");
                        qCInfo(chiakiGui) << "";
                        qCInfo(chiakiGui) << "→ Request: token-checkdirectconf";
                        
                        QUrl url5("https://4cloud.pro/api.php");
                        QUrlQuery query5;
                        query5.addQueryItem("method", "token-checkdirectconf");
                        query5.addQueryItem("jwt", jwt);
                        url5.setQuery(query5);
                        
                        QNetworkRequest request5(url5);
                        QNetworkReply *reply5 = network_manager->get(request5);
                        
                        connect(reply5, &QNetworkReply::finished, this, [this, reply5, configUrl]() {
                            qCInfo(chiakiGui) << "";
                            qCInfo(chiakiGui) << "← RESPONSE: token-checkdirectconf";
                            reply5->deleteLater();
                            
                            if (reply5->error() != QNetworkReply::NoError) {
                                qCWarning(chiakiGui) << "✗ Error:" << reply5->errorString();
                                emit autoConfigError("Ошибка checkdirectconf: " + reply5->errorString());
                                return;
                            }
                            
                            QByteArray data5 = reply5->readAll();
                            qCInfo(chiakiGui) << "Response:" << data5;
                            
                            QJsonDocument doc5 = QJsonDocument::fromJson(data5);
                            QJsonObject obj5 = doc5.object();
                            
                            QString status5 = obj5.value("Status").toString();
                            QString message5 = obj5.value("Message").toString();
                            QString base5 = obj5.value("Base").toString();
                            
                            qCInfo(chiakiGui) << "Status:" << status5 << "Message:" << message5 << "Base:" << base5;
                            
                            if (status5 != "Success") {
                                qCWarning(chiakiGui) << "✗ Failed, status:" << status5;
                                emit autoConfigError("checkdirectconf: " + message5);
                                return;
                            }
                            
                            qCInfo(chiakiGui) << "✓ Проверка завершена: " << message5;
                            emit autoConfigStatus("✓ Проверка завершена: " + message5);
                            
                            // Шаг 6: ТЕПЕРЬ загружаем и импортируем конфиг (ПОСЛЕ всех API вызовов)
                            emit autoConfigStatus("Шаг 6/6: Загрузка и импорт конфигурации...");
                            qCInfo(chiakiGui) << "";
                            qCInfo(chiakiGui) << "→ Request: Download config from" << configUrl;
                            
                            QUrl configUrlObj(configUrl);
                            QNetworkRequest configRequest(configUrlObj);
                            
                            QNetworkReply *configReply = network_manager->get(configRequest);
                            
                            connect(configReply, &QNetworkReply::finished, this, [this, configReply]() {
                                qCInfo(chiakiGui) << "";
                                qCInfo(chiakiGui) << "← RESPONSE: Config file";
                                configReply->deleteLater();
                                
                                if (configReply->error() != QNetworkReply::NoError) {
                                    qCWarning(chiakiGui) << "✗ Download error:" << configReply->errorString();
                                    emit autoConfigError("Ошибка загрузки конфига: " + configReply->errorString());
                                    return;
                                }
                                
                                QByteArray configData = configReply->readAll();
                                qCInfo(chiakiGui) << "✓ Config size:" << configData.size() << "bytes";
                                emit autoConfigStatus("✓ Конфигурация загружена (" + QString::number(configData.size()) + " байт)");
                                
                                emit autoConfigStatus("Импорт настроек...");
                                qCInfo(chiakiGui) << "→ Importing settings...";
                                
                                // Импортируем настройки
                                QTemporaryFile tempFile;
                                if (!tempFile.open()) {
                                    qCWarning(chiakiGui) << "✗ Failed to create temp file";
                                    emit autoConfigError("Не удалось создать временный файл");
                                    return;
                                }
                                
                                tempFile.write(configData);
                                tempFile.flush();
                                QString filePath = tempFile.fileName();
                                
                            qCInfo(chiakiGui) << "→ Calling ImportSettings with:" << filePath;
                            settings->ImportSettings(filePath);
                            qCInfo(chiakiGui) << "✓ Settings imported";
                            
                            // Автоматически устанавливаем декодер на d3d11va после импорта
                            settings->SetHardwareDecoder("d3d11va");
                            qCInfo(chiakiGui) << "✓ Hardware decoder set to d3d11va";
                            
                            emit autoConfigStatus("✓ Настройки импортированы");
                                
                                qCInfo(chiakiGui) << "";
                                qCInfo(chiakiGui) << "============================================";
                                qCInfo(chiakiGui) << "=== AUTO CONFIG SUCCESS ===";
                                qCInfo(chiakiGui) << "============================================";
                                qCInfo(chiakiGui) << "";
                                
                                emit autoConfigSuccess();
                            });
                        });
                    });
                });
            });
        });
}

void QmlBackend::authenticate(const QString &email, const QString &password)
{
    qCInfo(chiakiGui) << "Authentication request (UDP token) for email:" << email;
    // Drop previous account identity before switching users.
    settings->SetCloudBillingUserId(0);
    settings->SetFourCloudEmail(email.trimmed().toLower());
    if (settings_qml) {
        settings_qml->refreshCloudBillingUserId();
        settings_qml->refreshFourCloudEmail();
    }
    if (cloud_catalog_backend)
        cloud_catalog_backend->invalidateCache();

    const QString host = settings->GetCloudBillingHost();
    const quint16 port = settings->GetCloudAuthPort();
    if (host.isEmpty()) {
        emit authenticationError(QStringLiteral("Не задан адрес сервера авторизации"));
        return;
    }

    const QString login_email = email.trimmed().toLower();
    auto *watcher = new QFutureWatcher<CloudBillingClient::Result>(this);
    connect(watcher, &QFutureWatcher<CloudBillingClient::Result>::finished, this, [this, watcher]() {
        const CloudBillingClient::Result result = watcher->result();
        watcher->deleteLater();
        if (!result.ok) {
            QString msg = result.ui_message;
            if (msg.isEmpty())
                msg = result.error;
            if (msg.isEmpty())
                msg = result.data.value(QStringLiteral("message")).toString();
            if (msg.isEmpty())
                msg = QStringLiteral("Ошибка авторизации");
            qCWarning(chiakiGui) << "UDP auth failed:" << msg;
            clearAuthEntitlements();
            emit authenticationError(msg);
            return;
        }
        applyAuthSession(result.data, true);
    });
    watcher->setFuture(QtConcurrent::run([host, port, login_email, password]() {
        return CloudBillingClient::authSignIn(host, port, login_email, password);
    }));
}

void QmlBackend::checkJwtToken()
{
    QString jwt = settings->GetJwtToken();
    if (jwt.isEmpty()) {
        qCInfo(chiakiGui) << "No JWT token to check";
        clearAuthEntitlements();
        emit jwtTokenExpired();
        return;
    }

    // Local console expiry only logs out when cloud access is also absent.
    QString localExpiryStr = settings->GetSubscriptionExpiryDate();
    if (!localExpiryStr.isEmpty() && settings->GetConsoleCatalogAccess()) {
        QDateTime localExpiry = QDateTime::fromString(localExpiryStr, "dd.MM.yyyy HH:mm");
        if (localExpiry.isValid() && QDateTime::currentDateTime() >= localExpiry
            && !settings->GetCloudGamesAccess()) {
            qCWarning(chiakiGui) << "Local subscription expiry reached, logging out";
            settings->SetJwtToken("");
            settings->SetJwtPort(0);
            settings->SetNps4("");
            settings->SetSubscriptionExpiryDate("");
            clearAuthEntitlements();
            clearFourcloudState();
            emit subscriptionExpired("Нет активной подписки");
            return;
        }
    }

    const QString host = settings->GetCloudBillingHost();
    const quint16 port = settings->GetCloudAuthPort();
    if (host.isEmpty()) {
        qCWarning(chiakiGui) << "Auth host empty, treating JWT as expired";
        settings->SetJwtToken("");
        clearAuthEntitlements();
        clearFourcloudState();
        emit jwtTokenExpired();
        return;
    }

    qCInfo(chiakiGui) << "Checking JWT via UDP auth" << host << port;
    auto *watcher = new QFutureWatcher<CloudBillingClient::Result>(this);
    connect(watcher, &QFutureWatcher<CloudBillingClient::Result>::finished, this, [this, watcher]() {
        const CloudBillingClient::Result result = watcher->result();
        watcher->deleteLater();
        if (!result.ok) {
            QString msg = result.ui_message;
            if (msg.isEmpty())
                msg = result.error;
            if (msg.isEmpty())
                msg = result.data.value(QStringLiteral("message")).toString();
            qCWarning(chiakiGui) << "UDP check_session failed:" << msg;
            settings->SetJwtToken("");
            settings->SetJwtPort(0);
            settings->SetNps4("");
            settings->SetSubscriptionExpiryDate("");
            clearAuthEntitlements();
            clearFourcloudState();
            if (msg.contains(QStringLiteral("подписк"), Qt::CaseInsensitive)
                || msg.contains(QStringLiteral("no_access"), Qt::CaseInsensitive))
                emit subscriptionExpired(msg.isEmpty() ? QStringLiteral("Нет активной подписки") : msg);
            else
                emit jwtTokenExpired();
            return;
        }
        applyAuthSession(result.data, false);
    });
    watcher->setFuture(QtConcurrent::run([host, port, jwt]() {
        return CloudBillingClient::authCheckSession(host, port, jwt);
    }));
}

void PsnConnectionWorker::ConnectPsnConnection(StreamSession *session, const QString &duid, const bool &ps5)
{
    ChiakiErrorCode result = session->ConnectPsnConnection(duid, ps5);
    emit resultReady(result);
}
