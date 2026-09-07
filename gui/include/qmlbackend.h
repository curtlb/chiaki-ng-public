#pragma once

#include "streamsession.h"
#include "discoverymanager.h"
#include "qmlmainwindow.h"
#include "qmlcontroller.h"
#include "qmlsettings.h"
#include "cloudstreamingbackend.h"
#include "cloudcatalogbackend.h"

#include <QObject>
#include <QThread>
#include <QJSValue>
#include <QUrl>
#include <QFutureWatcher>
#include <QFuture>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkCookieJar>
#include <QJsonDocument>
#include <QJsonObject>
#ifdef CHIAKI_HAVE_WEBENGINE
#include <QQuickWebEngineProfile>
#include <QWebEngineUrlRequestInterceptor>
#endif

class SystemdInhibit;
#ifdef Q_OS_MACOS
    class MacWakeSleep;
#elif defined(Q_OS_WINDOWS)
    class WindowsWakeSleep;
#endif

class QmlRegist : public QObject
{
    Q_OBJECT

public:
    QmlRegist(const ChiakiRegistInfo &regist_info, uint32_t log_mask, QObject *parent = nullptr);

signals:
    void log(ChiakiLogLevel level, const QString &msg);
    void failed();
    void success(const RegisteredHost& host);

private:
    static void log_cb(ChiakiLogLevel level, const char *msg, void *user);
    static void regist_cb(ChiakiRegistEvent *event, void *user);

    ChiakiLog chiaki_log;
    ChiakiRegist chiaki_regist;
};

class PsnConnectionWorker : public QObject
{
    Q_OBJECT

public:
    void ConnectPsnConnection(StreamSession *session, const QString &duid, const bool &ps5);

signals:
    void resultReady(const ChiakiErrorCode &err);
};

