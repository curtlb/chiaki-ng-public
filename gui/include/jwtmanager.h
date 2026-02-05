#pragma once

#include <QObject>
#include <QString>
#include <QSettings>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>

class JwtManager : public QObject
{
    Q_OBJECT

public:
    explicit JwtManager(QObject *parent = nullptr);
    ~JwtManager();

    // JWT management
    static void saveJwt(const QString &jwt, const QString &login = QString());
    static QString getJwt();
    static QString getUserLogin();
    static bool isJwtSaved();
    static void clearJwt();
    static void logout();

    // JWT validation
    void validateJwt(std::function<void(bool)> callback);
    
    // User data
    static QString getPsnId();
    static QString getEmail();
    static QString getNp();
    static QString getDateExp();
    static QString getTimeUntilExpiration();
    static QString getCachedAvatarUrl();
    
    // Avatar loading
    void loadAvatar(std::function<void(const QString &)> callback);
    
    // Console status
    void getConsoleStatus(const QString &np, std::function<void(const QString &)> callback);
    
    // Wakeup via API
    void sendWakeupViaApi(const QString &np);
    
    // Date expiration update
    void updateDateExpIfNeeded(std::function<void()> callback = nullptr);

signals:
    void avatarLoaded(const QString &url);

private:
    static QSettings* getSettings();
    static void savePsnId(const QString &psnId);
    static void saveEmail(const QString &email);
    static void saveNp(const QString &np);
    static void saveDateExp(const QString &dateExp);
    static void saveAvatarUrl(const QString &url);
    
    QString calculateTimeUntilExpiration(const QString &dateExpStr);
    
    QNetworkAccessManager *network_manager;
    bool is_updating_date_exp;
    
    static const QString PREFS_NAME;
    static const QString KEY_JWT;
    static const QString KEY_USER_LOGIN;
    static const QString KEY_PSN_ID;
    static const QString KEY_AVATAR_URL;
    static const QString KEY_DATE_EXP;
    static const QString KEY_NP;
    static const QString KEY_EMAIL;
    static const QString DECODE_JWT_URL;
    static const QString PSN_AVATAR_URL;
    static const QString DEFAULT_AVATAR_URL;
};
