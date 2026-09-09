// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CLOUDSTREAMINGBACKEND_H
#define CLOUDSTREAMINGBACKEND_H

#include "settings.h"

#include <QObject>
#include <QString>
#include <QJSValue>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <atomic>

// ============================================================================
// CONFIGURATION - Shared settings and values used by multiple classes
// ============================================================================
namespace CloudConfig {
    // Shared base values (used by both PSNOW and PSCLOUD)
    static const QString ACCOUNT_BASE = "https://ca.account.sony.com/api";
}

/**
 * CloudStreamingBackend - Orchestrates PlayStation Plus Cloud Gaming flow
 * 
 * This class is the main entry point for cloud gaming. It:
 * - Holds shared configuration (CloudConfig namespace in header)
 * - Runs the whole provisioning flow (auth check, Kamaji resolve, Gaikai
 *   allocation, datacenter ping/select) in libchiaki via
 *   chiaki_cloud_provision_session, on a worker thread
 * - Provides a single unified API for the frontend
 *
 * Architecture:
 *   CloudStreamingBackend (thin Qt wrapper)
 *     └─> libchiaki chiaki_cloud_provision_session (the unified C flow)
 */
class StreamSession; // Forward declaration

class CloudStreamingBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString allocationProgress READ getAllocationProgress NOTIFY allocationProgressChanged)
    Q_PROPERTY(QString gameImageUrl READ getGameImageUrl WRITE setGameImageUrl NOTIFY gameImageUrlChanged)
    Q_PROPERTY(QString billingStatusMessage READ billingStatusMessage NOTIFY billingStatusChanged)
    Q_PROPERTY(int billingMinutesLeft READ billingMinutesLeft NOTIFY billingStatusChanged)

public:
    explicit CloudStreamingBackend(Settings *settings, QObject *parent = nullptr);
    ~CloudStreamingBackend() override;

    // Rebind to a new profile's Settings (profile switch deletes the old object).
    void setSettings(Settings *new_settings) { settings = new_settings; }

    // MAIN ENTRY POINT - Complete cloud streaming session (Steps 1-13)
    // Parameters:
    //   serviceType: "psnow" or "pscloud"
    //   gameIdentifier: Product ID (PSNOW) or Entitlement ID (PSCLOUD)
    // Platform is automatically detected from API response for PSNOW, or hardcoded to "ps5" for PSCLOUD
    Q_INVOKABLE void startCompleteCloudSession(QString serviceType, QString gameIdentifier, const QJSValue &callback);
    Q_INVOKABLE void startCompleteCloudSession(QString serviceType, QString gameIdentifier, QString gameName, const QJSValue &callback);
    Q_INVOKABLE void startCompleteCloudSession(QString serviceType, QString gameIdentifier, QString gameName, QString platform, const QJSValue &callback);
    Q_INVOKABLE void startCompleteCloudSession(QString serviceType, QString gameIdentifier, QString gameName, QString platform, quint64 accountId, const QJSValue &callback);

    /** Fetch hourly billing quote (no charge). Callback: ok, message, hourlyPrice, resumeSession, accountChoicesJson.
     *  Optional accountId skips multi-account choice for that PS account. */
    Q_INVOKABLE void fetchBillingQuote(QString serviceType, QString gameIdentifier, QString gameName, const QJSValue &callback);
    Q_INVOKABLE void fetchBillingQuote(QString serviceType, QString gameIdentifier, QString gameName, quint64 accountId, const QJSValue &callback);

    /** Re-run Gaikai allocation for the last-started cloud game (e.g. after bitrate change). */
    Q_INVOKABLE void reconnectCurrentSession();

    /** Heartbeat for hourly billing (call periodically while streaming). */
    Q_INVOKABLE void sendBillingHeartbeat(bool streaming);

    /** Notify billing service that the user stopped the stream (end_stream). */
    Q_INVOKABLE void notifyStreamStopped();
    
    QString getAllocationProgress() const { return allocation_progress; }
    QString getGameImageUrl() const { return game_image_url; }
    void setGameImageUrl(const QString &url);
    QString billingStatusMessage() const { return billing_status_message; }
    int billingMinutesLeft() const { return billing_minutes_left; }

signals:
    // Emitted when a cloud streaming session is created and ready to be registered
    void sessionCreated(StreamSession *session);
    // Emitted when allocation progress updates
    void allocationProgressChanged();
    // Emitted when game image URL changes
    void gameImageUrlChanged();
    void billingStatusChanged();
    /** Emitted after cloud stream ends with save-freeze info from billing server. */
    void saveRetentionDialogRequested(QString message);

private slots:
    void onAllocationProgress(QString message);
    void onBillingHeartbeatTick();

private:
    void setAllocationProgress(const QString &message);
    void setBillingStatus(const QString &message, int minutes_left = -1);
    void setBillingMinutesOnly(int minutes_left);
    void startBillingHeartbeat();
    void stopBillingHeartbeat();
    bool runBillingStart(QString serviceType, QString gameIdentifier, QString gameName, QString *out_npsso, QString *out_error, quint64 account_id = 0);
    bool confirmBillingCharge(QString *out_error);
    void abandonBillingReservation();

    // Continue cloud session: runs the unified C
    // provisioning flow (chiaki_cloud_provision_session) on a worker thread and
    // hands the stream-ready result to StreamSession. Kamaji+Gaikai, the owned
    // fast-path and the one-shot noGameForEntitlementId retry all live in libchiaki.
    void continueCloudSessionAfterAuth(QString serviceType, QString gameIdentifier, const QJSValue &callback, QString npssoToken, QString sharedDuid, bool is_reconnect = false);

    // Build StreamSessionConnectInfo from a successful provision result and start the session.
    void finishCloudSession(QString serviceType, QString serverIp, int serverPort,
                            QString handshakeKey, QString launchSpec, QString sessionId,
                            uint8_t psnWrapperType, uint32_t mtuIn, uint32_t mtuOut, uint64_t rttUs,
                            const QJSValue &callback);
    // Map a provisioning failure (error_message sentinels) to the right UI dialog.
    void handleProvisionError(QString serviceType, QString errorMessage, const QJSValue &callback);
    // C progress callback (called from the worker thread): marshals to setAllocationProgress.
    static void provisionProgressThunk(const char *stage, void *user);

    void finishProvisionRun(bool schedule_pending_reconnect = true);

    Settings *settings;
    QString allocation_progress;
    QString game_image_url;  // Landscape image URL for current cloud game
    QString last_service_type;
    QString last_game_identifier;
    QString last_game_name;
    QString last_platform;
    QString billing_session_token;
    QString billing_npsso;
    QString billing_game_identifier;
    QString billing_store_country;
    QString billing_store_lang;
    bool billing_payment_pending = false;
    QString billing_status_message;
    int billing_minutes_left = 0;
    QTimer billing_heartbeat_timer;
    std::atomic<bool> billing_heartbeat_inflight{false};

    QHash<quint64, QJSValue> pending_callbacks; // GUI thread only
    quint64 next_request_id = 0;
    std::atomic<bool> provision_active{false};
    bool reconnect_after_provision = false;
};

#endif // CLOUDSTREAMINGBACKEND_H