#ifdef CHIAKI_HAVE_WEBENGINE
class SecUaRequestInterceptor : public QWebEngineUrlRequestInterceptor {
    Q_OBJECT

public:
    SecUaRequestInterceptor(QString version, QObject *p = Q_NULLPTR) : QWebEngineUrlRequestInterceptor(p) { chrome_version = version; };
    void interceptRequest(QWebEngineUrlRequestInfo &info) override {
#ifdef Q_OS_WINDOWS
        info.setHttpHeader("Sec-Ch-Ua", QString("\"Not(A:Brand\";v=\"99\", \"Microsoft Edge\";v=\"%1\", \"Chromium\";v=\"%1\"").arg(chrome_version).toUtf8());
#else
        info.setHttpHeader("Sec-Ch-Ua", QString("\"Not(A:Brand\";v=\"99\", \"Google Chrome\";v=\"%1\", \"Chromium\";v=\"%1\"").arg(chrome_version).toUtf8());
#endif
    }
private:
    QString chrome_version;
};
#endif
class QmlBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QmlMainWindow* window READ qmlWindow CONSTANT)
    Q_PROPERTY(QmlSettings* settings READ qmlSettings CONSTANT)
    Q_PROPERTY(StreamSession* session READ qmlSession NOTIFY sessionChanged)
    Q_PROPERTY(QList<QmlController*> controllers READ qmlControllers NOTIFY controllersChanged)
    Q_PROPERTY(bool discoveryEnabled READ discoveryEnabled WRITE setDiscoveryEnabled NOTIFY discoveryEnabledChanged)
    Q_PROPERTY(QVariantList hosts READ hosts NOTIFY hostsChanged)
    Q_PROPERTY(QVariantList hiddenHosts READ hiddenHosts NOTIFY hiddenHostsChanged)
    Q_PROPERTY(bool autoConnect READ autoConnect NOTIFY autoConnectChanged)
    Q_PROPERTY(PsnConnectState connectState READ connectState WRITE setConnectState NOTIFY connectStateChanged)
    Q_PROPERTY(QVariantList currentControllerMapping READ currentControllerMapping NOTIFY currentControllerMappingChanged)
    Q_PROPERTY(QString currentControllerType READ currentControllerType NOTIFY currentControllerTypeChanged)
    Q_PROPERTY(bool controllerMappingDefaultMapping READ controllerMappingDefaultMapping NOTIFY controllerMappingDefaultMappingChanged)
    Q_PROPERTY(bool controllerMappingInProgress READ controllerMappingInProgress NOTIFY controllerMappingInProgressChanged)
    Q_PROPERTY(bool controllerMappingAltered READ controllerMappingAltered NOTIFY controllerMappingAlteredChanged)
    Q_PROPERTY(bool enableAnalogStickMapping READ enableAnalogStickMapping WRITE setEnableAnalogStickMapping NOTIFY enableAnalogStickMappingChanged)
    Q_PROPERTY(bool showPingTimeoutDialog READ showPingTimeoutDialog WRITE setShowPingTimeoutDialog NOTIFY showPingTimeoutDialogChanged)
    Q_PROPERTY(bool showAuthorizationFailedDialog READ showAuthorizationFailedDialog WRITE setShowAuthorizationFailedDialog NOTIFY showAuthorizationFailedDialogChanged)
    Q_PROPERTY(bool showPSPlusSubscriptionDialog READ showPSPlusSubscriptionDialog WRITE setShowPSPlusSubscriptionDialog NOTIFY showPSPlusSubscriptionDialogChanged)
    Q_PROPERTY(bool showAccountPrivacySettingsDialog READ showAccountPrivacySettingsDialog WRITE setShowAccountPrivacySettingsDialog NOTIFY showAccountPrivacySettingsDialogChanged)
    Q_PROPERTY(QString accountPrivacyUpgradeUrl READ accountPrivacyUpgradeUrl WRITE setAccountPrivacyUpgradeUrl NOTIFY accountPrivacyUpgradeUrlChanged)
    Q_PROPERTY(QString subscriptionTimeRemaining READ subscriptionTimeRemaining NOTIFY subscriptionTimeRemainingChanged)
    Q_PROPERTY(CloudStreamingBackend* cloudStreaming READ cloudStreaming CONSTANT)
    Q_PROPERTY(CloudCatalogBackend* cloudCatalog READ cloudCatalog CONSTANT)
    Q_PROPERTY(QString cloudLogPath READ cloudLogPath CONSTANT)
    Q_PROPERTY(QString cloudLogPathAlt READ cloudLogPathAlt CONSTANT)
    Q_PROPERTY(bool cloudSteamShortcutEnabled READ cloudSteamShortcutEnabled CONSTANT)
    Q_PROPERTY(bool cloudSessionReconnecting READ cloudSessionReconnecting NOTIFY cloudSessionReconnectingChanged)
    Q_PROPERTY(bool showConsoleCatalogTab READ showConsoleCatalogTab NOTIFY authEntitlementsChanged)
    Q_PROPERTY(bool showCloudGamesTab READ showCloudGamesTab NOTIFY authEntitlementsChanged)

