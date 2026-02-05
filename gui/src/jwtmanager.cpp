#include "jwtmanager.h"
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QLoggingCategory>
#include <QUrlQuery>
#include <functional>

Q_DECLARE_LOGGING_CATEGORY(chiakiGui);

const QString JwtManager::PREFS_NAME = "chiaki_auth";
const QString JwtManager::KEY_JWT = "jwt_token";
const QString JwtManager::KEY_USER_LOGIN = "user_login";
const QString JwtManager::KEY_PSN_ID = "psn_id";
const QString JwtManager::KEY_AVATAR_URL = "avatar_url";
const QString JwtManager::KEY_DATE_EXP = "date_exp";
const QString JwtManager::KEY_NP = "np";
const QString JwtManager::KEY_EMAIL = "email";
const QString JwtManager::DECODE_JWT_URL = "https://4cloud.pro/api.php";
const QString JwtManager::PSN_AVATAR_URL = "https://4cloud.pro/api/psn.php";
const QString JwtManager::DEFAULT_AVATAR_URL = "https://static-resource.np.community.playstation.net/avatar/WWS_E/E2098_l.png";

JwtManager::JwtManager(QObject *parent)
    : QObject(parent)
    , network_manager(new QNetworkAccessManager(this))
    , is_updating_date_exp(false)
{
}

JwtManager::~JwtManager()
{
}

QSettings* JwtManager::getSettings()
{
    QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (configPath.isEmpty()) {
        qCWarning(chiakiGui) << "JwtManager: Cannot get config location, using fallback";
        configPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    QDir configDir(configPath);
    if (!configDir.exists("Chiaki")) {
        if (!configDir.mkpath("Chiaki")) {
            qCWarning(chiakiGui) << "JwtManager: Cannot create Chiaki config directory";
        }
    }
    QString settingsPath = configPath + "/Chiaki/auth.ini";
    return new QSettings(settingsPath, QSettings::IniFormat);
}

void JwtManager::saveJwt(const QString &jwt, const QString &login)
{
    QSettings *settings = getSettings();
    settings->setValue(KEY_JWT, jwt);
    if (!login.isEmpty()) {
        settings->setValue(KEY_USER_LOGIN, login);
    }
    settings->sync();
    delete settings;
}

QString JwtManager::getJwt()
{
    QSettings *settings = getSettings();
    QString jwt = settings->value(KEY_JWT).toString();
    delete settings;
    return jwt;
}

QString JwtManager::getUserLogin()
{
    QSettings *settings = getSettings();
    QString login = settings->value(KEY_USER_LOGIN).toString();
    delete settings;
    return login;
}

bool JwtManager::isJwtSaved()
{
    return !getJwt().isEmpty();
}

void JwtManager::clearJwt()
{
    QSettings *settings = getSettings();
    settings->clear();
    settings->sync();
    delete settings;
}

void JwtManager::logout()
{
    clearJwt();
}

void JwtManager::validateJwt(std::function<void(bool)> callback)
{
    QString jwt = getJwt();
    if (jwt.isEmpty()) {
        callback(false);
        return;
    }

    QUrl url(DECODE_JWT_URL);
    QUrlQuery query;
    query.addQueryItem("method", "decode-jwt");
    query.addQueryItem("jwt", jwt);
    url.setQuery(query);

    QNetworkRequest request(url);
    QNetworkReply *reply = network_manager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(chiakiGui) << "Failed to validate JWT:" << reply->errorString();
            callback(false);
            return;
        }

        QByteArray data = reply->readAll();
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

        if (parseError.error != QJsonParseError::NoError) {
            qCWarning(chiakiGui) << "Error parsing JWT validation response:" << parseError.errorString();
            callback(false);
            return;
        }

        QJsonObject obj = doc.object();
        QString error = obj.value("error").toString();
        QString np = obj.value("NP").toString();

        bool isValid = error.isEmpty() && !np.isEmpty();

        if (isValid) {
            // Save user data
            QString psnId = obj.value("PSN_ID").toString();
            QString dateExp = obj.value("Date_exp").toString();
            QString email = obj.value("Email").toString();
            QString npValue = obj.value("NP").toString();

            if (!psnId.isEmpty()) {
                savePsnId(psnId);
            }
            if (!dateExp.isEmpty()) {
                saveDateExp(dateExp);
            }
            if (!email.isEmpty()) {
                saveEmail(email);
            }
            if (!npValue.isEmpty()) {
                saveNp(npValue);
            }
        } else {
            qCWarning(chiakiGui) << "JWT is invalid or expired. Error:" << error;
            if (!error.isEmpty()) {
                clearJwt();
            }
        }

        callback(isValid);
    });
}

