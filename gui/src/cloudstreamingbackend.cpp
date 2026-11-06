// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloudstreamingbackend.h"
#include "streamsession.h"
#include "exception.h"
#include "cloudlog.h"
#include "cloudbillingclient.h"
#include "chiaki/remote/holepunch.h"
#include "chiaki/session.h"
#include "chiaki/cloudsession.h"
#include "chiaki/log.h"
#include "qmlbackend.h"
#include "qmlsettings.h"
#include "cloudcatalogbackend.h"

#include <QObject>
#include <QCoreApplication>
#include <QDateTime>
#include <QLoggingCategory>
#include <QPointer>
#include <QSet>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QMetaObject>
#include <functional>
#include <thread>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
}

Q_DECLARE_LOGGING_CATEGORY(chiakiGui)

static int billingMinutesFromResponse(const QJsonObject &data)
{
    if(data.contains(QStringLiteral("minutes_left")))
        return data.value(QStringLiteral("minutes_left")).toInt(0);
    const QString paid_until = data.value(QStringLiteral("paid_until")).toString().trimmed();
    if(paid_until.isEmpty())
        return 0;
    const QDateTime until = QDateTime::fromString(paid_until, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if(!until.isValid())
        return 0;
    const qint64 secs = QDateTime::currentDateTime().secsTo(until);
    if(secs <= 0)
        return 0;
    return static_cast<int>((secs + 59) / 60);
}

static void noteBillingIdentity(Settings *settings, QObject *context, const QJsonObject &data)
{
    if(!settings)
        return;
    const qint64 uid = data.value(QStringLiteral("user_id")).toVariant().toLongLong();
    if(uid > 0) {
        settings->SetCloudBillingUserId(uid);
        if(auto *backend = qobject_cast<QmlBackend*>(context)) {
            if(QmlSettings *qs = backend->qmlSettings())
                qs->refreshCloudBillingUserId();
        }
    }
    const QString email = data.value(QStringLiteral("email")).toString().trimmed();
    if(!email.isEmpty()) {
        const QString normalized = email.toLower();
        if(settings->GetFourCloudEmail().compare(normalized, Qt::CaseInsensitive) != 0
            || settings->GetFourCloudEmail().isEmpty()) {
            settings->SetFourCloudEmail(normalized);
            if(auto *backend = qobject_cast<QmlBackend*>(context)) {
                if(QmlSettings *qs = backend->qmlSettings())
                    qs->refreshFourCloudEmail();
            }
        }
    }
}

CloudStreamingBackend::CloudStreamingBackend(Settings *settings, QObject *parent)
    : QObject(parent)
    , settings(settings)
    , allocation_progress("")
{
    billing_heartbeat_timer.setInterval(15000);
    connect(&billing_heartbeat_timer, &QTimer::timeout, this, &CloudStreamingBackend::onBillingHeartbeatTick);
}

CloudStreamingBackend::~CloudStreamingBackend()
{
    notifyStreamStopped();
}

void CloudStreamingBackend::setBillingStatus(const QString &message, int minutes_left)
{
    bool changed = false;
    if(billing_status_message != message) {
        billing_status_message = message;
        changed = true;
    }
    if(minutes_left >= 0 && billing_minutes_left != minutes_left) {
        billing_minutes_left = minutes_left;
        changed = true;
    }
    if(changed)
        emit billingStatusChanged();
}

void CloudStreamingBackend::setBillingMinutesOnly(int minutes_left)
{
    const int mins = qMax(0, minutes_left);
    setBillingStatus(tr("Осталось %1 мин").arg(mins), mins);
}

void CloudStreamingBackend::startBillingHeartbeat()
{
    if(billing_session_token.isEmpty())
        return;
    if(!billing_heartbeat_timer.isActive())
        billing_heartbeat_timer.start();
    sendBillingHeartbeat(true);
}

void CloudStreamingBackend::stopBillingHeartbeat()
{
    billing_heartbeat_timer.stop();
}

void CloudStreamingBackend::sendBillingHeartbeat(bool streaming)
{
    if(!settings || billing_session_token.isEmpty())
        return;
    // Do not stack blocking UDP waits on the GUI/stream thread.
    bool expected = false;
    if(!billing_heartbeat_inflight.compare_exchange_strong(expected, true))
        return;

    const QString host = settings->GetCloudBillingHost();
    const quint16 port = settings->GetCloudBillingPort();
    const QString token = billing_session_token;
    const QString email = settings->GetFourCloudEmail();
    const bool streaming_flag = streaming;
    QPointer<CloudStreamingBackend> self(this);
    QPointer<QObject> ctx(parent());

    std::thread([self, ctx, host, port, token, email, streaming_flag]() {
        CloudBillingClient::Result res = CloudBillingClient::heartbeat(host, port, token, streaming_flag);
        CloudBillingClient::Result renew_res;
        bool did_renew = false;
        if(res.ok && res.data.value(QStringLiteral("should_renew")).toBool() && !email.isEmpty()) {
            renew_res = CloudBillingClient::renew(host, port, email, token);
            did_renew = true;
        }

        if(!self)
            return;
        QMetaObject::invokeMethod(self, [self, ctx, token, res, renew_res, did_renew]() {
            if(!self)
                return;
            self->billing_heartbeat_inflight.store(false);
            if(self->billing_session_token != token)
                return;

            auto forceStopIfNeeded = [self](const CloudBillingClient::Result &r) {
                const bool force = r.data.value(QStringLiteral("force_stop")).toBool(false)
                    || r.error.contains(QStringLiteral("истёк"), Qt::CaseInsensitive)
                    || r.error.contains(QStringLiteral("закончилось"), Qt::CaseInsensitive)
                    || r.error.contains(QStringLiteral("Сессия завершена"), Qt::CaseInsensitive)
                    || r.ui_message.contains(QStringLiteral("истёк"), Qt::CaseInsensitive)
                    || r.ui_message.contains(QStringLiteral("закончилось"), Qt::CaseInsensitive);
                if(!force)
                    return false;
                const QString msg = r.ui_message.isEmpty() ? r.error : r.ui_message;
                self->stopBillingHeartbeat();
                self->setBillingStatus(msg, 0);
                const QString save_msg = r.data.value(QStringLiteral("save_retention_message")).toString().trimmed();
                if(!save_msg.isEmpty())
                    emit self->saveRetentionDialogRequested(save_msg);
                emit self->billingForceStopRequested(msg);
                return true;
            };

            if(!res.ok) {
                if(forceStopIfNeeded(res))
                    return;
                self->setBillingStatus(res.ui_message.isEmpty() ? res.error : res.ui_message);
                return;
            }

            if(self->settings)
                noteBillingIdentity(self->settings, ctx, res.data);
            int mins = billingMinutesFromResponse(res.data);
            self->setBillingMinutesOnly(mins);

            if(did_renew) {
                if(renew_res.ok)
                    self->setBillingMinutesOnly(billingMinutesFromResponse(renew_res.data));
                else if(forceStopIfNeeded(renew_res))
                    return;
                else
                    self->setBillingStatus(
                        renew_res.ui_message.isEmpty() ? renew_res.error : renew_res.ui_message,
                        mins);
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void CloudStreamingBackend::notifyStreamStopped()
{
    stopBillingHeartbeat();
    if(!settings || billing_session_token.isEmpty())
        return;
    CloudLogMessage(QStringLiteral("Session"),
        QStringLiteral("billing end_stream (stream stopped, token=%1…)")
            .arg(billing_session_token.left(8)));
    CloudBillingClient::Result res = CloudBillingClient::endStream(
        settings->GetCloudBillingHost(),
        settings->GetCloudBillingPort(),
        billing_session_token);
    const QString save_msg = res.data.value(QStringLiteral("save_retention_message")).toString().trimmed();
    if(!save_msg.isEmpty())
        emit saveRetentionDialogRequested(save_msg);
    billing_session_token.clear();
    billing_npsso.clear();
    billing_game_identifier.clear();
    billing_store_country.clear();
    billing_store_lang.clear();
    billing_payment_pending = false;
    setBillingStatus(QString(), 0);
}

bool CloudStreamingBackend::runBillingStart(QString serviceType, QString gameIdentifier, QString gameName, QString *out_npsso, QString *out_error, quint64 account_id)
{
    if(!settings || !settings->GetCloudBillingEnabled()) {
        CloudLogMessage(QStringLiteral("Session"), QStringLiteral("billing skipped: cloud_billing_enabled=false"));
        return false;
    }
    const QString email = settings->GetFourCloudEmail();
    if(email.isEmpty()) {
        CloudLogMessage(QStringLiteral("Session"),
            QStringLiteral("billing skipped: fourCloudEmail empty (re-login to 4cloud.pro may be required)"));
        return false;
    }

    const QString host = settings->GetCloudBillingHost();
    if(host.isEmpty()) {
        CloudLogMessage(QStringLiteral("Session"), QStringLiteral("billing skipped: cloud_billing_host empty"));
        return false;
    }
    const quint16 port = settings->GetCloudBillingPort();

    CloudLogMessage(QStringLiteral("Session"),
        QStringLiteral("billing start request email=%1 host=%2:%3 game=%4/%5 account_id=%6")
            .arg(email, host).arg(port).arg(serviceType, gameIdentifier).arg(account_id));

    setAllocationProgress(tr("Списание с привязанной карты…"));
    const auto start = CloudBillingClient::start(host, port, email, serviceType, gameIdentifier, gameName,
                                                 static_cast<qint64>(account_id));
    if(!start.ok) {
        *out_error = start.ui_message.isEmpty() ? start.error : start.ui_message;
        return true;
    }

    noteBillingIdentity(settings, parent(), start.data);
    billing_session_token = start.data.value(QStringLiteral("session_token")).toString();
    *out_npsso = start.data.value(QStringLiteral("npsso")).toString();
    billing_npsso = *out_npsso;
    // NPSSO is ephemeral for this launch only — never write to QSettings.
    billing_game_identifier = start.data.value(QStringLiteral("game_identifier")).toString().trimmed();
    billing_store_country = start.data.value(QStringLiteral("store_country")).toString().trimmed().toUpper();
    billing_store_lang = start.data.value(QStringLiteral("store_lang")).toString().trimmed().toLower();
    if (billing_store_lang.isEmpty())
        billing_store_lang = QStringLiteral("en");
    billing_payment_pending = start.data.value(QStringLiteral("payment_pending")).toBool(false);
    setBillingMinutesOnly(billingMinutesFromResponse(start.data));
    setAllocationProgress(start.ui_message);
    if(!billing_payment_pending)
        startBillingHeartbeat();
    return true;
}

bool CloudStreamingBackend::confirmBillingCharge(QString *out_error)
{
    if(!settings || billing_session_token.isEmpty() || !billing_payment_pending)
        return true;
    const QString email = settings->GetFourCloudEmail();
    const auto res = CloudBillingClient::confirmStream(
        settings->GetCloudBillingHost(),
        settings->GetCloudBillingPort(),
        email,
        billing_session_token);
    if(!res.ok) {
        if(out_error)
            *out_error = res.ui_message.isEmpty() ? res.error : res.ui_message;
        return false;
    }
    billing_payment_pending = false;
    setBillingMinutesOnly(billingMinutesFromResponse(res.data));
    startBillingHeartbeat();
    return true;
}

void CloudStreamingBackend::abandonBillingReservation()
{
    if(!settings || billing_session_token.isEmpty())
        return;
    if(billing_payment_pending) {
        CloudBillingClient::endStream(
            settings->GetCloudBillingHost(),
            settings->GetCloudBillingPort(),
            billing_session_token);
        billing_session_token.clear();
        billing_npsso.clear();
        billing_game_identifier.clear();
        billing_store_country.clear();
        billing_store_lang.clear();
        billing_payment_pending = false;
        stopBillingHeartbeat();
    }
}

void CloudStreamingBackend::onBillingHeartbeatTick()
{
    sendBillingHeartbeat(true);
}

void CloudStreamingBackend::fetchBillingQuote(QString serviceType, QString gameIdentifier, QString gameName, const QJSValue &callback)
{
    fetchBillingQuote(serviceType, gameIdentifier, gameName, 0, callback);
}

void CloudStreamingBackend::fetchBillingQuote(QString serviceType, QString gameIdentifier, QString gameName, quint64 accountId, const QJSValue &callback)
{
    if(!settings || !settings->GetCloudBillingEnabled()) {
        if(callback.isCallable())
            callback.call({false, tr("Почасовая оплата отключена в настройках")});
        return;
    }
    const QString email = settings->GetFourCloudEmail();
    if(email.isEmpty()) {
        if(callback.isCallable())
            callback.call({false, tr("Войдите в аккаунт 4cloud.pro (меню входа)")});
        return;
    }
    const QString host = settings->GetCloudBillingHost();
    if(host.isEmpty()) {
        if(callback.isCallable())
            callback.call({false, tr("Укажите адрес сервера биллинга в настройках облака")});
        return;
    }

    setAllocationProgress(tr("Получение информации об оплате…"));
    const auto quote = CloudBillingClient::quote(
        host, settings->GetCloudBillingPort(), email, serviceType, gameIdentifier, gameName,
        static_cast<qint64>(accountId));
    if(quote.ok)
        noteBillingIdentity(settings, parent(), quote.data);
    const QString message = quote.ok
        ? quote.ui_message
        : (quote.ui_message.isEmpty() ? quote.error : quote.ui_message);
    const double price = quote.data.value(QStringLiteral("hourly_price")).toDouble(0);
    const bool resume = quote.data.value(QStringLiteral("resume_session")).toBool(false);
    const QJsonArray choices = quote.data.value(QStringLiteral("account_choices")).toArray();
    const QString choicesJson = QString::fromUtf8(
        QJsonDocument(choices).toJson(QJsonDocument::Compact));
    if(callback.isCallable())
        callback.call({quote.ok, message, price, resume, choicesJson});
}

// ============================================================================
// MAIN ENTRY POINT - Single method to complete entire flow (Steps 1-13)
// ============================================================================

void CloudStreamingBackend::startCompleteCloudSession(QString serviceType, QString gameIdentifier, const QJSValue &callback)
{
    startCompleteCloudSession(serviceType, gameIdentifier, QString(), QString(), 0, callback);
}

void CloudStreamingBackend::startCompleteCloudSession(QString serviceType, QString gameIdentifier, QString gameName, const QJSValue &callback)
{
    startCompleteCloudSession(serviceType, gameIdentifier, gameName, QString(), 0, callback);
}

void CloudStreamingBackend::startCompleteCloudSession(QString serviceType, QString gameIdentifier, QString gameName, QString platform, const QJSValue &callback)
{
    startCompleteCloudSession(serviceType, gameIdentifier, gameName, platform, 0, callback);
}

void CloudStreamingBackend::startCompleteCloudSession(QString serviceType, QString gameIdentifier, QString gameName, QString platform, quint64 accountId, const QJSValue &callback)
{
    serviceType = serviceType.toLower();
    platform = platform.trimmed().toLower();

    // Validate parameters
    if (serviceType != "psnow" && serviceType != "pscloud") {
        qWarning() << "Invalid serviceType:" << serviceType << "Must be 'psnow' or 'pscloud'";
        if (callback.isCallable()) {
            callback.call({false, QString("Invalid serviceType: %1").arg(serviceType)});
        }
        return;
    }

    CloudLogMessage(QStringLiteral("Session"),
        QStringLiteral("startCompleteCloudSession service=%1 game=%2 platform=%3 account_id=%4")
            .arg(serviceType, gameIdentifier, platform.isEmpty() ? QStringLiteral("-") : platform)
            .arg(accountId));

    // Lookup game image from cache before starting session
    QmlBackend *qmlBackend = qobject_cast<QmlBackend*>(parent());
    if (qmlBackend && qmlBackend->cloudCatalog()) {
        QString imageUrl = qmlBackend->cloudCatalog()->getGameLandscapeImageFromCache(serviceType, gameIdentifier);
        if (!imageUrl.isEmpty()) {
            qInfo() << "Found game landscape image for" << gameIdentifier << ":" << imageUrl;
            setGameImageUrl(imageUrl);
        } else {
            qInfo() << "No game image found in cache for" << gameIdentifier;
            setGameImageUrl(QString()); // Clear any previous image
        }
    } else {
        qWarning() << "Could not access CloudCatalogBackend for image lookup";
        setGameImageUrl(QString()); // Clear any previous image
    }

    last_service_type = serviceType;
    last_game_identifier = gameIdentifier;
    last_game_name = gameName;
    last_platform = platform;

    const bool billing_server = settings
        && settings->GetCloudBillingEnabled()
        && !settings->GetCloudBillingHost().trimmed().isEmpty();
    if (billing_server && settings->GetFourCloudEmail().trimmed().isEmpty()) {
        const QString msg = tr("Войдите в аккаунт 4cloud.pro в приложении — без этого аренда PS-аккаунта и оплата недоступны.");
        CloudLogMessage(QStringLiteral("Session"), QStringLiteral("billing blocked: fourcloud email missing"));
        if (callback.isCallable())
            callback.call({false, msg});
        return;
    }

    QString npssoToken;
    QString billing_error;
    if(runBillingStart(serviceType, gameIdentifier, gameName, &npssoToken, &billing_error, accountId)) {
        if(npssoToken.isEmpty()) {
            qWarning() << "Cloud billing failed:" << billing_error;
            if(callback.isCallable())
                callback.call({false, billing_error});
            return;
        }
        if(!billing_game_identifier.isEmpty() && billing_game_identifier != gameIdentifier) {
            CloudLogMessage(QStringLiteral("Session"),
                QStringLiteral("billing remapped game %1 -> %2 (store %3/%4)")
                    .arg(gameIdentifier, billing_game_identifier,
                         billing_store_country, billing_store_lang));
            gameIdentifier = billing_game_identifier;
            last_game_identifier = gameIdentifier;
        }
        qInfo() << "Cloud billing: using rented PS account NPSSO for this launch only";
    } else {
        qWarning() << "Cloud billing required for NPSSO — local token storage disabled";
        if (callback.isCallable()) {
            callback.call({false, tr("Не удалось получить NPSSO арендованного аккаунта. Проверьте вход в 4cloud и биллинг на сервере.")});
            return;
        }
        return;
    }

    // The C provisioning flow runs the NPSSO authorizeCheck itself as its first
    // (silent) step and surfaces AUTHORIZATION_FAILED (handled in handleProvisionError)
    // if the token is expired -- no separate pre-flight pass is needed here anymore.
    continueCloudSessionAfterAuth(serviceType, gameIdentifier, callback, npssoToken, QString());
}

// Runs the unified C provisioning flow on a worker thread, then hands the
// stream-ready result back to the GUI thread. Kamaji + Gaikai + datacenter
// ping/select + the owned fast-path + the one-shot noGameForEntitlementId retry
// all live in libchiaki (chiaki_cloud_provision_session) now.
void CloudStreamingBackend::continueCloudSessionAfterAuth(QString serviceType, QString gameIdentifier, const QJSValue &callback, QString npssoToken, QString sharedDuid, bool is_reconnect)
{
    if (provision_active.exchange(true)) {
        qInfo() << "Cloud provision already running; will retry after it finishes";
        reconnect_after_provision = true;
        return;
    }

    const bool pscloud = (serviceType == "pscloud");

    // Snapshot everything the worker needs as owned byte arrays (must outlive the thread).
    const QByteArray svc = serviceType.toUtf8();
    const QByteArray gameId = gameIdentifier.toUtf8();
    const QByteArray npsso = npssoToken.toUtf8();
    // Store country/language for the resolve container URL -- byte-faithful to the old
    // Kamaji step0_5d (commit a43e8af2): in native mode (resolvedStoreCountry empty) derive
    // BOTH from the store locale; in fallback mode use the resolved country and the resolved
    // language (else the locale language). Hardcoded US/en would 404 a non-US native store.
    QString loc = settings->GetCloudStoreLocale();
    const QStringList lp = (loc.isEmpty() ? QStringLiteral("en-US") : loc).split('-');
    const QString localeLang = (!lp.isEmpty() && !lp[0].isEmpty()) ? lp[0].toLower() : QStringLiteral("en");
    const QString localeCountry = (lp.size() > 1 && !lp[1].isEmpty()) ? lp[1].toUpper() : QStringLiteral("US");
    const QString resolvedCountry = settings->GetCloudResolvedStoreCountry();
    const QString resolvedLang = settings->GetCloudResolvedStoreLang();
    QString cc, cl;
    // Hourly rental: store locale must follow the rented account region, not the
    // player's personal catalog locale (US UP* SKUs fail on PL/EU NPSSO).
    if(!billing_store_country.isEmpty()) {
        cc = billing_store_country;
        cl = billing_store_lang.isEmpty() ? QStringLiteral("en") : billing_store_lang;
    } else if (!resolvedCountry.isEmpty()) {
        cc = resolvedCountry;
        cl = !resolvedLang.isEmpty() ? resolvedLang : localeLang;
    } else {
        cc = localeCountry;
        cl = localeLang;
    }
    const QByteArray storeCountry = cc.toUtf8();
    const QByteArray storeLang = cl.toUtf8();
    // Voice language chosen in the pre-launch picker. Empty falls back to the
    // saved cloud language, then the store locale.
    QString gameLangStr = launch_language.trimmed();
    if (gameLangStr.isEmpty())
        gameLangStr = settings->GetCloudGameLanguage();
    if (gameLangStr.isEmpty())
        gameLangStr = settings->GetCloudStoreLocale();
    qInfo() << "Cloud launch language:" << gameLangStr;
    const QByteArray gameLang = gameLangStr.toUtf8();
    const QByteArray gameName = last_game_name.toUtf8();
    const QByteArray forcedDc = (pscloud ? settings->GetCloudDatacenterPSCloud()
                                         : settings->GetCloudDatacenterPSNOW()).toUtf8();
    // Prior stored datacenters for this service -> merged with this run's pings by the lib
    // and returned, so the Settings picker keeps previously-measured RTTs (like the old code).
    const QByteArray priorDc = (pscloud ? settings->GetCloudDatacentersJsonPSCloud()
                                        : settings->GetCloudDatacentersJsonPSNOW()).toUtf8();
    const int resolution = pscloud ? settings->GetCloudResolutionPSCloud()
                                    : settings->GetCloudResolutionPSNOW();
    const bool isForeign = settings->IsCloudCatalogIsForeign();
    const bool attrPassed = settings->GetAccountAttributesCheckPassed();

    // Owned-PSNOW fast-path only when provisioning with a non-billing local path.
    // Billing launches always use the NPSSO returned for this title from the VM.
    QByteArray ownedEnt, ownedPlat;
    const bool fromBillingLaunch = !billing_npsso.trimmed().isEmpty()
        && npssoToken.trimmed() == billing_npsso.trimmed();
    if (!pscloud && !is_reconnect && !fromBillingLaunch) {
        QmlBackend *qb = qobject_cast<QmlBackend*>(parent());
        QString e, p;
        if (qb && qb->cloudCatalog() && qb->cloudCatalog()->getOwnedPsnowEntitlement(gameIdentifier, e, p)) {
            qInfo() << "PSNOW owned fast-path: entitlementId=" << e << "platform=" << p;
            ownedEnt = e.toUtf8();
            ownedPlat = p.toUtf8();
        }
    }

    // Bitrate comes from the cloud-streaming settings tab: PS5 games use the PS5
    // slider, PS4 games use the PS4 slider. That value is what allocate sends as BwKbpsSent.
    QString platform = last_platform.trimmed().toLower();
    if (platform.isEmpty() && !ownedPlat.isEmpty())
        platform = QString::fromUtf8(ownedPlat).trimmed().toLower();
    if (platform.isEmpty())
        platform = pscloud ? QStringLiteral("ps5") : QStringLiteral("ps4");
    if (last_platform.isEmpty())
        last_platform = platform;
    const bool ps5_game = platform == QStringLiteral("ps5");
    const int bitrate = settings
        ? static_cast<int>(ps5_game ? settings->GetCloudBitratePSCloud() : settings->GetCloudBitratePSNOW())
        : (ps5_game ? 25000 : 10000);
    qInfo() << "Cloud bitrate for platform" << platform << ":" << bitrate << "kbps";
    Q_UNUSED(sharedDuid); // the C flow generates its own shared DUID for Kamaji+Gaikai

    setAllocationProgress(tr("Starting cloud session..."));

    const quint64 reqId = ++next_request_id;
    pending_callbacks.insert(reqId, callback);
    QPointer<CloudStreamingBackend> self(this);

    std::thread([self, reqId, svc, gameId, gameName, npsso, storeCountry, storeLang, gameLang,
                 forcedDc, priorDc, resolution, bitrate, isForeign, attrPassed, ownedEnt, ownedPlat,
                 fromBillingLaunch]() mutable {
        // Verbose HTTP trace (all request/response headers + bodies) -> chiaki_cloud.log via cc_http_perform.
        // Set CHIAKI_CLOUD_HTTP_TRACE_SECRETS=1 for unredacted npsso/tokens (debug only).
        CloudChiakiLog file_log(CHIAKI_LOG_INFO | CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR
            | CHIAKI_LOG_VERBOSE | CHIAKI_LOG_DEBUG, "Session");
        ChiakiLog *log = file_log.GetChiakiLog();
        CHIAKI_LOGI(log, "provisioning started (service=%s, game=%s, npsso=%s, http_trace=on)",
            svc.constData(), gameId.constData(),
            npsso.isEmpty() ? "missing" : (fromBillingLaunch ? "billing_ephemeral" : "present"));
        CHIAKI_LOGI(log, "HTTP debug: full exchange in this log; secrets redacted unless CHIAKI_CLOUD_HTTP_TRACE_SECRETS=1");

        ChiakiCloudProvisionConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.service_type = svc.constData();
        cfg.game_identifier = gameId.constData();
        cfg.game_name = gameName.constData();
        cfg.npsso = npsso.constData();
        cfg.store_country = storeCountry.constData();
        cfg.store_lang = storeLang.constData();
        cfg.game_language = gameLang.constData();
        cfg.owned_entitlement_id = ownedEnt.constData();
        cfg.owned_platform = ownedPlat.constData();
        cfg.catalog_is_foreign = isForeign;
        cfg.skip_account_attr_check = attrPassed;
        cfg.forced_datacenter = forcedDc.constData();
        cfg.prior_datacenters_json = priorDc.constData();
        cfg.resolution = resolution;
        cfg.bitrate_kbps = bitrate;
        cfg.progress = &CloudStreamingBackend::provisionProgressThunk;
        cfg.is_cancelled = nullptr;
        cfg.user = &self; // address of the lambda-local QPointer — valid for the blocking call's lifetime

        ChiakiCloudProvisionResult res;
        ChiakiErrorCode err = chiaki_cloud_provision_session(&cfg, &res, log);

        const bool success = (err == CHIAKI_ERR_SUCCESS);
        const QString serviceTypeStr = QString::fromUtf8(svc);
        const QString serverIp = QString::fromUtf8(res.server_ip);
        const int serverPort = res.server_port;
        const QString handshakeKey = res.handshake_key ? QString::fromUtf8(res.handshake_key) : QString();
        const QString launchSpec = res.launch_spec ? QString::fromUtf8(res.launch_spec) : QString();
        const QString sessionId = res.session_id ? QString::fromUtf8(res.session_id) : QString();
        const uint8_t wrap = res.psn_wrapper_type;
        const uint32_t mtuIn = res.mtu_in, mtuOut = res.mtu_out;
        const quint64 rttUs = res.rtt_us;
        const QString errMsg = res.error_message ? QString::fromUtf8(res.error_message) : QString();
        const QString dcPings = res.datacenter_pings ? QString::fromUtf8(res.datacenter_pings) : QString();
        chiaki_cloud_provision_result_fini(&res);

        QCoreApplication *app = QCoreApplication::instance();
        if (!app)
            return; // user quit mid-provision: the app object is gone, nothing to deliver to
        QMetaObject::invokeMethod(app, [self, reqId, success, attrPassed, serviceTypeStr, serverIp, serverPort,
                                         handshakeKey, launchSpec, sessionId, wrap, mtuIn, mtuOut, rttUs, errMsg, dcPings]() mutable {
            if (!self)
                return; // backend destroyed while the worker ran
            const QJSValue callback = self->pending_callbacks.take(reqId);
            CloudLogMessage(QStringLiteral("Session"),
                success ? QStringLiteral("provisioning finished: success")
                        : QStringLiteral("provisioning finished: %1")
                              .arg(errMsg.isEmpty() ? QStringLiteral("failed") : errMsg));
            // Persist the merged datacenter list so Settings shows the measured RTTs
            // (done whether or not allocation succeeded -- the old code saved during the ping).
            if (!dcPings.isEmpty()) {
                if (serviceTypeStr == "pscloud") self->settings->SetCloudDatacentersJsonPSCloud(dcPings);
                else self->settings->SetCloudDatacentersJsonPSNOW(dcPings);
            }
            if (success) {
                QString billing_error;
                if(!self->confirmBillingCharge(&billing_error)) {
                    self->abandonBillingReservation();
                    if (callback.isCallable())
                        callback.call({false, billing_error});
                    self->finishProvisionRun();
                    return;
                }
                if (!attrPassed)
                    self->settings->SetAccountAttributesCheckPassed(true);
                self->finishCloudSession(serviceTypeStr, serverIp, serverPort, handshakeKey, launchSpec,
                                         sessionId, wrap, mtuIn, mtuOut, rttUs, callback);
            } else {
                self->abandonBillingReservation();
                self->handleProvisionError(serviceTypeStr, errMsg, callback);
            }
            self->finishProvisionRun();
        }, Qt::QueuedConnection);
    }).detach();
}

// Build StreamSessionConnectInfo from the C result and start the StreamSession.
// This boundary (and everything below it) is unchanged from the previous flow --
// only the source of the parameters moved from PSGaikaiStreaming to the C result.
void CloudStreamingBackend::finishCloudSession(QString serviceType, QString serverIp, int serverPort,
                                               QString handshakeKey, QString launchSpec, QString sessionId,
                                               uint8_t psnWrapperType, uint32_t mtuIn, uint32_t mtuOut, uint64_t rttUs,
                                               const QJSValue &callback)
{
    qInfo() << "=== COMPLETE CLOUD SESSION SUCCESS ===";
    qInfo() << "  IP:" << serverIp << " Port:" << serverPort << " SessionId len:" << sessionId.length();

    if (QmlBackend *qmlBackend = qobject_cast<QmlBackend*>(parent())) {
        if (CloudCatalogBackend *catalog = qmlBackend->cloudCatalog()) {
            catalog->recordRecentPlay(last_game_identifier, serviceType, last_game_name);
        }
    }

    qInfo() << "  handshake len:" << handshakeKey.length() << " launchSpec len:" << launchSpec.length();

    // PSCLOUD streams as PS5, PSNOW (PS3 + PS4) as PS4.
    const ChiakiTarget target = (serviceType == "pscloud") ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_9;

    // Read window type from settings (same as remote play)
    bool fullscreen = false, zoom = false, stretch = false;
    switch (settings->GetWindowType()) {
    case WindowType::SelectedResolution:
    case WindowType::CustomResolution:
    case WindowType::AdjustableResolution:
        break;
    case WindowType::Fullscreen: fullscreen = true; break;
    case WindowType::Zoom: zoom = true; break;
    case WindowType::Stretch: stretch = true; break;
    default: break;
    }

    // Pass host as "IP:PORT"; StreamSession extracts the port for cloud mode.
    StreamSessionConnectInfo connect_info(
        settings,
        target,
        QString("%1:%2").arg(serverIp).arg(serverPort),
        QString(),     // nickname
        QByteArray(),  // regist_key (not used for cloud)
        QByteArray(),  // morning (not used for cloud)
        QString(),     // initial_login_pin
        QString(),     // duid (not used for cloud, direct connection)
        false,         // auto_regist
        fullscreen, zoom, stretch);

    connect_info.cloud_launch_spec = launchSpec;
    connect_info.cloud_handshake_key = handshakeKey;
    connect_info.cloud_session_id = sessionId;
    if (serviceType == "pscloud")
        connect_info.service_type = CHIAKI_SERVICE_TYPE_PSCLOUD;
    else if (serviceType == "psnow")
        connect_info.service_type = CHIAKI_SERVICE_TYPE_PSNOW;
    else
        connect_info.service_type = CHIAKI_SERVICE_TYPE_REMOTE_PLAY;
    connect_info.cloud_psn_wrapper_type = psnWrapperType;
    connect_info.cloud_mtu_in = mtuIn;
    connect_info.cloud_mtu_out = mtuOut;
    connect_info.cloud_rtt_us = rttUs;
    connect_info.video_profile = settings->GetCloudVideoProfile(serviceType);

    qInfo() << "Cloud streaming parameters set:";
    qInfo() << "  service_type:" << chiaki_service_type_string(connect_info.service_type);
    qInfo() << "  bitrate_kbps:" << connect_info.video_profile.bitrate;
    qInfo() << "  cloud_psn_wrapper_type:" << QString("0x%1").arg(connect_info.cloud_psn_wrapper_type, 2, 16, QChar('0'));
    qInfo() << "  mtu_in:" << mtuIn << " mtu_out:" << mtuOut << " rtt_us:" << rttUs;

    // Resolve "auto" hardware decoder to an actual decoder.
    if (connect_info.hw_decoder == "auto") {
        connect_info.hw_decoder = QString();
        static QSet<QString> allowed = {
            "vulkan",
#if defined(Q_OS_LINUX)
            "vaapi",
#elif defined(Q_OS_MACOS)
            "videotoolbox",
#elif defined(Q_OS_WIN)
            "d3d11va",
#endif
        };
        enum AVHWDeviceType hw_dev = AV_HWDEVICE_TYPE_NONE;
        QStringList available;
        while (true) {
            hw_dev = av_hwdevice_iterate_types(hw_dev);
            if (hw_dev == AV_HWDEVICE_TYPE_NONE)
                break;
            const QString name = QString::fromUtf8(av_hwdevice_get_type_name(hw_dev));
            if (allowed.contains(name))
                available.append(name);
        }
        if (available.contains("vulkan")) {
            connect_info.hw_decoder = "vulkan";
            qInfo() << "Auto-selected hardware decoder: vulkan";
        }
#if defined(Q_OS_LINUX)
        else if (available.contains("vaapi")) {
            connect_info.hw_decoder = "vaapi";
            qInfo() << "Auto-selected hardware decoder: vaapi";
        }
#elif defined(Q_OS_WIN)
        else if (available.contains("d3d11va")) {
            connect_info.hw_decoder = "d3d11va";
            qInfo() << "Auto-selected hardware decoder: d3d11va";
        }
#elif defined(Q_OS_MACOS)
        else if (available.contains("videotoolbox")) {
            connect_info.hw_decoder = "videotoolbox";
            qInfo() << "Auto-selected hardware decoder: videotoolbox";
        }
#endif
        else {
            qInfo() << "No hardware decoder available, using software decoding";
        }
    }

    qInfo() << "=== Creating StreamSession ===";
    try {
        StreamSession *session = new StreamSession(connect_info, parent());
        emit sessionCreated(session);

        setAllocationProgress("");
        if(billing_minutes_left > 0)
            setBillingMinutesOnly(billing_minutes_left);
        session->Start();
        qInfo() << "StreamSession Start() called (connection is asynchronous)";

        if (callback.isCallable()) {
            callback.call({
                true,
                "Cloud session connection initiated (waiting for server response...)",
                serverIp
            });
        }
    } catch (const Exception &e) {
        qWarning() << "Failed to start cloud streaming session:" << e.what();
        setGameImageUrl(QString());
        // Same dismissal contract as handleProvisionError: without sessionError the
        // loading page has no error text, no Escape handler, and never exits.
        if (QmlBackend *qmlBackend = qobject_cast<QmlBackend*>(parent())) {
            qmlBackend->setCloudSessionReconnecting(false);
            emit qmlBackend->sessionError(tr("Cloud Streaming Failed"),
                QString("Failed to start session: %1").arg(e.what()));
        }
        if (callback.isCallable()) {
            callback.call({false, QString("Failed to start session: %1").arg(e.what())});
        }
        setAllocationProgress("");
    }
}

// Map the C error_message sentinels to the same dialogs the old flow raised.
void CloudStreamingBackend::handleProvisionError(QString serviceType, QString errorMessage, const QJSValue &callback)
{
    Q_UNUSED(serviceType);
    qWarning() << "Cloud provisioning failed:" << errorMessage;
    setGameImageUrl(QString());

    // Set the specific dialog (supplementary), then ALWAYS emit sessionError so the
    // stream/loading page dismisses and returns to the main menu -- the original
    // emitted both its special signal AND AllocationError/sessionComplete(false)
    // (which fired sessionError). Without the sessionError the page never exits and
    // the dialog just toasts on the streaming page.
    QString userMessage;
    QmlBackend *qmlBackend = qobject_cast<QmlBackend*>(parent());
    if (errorMessage.contains(QStringLiteral("AUTHORIZATION_FAILED"))) {
        if (qmlBackend) qmlBackend->setShowAuthorizationFailedDialog(true);
        userMessage = tr("Your NPSSO token is likely expired. Please re-login to continue using cloud streaming.");
    } else if (errorMessage.contains(QStringLiteral("PS_PLUS_SUBSCRIPTION_REQUIRED"))) {
        if (qmlBackend) qmlBackend->setShowPSPlusSubscriptionDialog(true);
        userMessage = tr("PS Plus subscription required");
    } else if (errorMessage.contains(QStringLiteral("ACCOUNT_PRIVACY_SETTINGS"))) {
        // Sentinel is "ACCOUNT_PRIVACY_SETTINGS:<upgrade-url>" (URL omitted when no
        // missing elements were parsed). Extract the URL for the dialog.
        const QString prefix = QStringLiteral("ACCOUNT_PRIVACY_SETTINGS:");
        QString upgradeUrl;
        int idx = errorMessage.indexOf(prefix);
        if (idx >= 0)
            upgradeUrl = errorMessage.mid(idx + prefix.length());
        if (qmlBackend) {
            qmlBackend->setAccountPrivacyUpgradeUrl(upgradeUrl);
            qmlBackend->setShowAccountPrivacySettingsDialog(true);
        }
        userMessage = tr("Account privacy settings need updating");
    } else if (errorMessage.contains(QStringLiteral("PING_TIMEOUT"))) {
        if (qmlBackend) qmlBackend->setShowPingTimeoutDialog(true);
        userMessage = tr("Ping must be < 80ms to start a cloud session");
    } else if (errorMessage.contains(QStringLiteral("GAME_NOT_FREE"))) {
        // Stale catalog: a title that was a free PS+ offer now costs money. Sentinel is
        // "GAME_NOT_FREE:<price>" (price may be empty). Tell the user to refresh.
        const QString prefix = QStringLiteral("GAME_NOT_FREE:");
        QString price;
        int idx = errorMessage.indexOf(prefix);
        if (idx >= 0) price = errorMessage.mid(idx + prefix.length()).trimmed();
        userMessage = price.isEmpty()
            ? tr("This game is no longer free to stream. Your game list may be out of date — refresh it and try again.")
            : tr("This game is no longer free to stream (price: %1). Your game list may be out of date — refresh it and try again.").arg(price);
    } else {
        userMessage = errorMessage.isEmpty() ? tr("Allocation failed")
                                             : QString("Allocation failed: %1").arg(errorMessage);
    }

    if (qmlBackend) {
        qmlBackend->setCloudSessionReconnecting(false);
        emit qmlBackend->sessionError(tr("Cloud Streaming Failed"), userMessage);
    }

    if (callback.isCallable()) {
        callback.call({false, userMessage});
    }

    setAllocationProgress("");
}

// C progress callback -- runs on the worker thread; marshal to the GUI thread.
void CloudStreamingBackend::provisionProgressThunk(const char *stage, void *user)
{
    auto *holder = static_cast<QPointer<CloudStreamingBackend>*>(user);
    if (!holder || !stage)
        return;
    // NOTE: copying a QPointer off the GUI thread is not strictly thread-safe (it touches the
    // QWeakPointer control block, which the GUI thread mutates on destruction). It is safe HERE
    // only because CloudStreamingBackend is owned by QmlBackend and outlives every provision, so
    // it is never destroyed concurrently with a progress callback. Do not copy this pattern to a
    // backend with a shorter lifetime.
    QPointer<CloudStreamingBackend> self = *holder;
    const QString s = QString::fromUtf8(stage);
    QCoreApplication *app = QCoreApplication::instance();
    if (!app)
        return; // user quit mid-provision: the app object is gone, nothing to deliver to
    QMetaObject::invokeMethod(app, [self, s]() {
        if (self)
            self->setAllocationProgress(s);
    }, Qt::QueuedConnection);
}

void CloudStreamingBackend::onAllocationProgress(QString message)
{
    setAllocationProgress(message);
}


void CloudStreamingBackend::setAllocationProgress(const QString &message)
{
    if (allocation_progress != message) {
        allocation_progress = message;
        emit allocationProgressChanged();
    }
}

void CloudStreamingBackend::setGameImageUrl(const QString &url)
{
    if (game_image_url != url) {
        game_image_url = url;
        emit gameImageUrlChanged();
    }
}

static const struct {
    const char *code;
    const char *label;
} kVoiceLanguages[] = {
    {"ru", "Русский"},
    {"en", "Английский"},
    {"ja", "Японский"},
    {"de", "Немецкий"},
    {"fr", "Французский"},
    {"es", "Испанский"},
    {"it", "Итальянский"},
    {"pt", "Португальский"},
    {"pl", "Польский"},
    {"ko", "Корейский"},
    {"zh-Hans", "Китайский (упрощённый)"},
    {"zh-Hant", "Китайский (традиционный)"},
    {"ar", "Арабский"},
    {"nl", "Нидерландский"},
    {"tr", "Турецкий"},
    {"sv", "Шведский"},
    {"da", "Датский"},
    {"no", "Норвежский"},
    {"fi", "Финский"},
    {"cs", "Чешский"},
    {"hu", "Венгерский"},
    {"el", "Греческий"},
    {"ro", "Румынский"},
    {"th", "Тайский"},
};

static QString normalizeVoiceCode(const QString &raw)
{
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty())
        return QString();
    if (trimmed.compare(QStringLiteral("zh-Hant"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("zh-Hant");
    if (trimmed.compare(QStringLiteral("zh-Hans"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("zh-Hans");

    const int dash = trimmed.indexOf(QLatin1Char('-'));
    const int under = trimmed.indexOf(QLatin1Char('_'));
    int cut = -1;
    if (dash >= 0 && under >= 0)
        cut = qMin(dash, under);
    else
        cut = qMax(dash, under);
    QString base = (cut > 0 ? trimmed.left(cut) : trimmed).toLower();
    if (base == QLatin1String("jp"))
        base = QStringLiteral("ja");
    if (base == QLatin1String("zh") || base == QLatin1String("ch")) {
        if (trimmed.contains(QStringLiteral("Hant"), Qt::CaseInsensitive)
            || trimmed.contains(QStringLiteral("TW"), Qt::CaseInsensitive)
            || trimmed.contains(QStringLiteral("HK"), Qt::CaseInsensitive))
            return QStringLiteral("zh-Hant");
        return QStringLiteral("zh-Hans");
    }
    for (const auto &lang : kVoiceLanguages) {
        if (base == QLatin1String(lang.code))
            return base;
    }
    return QString();
}

static void collectVoiceCodesFromValue(const QJsonValue &value, QSet<QString> *out)
{
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (const QJsonValue &item : array)
            collectVoiceCodesFromValue(item, out);
        return;
    }
    if (!value.isObject())
        return;
    const QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.key() == QLatin1String("voiceLanguageCode") || it.key() == QLatin1String("voice_language_codes")) {
            const QJsonArray codes = it.value().toArray();
            for (const QJsonValue &code : codes) {
                if (code.isString())
                    out->insert(code.toString());
            }
        } else {
            collectVoiceCodesFromValue(it.value(), out);
        }
    }
}

static QSet<QString> voiceCodesFromContainer(const QByteArray &body, const QString &productId)
{
    QSet<QString> matched;
    QSet<QString> all;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject())
        return all;
    const QJsonObject root = doc.object();

    auto takeEntitlement = [&](const QJsonObject &entitlement) {
        QSet<QString> codes;
        collectVoiceCodesFromValue(entitlement, &codes);
        if (codes.isEmpty())
            return;
        all.unite(codes);
        if (!productId.isEmpty() && entitlement.value(QStringLiteral("id")).toString() == productId)
            matched.unite(codes);
    };
    auto takeSku = [&](const QJsonObject &sku) {
        const QJsonArray entitlements = sku.value(QStringLiteral("entitlements")).toArray();
        for (const QJsonValue &entitlement : entitlements) {
            if (entitlement.isObject())
                takeEntitlement(entitlement.toObject());
        }
    };

    const QJsonObject defaultSku = root.value(QStringLiteral("default_sku")).toObject();
    if (!defaultSku.isEmpty())
        takeSku(defaultSku);
    const QJsonArray skus = root.value(QStringLiteral("skus")).toArray();
    for (const QJsonValue &sku : skus) {
        if (sku.isObject())
            takeSku(sku.toObject());
    }
    if (matched.isEmpty() && all.isEmpty())
        collectVoiceCodesFromValue(root, &all);
    return matched.isEmpty() ? all : matched;
}

static QSet<QString> voiceCodesFromStoreHtml(const QByteArray &body, const QString &platformKey)
{
    const QString html = QString::fromUtf8(body);
    QSet<QString> codes;
    const QRegularExpression platformRe(
        QStringLiteral("\"platform\"\\s*:\\s*\"%1\"\\s*,\\s*\"spokenLanguages\"\\s*:\\s*\\[([^\\]]*)\\]").arg(platformKey));
    const QRegularExpressionMatch platformMatch = platformRe.match(html);
    QString list;
    if (platformMatch.hasMatch())
        list = platformMatch.captured(1);
    else {
        const QRegularExpression topRe(QStringLiteral("\"spokenLanguages\"\\s*:\\s*\\[([^\\]]*)\\]"));
        const QRegularExpressionMatch topMatch = topRe.match(html);
        if (topMatch.hasMatch())
            list = topMatch.captured(1);
    }
    const QRegularExpression codeRe(QStringLiteral("\"([^\"]+)\""));
    auto it = codeRe.globalMatch(list);
    while (it.hasNext())
        codes.insert(it.next().captured(1));
    return codes;
}

static QString voiceLanguageJson(const QSet<QString> &rawCodes)
{
    QSet<QString> normalized;
    for (const QString &raw : rawCodes) {
        const QString code = normalizeVoiceCode(raw);
        if (!code.isEmpty())
            normalized.insert(code);
    }
    QJsonArray out;
    for (const auto &lang : kVoiceLanguages) {
        const QString code = QString::fromLatin1(lang.code);
        if (!normalized.contains(code))
            continue;
        QJsonObject row;
        row.insert(QStringLiteral("code"), code);
        row.insert(QStringLiteral("label"), QString::fromUtf8(lang.label));
        out.append(row);
    }
    return QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact));
}

struct VoiceLanguageFetch {
    QPointer<CloudStreamingBackend> self;
    quint64 generation = 0;
    QStringList urls;
    QStringList kinds;
    int index = 0;
    QString platformKey;
    QString productId;
    QJSValue callback;
    QNetworkAccessManager *nam = nullptr;
    QNetworkReply *reply = nullptr;
    QTimer *timer = nullptr;
};

static void voiceLanguageDeliver(const VoiceLanguageFetch &fetch, const QString &json)
{
    if (!fetch.self || fetch.generation != fetch.self->voiceFetchGeneration())
        return;
    if (fetch.callback.isCallable())
        fetch.callback.call({json});
}

static void voiceLanguageStep(VoiceLanguageFetch *fetch);

static void voiceLanguageDiscard(VoiceLanguageFetch *fetch)
{
    if (fetch->reply)
        fetch->reply->deleteLater();
    if (fetch->timer)
        fetch->timer->deleteLater();
    delete fetch;
}

static void voiceLanguageFinishReply(VoiceLanguageFetch *fetch)
{
    if (!fetch->self || fetch->generation != fetch->self->voiceFetchGeneration()) {
        voiceLanguageDiscard(fetch);
        return;
    }
    QByteArray body;
    int status = 0;
    if (fetch->reply) {
        status = fetch->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (fetch->reply->error() == QNetworkReply::NoError)
            body = fetch->reply->readAll();
        fetch->reply->deleteLater();
        fetch->reply = nullptr;
    }
    if (fetch->timer) {
        fetch->timer->stop();
        fetch->timer->deleteLater();
        fetch->timer = nullptr;
    }

    const bool storePage = fetch->index < fetch->kinds.size() && fetch->kinds.at(fetch->index) == QLatin1String("store");
    QSet<QString> found;
    if (status == 200 && !body.isEmpty())
        found = storePage ? voiceCodesFromStoreHtml(body, fetch->platformKey)
                           : voiceCodesFromContainer(body, fetch->productId);
    if (!found.isEmpty()) {
        const QString json = voiceLanguageJson(found);
        qInfo() << "Cloud voice languages:" << json;
        voiceLanguageDeliver(*fetch, json);
        delete fetch;
        return;
    }

    fetch->index++;
    voiceLanguageStep(fetch);
}

static void voiceLanguageStep(VoiceLanguageFetch *fetch)
{
    if (!fetch->self || fetch->generation != fetch->self->voiceFetchGeneration()) {
        voiceLanguageDiscard(fetch);
        return;
    }
    if (fetch->index >= fetch->urls.size()) {
        qInfo() << "Cloud voice languages: none published for" << fetch->productId;
        voiceLanguageDeliver(*fetch, QStringLiteral("[]"));
        delete fetch;
        return;
    }

    QNetworkRequest request{QUrl(fetch->urls.at(fetch->index))};
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"));
    request.setRawHeader("Accept", fetch->kinds.at(fetch->index) == QLatin1String("store")
        ? "text/html" : "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    fetch->reply = fetch->nam->get(request);
    fetch->timer = new QTimer(fetch->self);
    fetch->timer->setSingleShot(true);
    QObject::connect(fetch->timer, &QTimer::timeout, fetch->self, [fetch]() {
        if (fetch->reply)
            fetch->reply->abort();
    });
    QObject::connect(fetch->reply, &QNetworkReply::finished, fetch->self, [fetch]() {
        voiceLanguageFinishReply(fetch);
    });
    fetch->timer->start(12000);
}

void CloudStreamingBackend::setLaunchLanguage(QString language)
{
    launch_language = language.trimmed();
    qInfo() << "Cloud voice language selected:"
            << (launch_language.isEmpty() ? QStringLiteral("(catalog default)") : launch_language);
}

void CloudStreamingBackend::fetchGameVoiceLanguages(QString productId, QString streamId, QString platform, const QJSValue &callback)
{
    productId = productId.trimmed();
    streamId = streamId.trimmed();
    const QString platformNorm = platform.trimmed().toLower();
    QString platformKey = QStringLiteral("PS5");
    if (platformNorm == QLatin1String("ps4"))
        platformKey = QStringLiteral("PS4");
    else if (platformNorm == QLatin1String("ps3"))
        platformKey = QStringLiteral("PS3");

    QStringList ids;
    if (!productId.isEmpty())
        ids.append(productId);
    if (!streamId.isEmpty() && streamId != productId)
        ids.append(streamId);
    if (ids.isEmpty()) {
        if (callback.isCallable())
            callback.call({QStringLiteral("[]")});
        return;
    }

    if (!voice_nam)
        voice_nam = new QNetworkAccessManager(this);
    const quint64 generation = ++voice_fetch_generation;

    auto *fetch = new VoiceLanguageFetch;
    fetch->self = this;
    fetch->generation = generation;
    fetch->platformKey = platformKey;
    fetch->productId = ids.first();
    fetch->callback = callback;
    fetch->nam = voice_nam;
    for (const QString &id : ids) {
        const QString enc = QString::fromUtf8(QUrl::toPercentEncoding(id));
        for (const char *country : {"GB", "US"}) {
            fetch->urls.append(QStringLiteral(
                "https://psnow.playstation.com/store/api/pcnow/00_09_000/container/%1/en/19/%2?useOffers=true&gkb=1&gkb2=1")
                .arg(QLatin1String(country), enc));
            fetch->kinds.append(QStringLiteral("container"));
        }
    }
    for (const QString &id : ids) {
        const QString enc = QString::fromUtf8(QUrl::toPercentEncoding(id));
        for (const char *locale : {"en-us", "en-gb"}) {
            fetch->urls.append(QStringLiteral("https://store.playstation.com/%1/product/%2")
                .arg(QLatin1String(locale), enc));
            fetch->kinds.append(QStringLiteral("store"));
        }
    }
    voiceLanguageStep(fetch);
}

void CloudStreamingBackend::finishProvisionRun(bool schedule_pending_reconnect)
{
    provision_active = false;
    if (!schedule_pending_reconnect || !reconnect_after_provision)
        return;
    reconnect_after_provision = false;
    if (last_service_type.isEmpty() || last_game_identifier.isEmpty())
        return;
    qInfo() << "Running queued cloud reconnect after previous provision finished";
    QTimer::singleShot(1500, this, [this]() { reconnectCurrentSession(); });
}

void CloudStreamingBackend::reconnectCurrentSession()
{
    if (last_service_type.isEmpty() || last_game_identifier.isEmpty()) {
        qWarning() << "reconnectCurrentSession: no previous cloud session";
        return;
    }

    const QString npsso = billing_npsso;
    if (npsso.isEmpty()) {
        qWarning() << "reconnectCurrentSession: no NPSSO — cannot reconnect without re-start";
        return;
    }
    CloudLogMessage(QStringLiteral("Session"),
        QStringLiteral("reconnecting cloud session (service=%1, game=%2, npsso=billing_ephemeral) to apply new settings")
            .arg(last_service_type, last_game_identifier));
    setAllocationProgress(tr("Applying settings - reconnecting..."));
    continueCloudSessionAfterAuth(last_service_type, last_game_identifier, QJSValue(), npsso, QString(), true);
}