public:

    enum class PsnConnectState
    {
        NotStarted,
        WaitingForInternet,
        InitiatingConnection,
        LinkingConsole,
        RegisteringConsole,
        RegistrationFinished,
        DataConnectionStart,
        DataConnectionFinished,
        ConnectFailed,
        ConnectFailedStart,
        ConnectFailedConsoleUnreachable,
    };
    Q_ENUM(PsnConnectState);
    QmlBackend(Settings *settings, QmlMainWindow *window);
    ~QmlBackend();

    QmlMainWindow *qmlWindow() const;
    QmlSettings *qmlSettings() const;
    StreamSession *qmlSession() const;
    QList<QmlController*> qmlControllers() const;

    bool discoveryEnabled() const;
    void setDiscoveryEnabled(bool enabled);
    void refreshAuth();

    PsnConnectState connectState() const;
    void setConnectState(PsnConnectState connect_state);
    QVariantList hosts() const;

    QVariantList hiddenHosts() const;

    QVariantList currentControllerMapping() const;

    QString currentControllerType() const { return controller_mapping_controller_type; }

    void controllerMappingChangeButton(QString button);

    void controllerMappingUpdate(Controller *controller);

    bool controllerMappingDefaultMapping() const { return controller_mapping_default_mapping; }
    void setControllerMappingDefaultMapping(bool is_default_mapping);

    bool controllerMappingAltered() const { return controller_mapping_altered; }
    void setControllerMappingAltered(bool altered);

    bool controllerMappingInProgress() const  {return controller_mapping_in_progress; }
    void setControllerMappingInProgress(bool is_in_progress);

    bool enableAnalogStickMapping() const { return enable_analog_stick_mapping; }
    QString subscriptionTimeRemaining() const { return subscription_time_remaining; }
    void setEnableAnalogStickMapping(bool enabled);

    bool showPingTimeoutDialog() const { return show_ping_timeout_dialog; }
    void setShowPingTimeoutDialog(bool show);

    bool showAuthorizationFailedDialog() const { return show_authorization_failed_dialog; }
    void setShowAuthorizationFailedDialog(bool show);

    bool showPSPlusSubscriptionDialog() const { return show_ps_plus_subscription_dialog; }
    void setShowPSPlusSubscriptionDialog(bool show);

    bool showAccountPrivacySettingsDialog() const { return show_account_privacy_settings_dialog; }
    void setShowAccountPrivacySettingsDialog(bool show);

    QString accountPrivacyUpgradeUrl() const { return account_privacy_upgrade_url; }
    void setAccountPrivacyUpgradeUrl(const QString &url);

    CloudStreamingBackend *cloudStreaming() const;
    CloudCatalogBackend *cloudCatalog() const;
    QString cloudLogPath() const;
    QString cloudLogPathAlt() const;
    bool cloudSteamShortcutEnabled() const;

    bool cloudSessionReconnecting() const { return cloud_session_reconnecting; }
    void setCloudSessionReconnecting(bool reconnecting);

    bool showConsoleCatalogTab() const;
    bool showCloudGamesTab() const;

    void finishAutoRegister(const ChiakiRegisteredHost &host);

    bool autoConnect() const;

    void psnConnector();

    void createSession(const StreamSessionConnectInfo &connect_info);

    void psnSessionStart();

    void checkPsnConnection(const ChiakiErrorCode &err);

    void checkNickname(QString nickname);

    bool closeRequested();

    void setAllowJoystickBackgroundEvents();

    void setIsAppActive();

    void profileChanged();

    void goToSleep();

    bool zeroCopy()        { return !disable_zero_copy; };
    void disableZeroCopy() { disable_zero_copy = true; };

    Q_INVOKABLE void deleteHost(int index);
    Q_INVOKABLE void wakeUpHost(int index, QString nickname = QString());
    Q_INVOKABLE void addManualHost(int index, const QString &address);
    Q_INVOKABLE void hideHost(const QString &mac_string, const QString &host_nickname);
    Q_INVOKABLE void unhideHost(const QString &mac_string);
    Q_INVOKABLE bool registerHost(const QString &host, const QString &psn_id, const QString &pin, const QString &cpin, bool broadcast, int target, const QJSValue &callback);
    Q_INVOKABLE void connectToHost(int index, QString nickname = QString());
    Q_INVOKABLE void stopSession(bool sleep);
    Q_INVOKABLE void reconnectCloudSession();
    Q_INVOKABLE void sessionGoHome();
    Q_INVOKABLE void enterPin(const QString &pin);
    Q_INVOKABLE QUrl psnLoginUrl() const;
    Q_INVOKABLE bool checkPsnRedirectURL(const QUrl &url) const;
    Q_INVOKABLE bool handlePsnLoginRedirect(const QUrl &url);
    Q_INVOKABLE void stopAutoConnect();
    Q_INVOKABLE void setConsolePin(int index, QString console_pin);
    Q_INVOKABLE QString openPsnLink();
    Q_INVOKABLE void openNpssoPage();
    Q_INVOKABLE QString openPlaceboOptionsLink();
    Q_INVOKABLE void initPsnAuth(const QUrl &url, const QJSValue &callback);
    Q_INVOKABLE void psnCancel(bool stop_thread);
    Q_INVOKABLE void refreshPsnToken();
    Q_INVOKABLE void creatingControllerMapping(bool creating_controller_mapping);
    Q_INVOKABLE void updateButton(int chiaki_button, QString physical_button, int new_index);
    Q_INVOKABLE void controllerMappingSelectButton();
    Q_INVOKABLE void controllerMappingReset();
    Q_INVOKABLE void controllerMappingQuit();
    Q_INVOKABLE void controllerMappingButtonQuit();
    Q_INVOKABLE void controllerMappingApply();
    Q_INVOKABLE void autoRegister();
    Q_INVOKABLE void startAutoConfig(const QString &login, const QString &password);
    Q_INVOKABLE void authenticate(const QString &email, const QString &password);
    Q_INVOKABLE void checkJwtToken();
    Q_INVOKABLE void ensureFourcloudPolling();
    Q_INVOKABLE void logoutFourcloud();