QString JwtManager::getPsnId()
{
    QSettings *settings = getSettings();
    QString psnId = settings->value(KEY_PSN_ID).toString();
    delete settings;
    return psnId;
}

QString JwtManager::getEmail()
{
    QSettings *settings = getSettings();
    QString email = settings->value(KEY_EMAIL).toString();
    delete settings;
    return email;
}

QString JwtManager::getNp()
{
    QSettings *settings = getSettings();
    QString np = settings->value(KEY_NP).toString();
    delete settings;
    return np;
}

QString JwtManager::getDateExp()
{
    QSettings *settings = getSettings();
    QString dateExp = settings->value(KEY_DATE_EXP).toString();
    delete settings;
    return dateExp;
}

QString JwtManager::getTimeUntilExpiration()
{
    QString dateExpStr = getDateExp();
    if (dateExpStr.isEmpty()) {
        return QString();
    }

    JwtManager manager;
    return manager.calculateTimeUntilExpiration(dateExpStr);
}

QString JwtManager::getCachedAvatarUrl()
{
    QSettings *settings = getSettings();
    QString url = settings->value(KEY_AVATAR_URL).toString();
    delete settings;
    return url.isEmpty() ? DEFAULT_AVATAR_URL : url;
}

void JwtManager::loadAvatar(std::function<void(const QString &)> callback)
{
    QString psnId = getPsnId();
    if (psnId.isEmpty()) {
        callback(DEFAULT_AVATAR_URL);
        return;
    }

    QUrl url(PSN_AVATAR_URL);
    QUrlQuery query;
    query.addQueryItem("psnid", psnId);
    url.setQuery(query);

    QNetworkRequest request(url);
    QNetworkReply *reply = network_manager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(chiakiGui) << "Failed to load avatar:" << reply->errorString();
            callback(DEFAULT_AVATAR_URL);
            return;
        }

        QByteArray data = reply->readAll();
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

        if (parseError.error != QJsonParseError::NoError) {
            qCWarning(chiakiGui) << "Error parsing avatar response:" << parseError.errorString();
            callback(DEFAULT_AVATAR_URL);
            return;
        }

        QJsonObject obj = doc.object();
        QString avatarUrl = obj.value("avatarUrl").toString();

        if (avatarUrl.isEmpty()) {
            avatarUrl = DEFAULT_AVATAR_URL;
        }

        saveAvatarUrl(avatarUrl);
        callback(avatarUrl);
    });
}

void JwtManager::getConsoleStatus(const QString &np, std::function<void(const QString &)> callback)
{
    QUrl url("https://api.4cloud.pro/status_console.php");
    QUrlQuery query;
    query.addQueryItem("NPS4", np);
    url.setQuery(query);

    QNetworkRequest request(url);
    QNetworkReply *reply = network_manager->get(request);

    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(chiakiGui) << "Failed to get console status:" << reply->errorString();
            callback(QString());
            return;
        }

        QString status = QString::fromUtf8(reply->readAll()).trimmed();
        callback(status.isEmpty() ? QString() : status);
    });
}

void JwtManager::sendWakeupViaApi(const QString &np)
{
    QUrl url("https://4cloud.pro/api/wakeup.php");
    QUrlQuery query;
    query.addQueryItem("nps4", np);
    url.setQuery(query);

    QNetworkRequest request(url);
    QNetworkReply *reply = network_manager->get(request);

    connect(reply, &QNetworkReply::finished, this, [reply]() {
        reply->deleteLater();
    });
}