#if CHIAKI_GUI_ENABLE_STEAM_SHORTCUT
    Q_INVOKABLE void createSteamShortcut(QString shortcutName, QString launchOptions, const QJSValue &callback, QString steamDir);
#endif
#ifdef CHIAKI_HAVE_WEBENGINE
    Q_INVOKABLE void setWebEngineHints(QQuickWebEngineProfile *profile);
    Q_INVOKABLE void clearCookies(QQuickWebEngineProfile *profile);
#endif

signals:
    void sessionChanged(StreamSession *session);
    void psnConnect(StreamSession *session, const QString &duid, const bool &ps5);
    void showPsnView();
    void connectStateChanged();
    void controllersChanged();
    void currentControllerMappingChanged();
    void currentControllerTypeChanged();
    void controllerMappingInProgressChanged();
    void controllerMappingDefaultMappingChanged();
    void controllerMappingButtonSelected(QString button);
    void controllerMappingAlteredChanged();
    void controllerMappingSteamControllerSelected();
    void enableAnalogStickMappingChanged();
    void discoveryEnabledChanged();
    void hostsChanged();
    void hiddenHostsChanged();
    void psnTokenChanged();
    void psnCredsExpired();
    void autoConnectChanged();
    void wakeupStartInitiated();
    void wakeupStartFailed();
    void windowTypeUpdated(WindowType type);

    void error(const QString &title, const QString &text);
    void error(const QString &title, const QString &text, int durationMs);
    void sessionError(const QString &title, const QString &text);
    void sessionPinDialogRequested();
    void sessionStopDialogRequested();
    void cloudSessionReconnectingChanged();
    void registDialogRequested(const QString &host, bool ps5, const QString &duid);
    void psnLoginAccountIdDone(const QString &accountId);
    void psnLoginAccountIdError(const QString &error);
    void autoConfigStatus(const QString &message);
    void autoConfigSuccess();
    void autoConfigError(const QString &errorMessage);
    void authenticationSuccess();
    void authenticationError(const QString &errorMessage);
    void jwtTokenExpired();
    void jwtTokenValid();
    void authEntitlementsChanged();
    void subscriptionExpired(const QString &message);
    void subscriptionTimeRemainingChanged();
    void showPingTimeoutDialogChanged();
    void showAuthorizationFailedDialogChanged();
    void showPSPlusSubscriptionDialogChanged();
    void showAccountPrivacySettingsDialogChanged();
    void accountPrivacyUpgradeUrlChanged();

private:
    struct DisplayServer {
        bool valid = false;

        DiscoveryHost discovery_host;
        ManualHost manual_host;
        bool discovered;

        PsnHost psn_host;
        QString duid;
        RegisteredHost registered_host;
        bool registered;

        QString GetHostAddr() const { return discovered ? discovery_host.host_addr : manual_host.GetHost(); }
        bool IsPS5() const { return discovered ? discovery_host.ps5 :
            (registered ? chiaki_target_is_ps5(registered_host.GetTarget()) : true); }
    };

    DisplayServer displayServerAt(int index) const;
    bool sendWakeup(const DisplayServer &server);
    bool sendWakeup(const QString &host, const QByteArray &regist_key, bool ps5);
    void continueConnectToHost(int index, QString nickname, bool need_wakeup);
    void updateControllers();
    void updateControllerMappings();
    void updateDiscoveryHosts();
    void updatePsnHosts();
    void updatePsnHostsThread();
    void updateAudioVolume();
    void resumeFromSleep();
    uint32_t getStreamShortcut() const;
    void updateStreamShortcut();
    QString getExecutable();

    Settings *settings = {};
    QmlSettings *settings_qml = {};
    QmlMainWindow *window = {};
    StreamSession *session = {};
    bool cloud_session_reconnect_pending = false;
    bool cloud_session_reconnecting = false;
    QThread *frame_thread = {};
    QTimer *psn_reconnect_timer = {};
    QTimer *psn_auto_connect_timer = {};
    QTimer *wakeup_start_timer = {};
    QTimer *wakeup_repeat_timer = {};
    QString wakeup_host_addr;
    QByteArray wakeup_regist_key;
    bool wakeup_ps5 = false;
    int psn_reconnect_tries = 0;
    QThread psn_connection_thread;
    PsnConnectState psn_connect_state;
    DiscoveryManager discovery_manager;
    QList<QString> waking_sleeping_nicknames;
    QHash<int, QmlController*> controllers;
    DisplayServer regist_dialog_server;
    StreamSessionConnectInfo session_info = {};
    SystemdInhibit *sleep_inhibit = {};
#ifdef Q_OS_MACOS
    MacWakeSleep *mac_wake_sleep = {};
#elif defined(Q_OS_WINDOWS)
    WindowsWakeSleep *windows_wake_sleep = {};
#endif
    bool controller_mapping_default_mapping = false;
    bool controller_mapping_altered = false;
    bool updating_psn_hosts = false;
    QFutureWatcher<void> psn_hosts_watcher;
    QFuture<void> psn_hosts_future;
    bool disable_zero_copy = false;
    Controller *controller_mapping_controller = {};
    QMap<QString, QStringList> controller_guids_to_update = {};
    int controller_mapping_id = -1;
    QString controller_mapping_controller_guid = "";
    QString controller_mapping_controller_vid_pid = "";
    QString controller_mapping_controller_type = "";
    QMap<QString, QStringList> controller_mapping_controller_mappings = {};
    QMap<QString, QStringList> controller_mapping_applied_controller_mappings = {};
    QMap<QString, QString> controller_mapping_physical_button_mappings = {};
    QMap<QString, QString> controller_mapping_original_controller_mappings = {};
    bool controller_mapping_in_progress = false;
    bool enable_analog_stick_mapping = false;
    bool show_ping_timeout_dialog = false;
    bool show_authorization_failed_dialog = false;
    bool show_ps_plus_subscription_dialog = false;
    bool show_account_privacy_settings_dialog = false;
    QString account_privacy_upgrade_url;
    bool resume_session = false;
    bool settings_allocd = false;
    HostMAC auto_connect_mac = {};
    QNetworkAccessManager *network_manager = nullptr;
    QString auto_connect_nickname = "";
    QString wakeup_nickname = "";
    bool wakeup_start = false;
    QString fourcloud_state_cache;  // "ready"/"standby"/"unknown" из API 4cloud для manual host
    bool fourcloud_state_retrying = false;  // повторная проверка после первого "оффлайн"
    QTimer *fourcloud_state_timer = nullptr;
    void fetchFourcloudState();
    CloudStreamingBackend *cloud_streaming_backend = {};
    CloudCatalogBackend *cloud_catalog_backend = {};
    void clearFourcloudState();
    void fetchYandexIamByJwt(const QString &jwt);
    void applyAuthSession(const QJsonObject &session, bool from_login);
    void clearAuthEntitlements();
    void resolveCloudBillingIdentity();
    QString subscription_time_remaining;
    QTimer *subscription_expiry_timer = nullptr;
    void fetchSubscriptionExpiry();
    void startSubscriptionExpiryTimer();
    QMap<QString, PsnHost> psn_hosts = {};
    QMap<QString, PsnHost> psn_nickname_hosts = {};
#ifdef CHIAKI_HAVE_WEBENGINE
    SecUaRequestInterceptor * request_interceptor = {};
#endif
};