void JwtManager::updateDateExpIfNeeded(std::function<void()> callback)
{
    if (is_updating_date_exp) {
        if (callback) callback();
        return;
    }

    QString jwt = getJwt();
    if (jwt.isEmpty()) {
        if (callback) callback();
        return;
    }

    is_updating_date_exp = true;

    QUrl url(DECODE_JWT_URL);
    QUrlQuery query;
    query.addQueryItem("method", "decode-jwt");
    query.addQueryItem("jwt", jwt);
    url.setQuery(query);

    QNetworkRequest request(url);
    QNetworkReply *reply = network_manager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        is_updating_date_exp = false;
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(chiakiGui) << "Failed to update Date_exp:" << reply->errorString();
            if (callback) callback();
            return;
        }

        QByteArray data = reply->readAll();
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

        if (parseError.error != QJsonParseError::NoError) {
            qCWarning(chiakiGui) << "Error updating Date_exp:" << parseError.errorString();
            if (callback) callback();
            return;
        }

        QJsonObject obj = doc.object();
        QString error = obj.value("error").toString();
        QString dateExp = obj.value("Date_exp").toString();

        if (error.isEmpty() && !dateExp.isEmpty()) {
            QString currentDateExp = getDateExp();
            if (currentDateExp != dateExp) {
                saveDateExp(dateExp);
            }

            // Also update other fields
            QString np = obj.value("NP").toString();
            QString email = obj.value("Email").toString();
            QString psnId = obj.value("PSN_ID").toString();

            if (!np.isEmpty()) {
                QString currentNp = getNp();
                if (currentNp != np) {
                    saveNp(np);
                }
            }
            if (!email.isEmpty()) {
                QString currentEmail = getEmail();
                if (currentEmail != email) {
                    saveEmail(email);
                }
            }
            if (!psnId.isEmpty()) {
                QString currentPsnId = getPsnId();
                if (currentPsnId != psnId) {
                    savePsnId(psnId);
                }
            }
        }

        if (callback) callback();
    });
}

void JwtManager::savePsnId(const QString &psnId)
{
    QSettings *settings = getSettings();
    settings->setValue(KEY_PSN_ID, psnId);
    settings->sync();
    delete settings;
}

void JwtManager::saveEmail(const QString &email)
{
    QSettings *settings = getSettings();
    settings->setValue(KEY_EMAIL, email);
    settings->sync();
    delete settings;
}

void JwtManager::saveNp(const QString &np)
{
    QSettings *settings = getSettings();
    settings->setValue(KEY_NP, np);
    settings->sync();
    delete settings;
}

void JwtManager::saveDateExp(const QString &dateExp)
{
    QSettings *settings = getSettings();
    settings->setValue(KEY_DATE_EXP, dateExp);
    settings->sync();
    delete settings;
}

void JwtManager::saveAvatarUrl(const QString &url)
{
    QSettings *settings = getSettings();
    settings->setValue(KEY_AVATAR_URL, url);
    settings->sync();
    delete settings;
}

QString JwtManager::calculateTimeUntilExpiration(const QString &dateExpStr)
{
    // Parse date in format "ДД.ММ.ГГГГ ЧЧ:ММ"
    QStringList parts = dateExpStr.split(" ");
    if (parts.size() != 2) return QString();

    QStringList dateParts = parts[0].split(".");
    QStringList timeParts = parts[1].split(":");
    if (dateParts.size() != 3 || timeParts.size() != 2) return QString();

    bool ok;
    int day = dateParts[0].toInt(&ok);
    if (!ok) return QString();
    int month = dateParts[1].toInt(&ok);
    if (!ok) return QString();
    int year = dateParts[2].toInt(&ok);
    if (!ok) return QString();
    int hour = timeParts[0].toInt(&ok);
    if (!ok) return QString();
    int minute = timeParts[1].toInt(&ok);
    if (!ok) return QString();

    QDateTime expirationTime(QDate(year, month, day), QTime(hour, minute, 0));
    QDateTime currentTime = QDateTime::currentDateTime();

    qint64 diff = currentTime.msecsTo(expirationTime);
    if (diff <= 0) {
        return "Истекла";
    }

    qint64 days = diff / (1000 * 60 * 60 * 24);
    qint64 hours = (diff % (1000 * 60 * 60 * 24)) / (1000 * 60 * 60);
    qint64 minutes = (diff % (1000 * 60 * 60)) / (1000 * 60);

    return QString("%1 дн. %2 ч. %3 мин.").arg(days).arg(hours).arg(minutes);
}
