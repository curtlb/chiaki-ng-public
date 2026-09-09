// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloudcatalogbackend.h"
#ifdef CHIAKI_GUI_ENABLE_STEAM_SHORTCUT
#include "steamtools.h"
#endif
#include "cloudbillingclient.h"
#include <cloudlog.h>
#include <chiaki/cloudcatalog.h>
#include <chiaki/log.h>
#include <thread>
#include <cstring>
#include <QLoggingCategory>
#include <QUrlQuery>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QEventLoop>
#include <QTimer>
#include <QCoreApplication>
#include <QPointer>
#include <QProcessEnvironment>
#include <QImageReader>
#include <QPainter>
#include <QPixmap>
#include <climits>
#include <QJSEngine>
#include <algorithm>

Q_DECLARE_LOGGING_CATEGORY(chiakiGui)

CloudCatalogBackend::CloudCatalogBackend(Settings *settings, QObject *parent)
    : QObject(parent)
    , settings(settings)
    , networkManager(new QNetworkAccessManager(this))
{
    // Disable cookie jar - we use manual Cookie headers only
    networkManager->setCookieJar(nullptr);
    
    // Initialize cache directory
    cacheDirectory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/cloud_catalog";
    ensureCacheDirectory();
}

CloudCatalogBackend::~CloudCatalogBackend()
{
}

void CloudCatalogBackend::setSettings(Settings *new_settings)
{
    settings = new_settings;
}

void CloudCatalogBackend::ensureCacheDirectory()
{
    QDir dir;
    if (!dir.exists(cacheDirectory)) {
        dir.mkpath(cacheDirectory);
        if (settings && settings->GetLogVerbose()) {
            qInfo() << "Created cache directory:" << cacheDirectory;
        }
    }
}

QString CloudCatalogBackend::getCacheFilePath(const QString &key)
{
    // Sanitize key for filename (replace invalid chars)
    QString safeKey = key;
    safeKey.replace("/", "_");
    safeKey.replace("\\", "_");
    safeKey.replace(":", "_");
    return cacheDirectory + "/" + safeKey + ".json";
}

bool CloudCatalogBackend::getOwnedPsnowEntitlement(const QString &gameIdentifier,
                                                   QString &outEntitlementId, QString &outPlatform)
{
    if (gameIdentifier.isEmpty())
        return false;

    // The lib owns the unified catalog filename and bumps its version suffix, so resolve it by glob
    // (newest unified_catalog_v*.json) rather than hard-coding the current version.
    QDir dir(cacheDirectory);
    QFileInfoList matches = dir.entryInfoList({QStringLiteral("unified_catalog_v*.json")},
                                              QDir::Files, QDir::Time);
    if (matches.isEmpty())
        return false;

    QFile file(matches.first().absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject())
        return false;

    const QJsonArray games = doc.object().value(QStringLiteral("games")).toArray();
    for (const QJsonValue &v : games) {
        if (!v.isObject())
            continue;
        const QJsonObject g = v.toObject();
        // Match the launch identifier against the row's launch id (and productId as a fallback).
        const QString streamId = g.value(QStringLiteral("streamIdentifier")).toString();
        const QString productId = g.value(QStringLiteral("productId")).toString();
        if (gameIdentifier != streamId && gameIdentifier != productId)
            continue;

        // Only owned PSNOW rows carry a pre-resolved streaming entitlement we can stream directly.
        const QString svcRaw = g.value(QStringLiteral("streamServiceType")).toString();
        const QString svc = svcRaw.isEmpty() ? g.value(QStringLiteral("serviceType")).toString() : svcRaw;
        const QString entitlementId = g.value(QStringLiteral("entitlementId")).toString();
        if (svc != QStringLiteral("psnow") || !g.value(QStringLiteral("isOwned")).toBool()
            || entitlementId.isEmpty())
            return false;

        outEntitlementId = entitlementId;
        outPlatform = g.value(QStringLiteral("platform")).toString();
        return true;
    }
    return false;
}

QString CloudCatalogBackend::getCachedData(const QString &key, int maxAge)
{
    QString filePath = getCacheFilePath(key);
    QFileInfo fileInfo(filePath);
    
    if (!fileInfo.exists()) {
        qInfo() << "[CACHE MISS] No cache file found for:" << key;
        return QString();
    }
    
    // Check file age
    qint64 age = fileInfo.lastModified().msecsTo(QDateTime::currentDateTime());
    if (age > maxAge) {
        // Cache expired, delete file
        QFile::remove(filePath);
        qInfo() << "[CACHE EXPIRED] Cache file expired for:" << key << "(age:" << (age / 1000) << "seconds, max:" << (maxAge / 1000) << "seconds)";
        return QString();
    }
    
    // Read file
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[CACHE ERROR] Failed to open cache file:" << filePath;
        return QString();
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    qint64 ageSeconds = age / 1000;
    qInfo() << "[CACHE HIT] Loaded cached data for:" << key << "(" << (data.size() / 1024) << "KB, age:" << ageSeconds << "seconds)";
    
    return QString::fromUtf8(data);
}

QString CloudCatalogBackend::getCachedPs5CatalogV3(int maxAge)
{
    const QString cached = getCachedData(QStringLiteral("ps5_cloud_catalog_v6"), maxAge);
    if (cached.isEmpty())
        return QString();

    const QJsonDocument doc = QJsonDocument::fromJson(cached.toUtf8());
    if (!doc.isObject()) {
        QFile::remove(getCacheFilePath(QStringLiteral("ps5_cloud_catalog_v6")));
        return QString();
    }

    const QString expectedLocale = settings ? settings->GetCloudStoreLocale() : QStringLiteral("en-US");
    const QString cachedLocale = doc.object().value(QStringLiteral("locale")).toString();
    if (!cachedLocale.isEmpty() && cachedLocale != expectedLocale) {
        qInfo() << "[CACHE LOCALE MISMATCH] PS5 catalog v3 locale" << cachedLocale
                << "!=" << expectedLocale << ", refetching";
        QFile::remove(getCacheFilePath(QStringLiteral("ps5_cloud_catalog_v6")));
        return QString();
    }

    return cached;
}

void CloudCatalogBackend::setCachedData(const QString &key, const QJsonDocument &data)
{
    QString filePath = getCacheFilePath(key);
    
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[CACHE ERROR] Failed to write cache file:" << filePath;
        return;
    }
    
    QByteArray jsonData = data.toJson(QJsonDocument::Compact);
    file.write(jsonData);
    file.close();
    
    qInfo() << "[CACHE SAVED] Cached data for:" << key << "(" << (jsonData.size() / 1024) << "KB)";
}

QString CloudCatalogBackend::getNpSsoToken()
{
    return settings->GetNpssoToken();
}

static bool billingServerConfigured(const Settings *settings)
{
    if (!settings || !settings->GetCloudBillingEnabled())
        return false;
    return !settings->GetCloudBillingHost().trimmed().isEmpty();
}

static bool useBillingCatalogSource(const Settings *settings)
{
    // Catalog from MySQL does not need the player's 4cloud email — only server host.
    return billingServerConfigured(settings);
}

static QJsonObject billingCatalogEnvelope(const QJsonArray &games, const QString &locale)
{
    QJsonObject root;
    root[QStringLiteral("schemaVersion")] = 11;
    root[QStringLiteral("total")] = games.size();
    root[QStringLiteral("nativeMode")] = false;
    root[QStringLiteral("fallbackRegion")] = QString();
    root[QStringLiteral("resolvedStoreLang")] = QString();
    root[QStringLiteral("settledLocale")] = locale.isEmpty() ? QStringLiteral("en-US") : locale;
    root[QStringLiteral("warning")] = QString();
    root[QStringLiteral("catalogSource")] = QStringLiteral("billing_db");
    root[QStringLiteral("games")] = games;
    return root;
}

static QString normalizeTitleSku(QString sku)
{
    sku = sku.trimmed();
    if (sku.isEmpty())
        return {};
    // titlecontainer needs CUSA#####_00 — bare CUSA##### returns 404.
    const QString up = sku.toUpper();
    const QStringList prefixes = {
        QStringLiteral("CUSA"), QStringLiteral("PPSA"), QStringLiteral("NPEA"),
        QStringLiteral("NPEB"), QStringLiteral("NPUB"), QStringLiteral("NPUA"),
        QStringLiteral("NPUG")
    };
    for (const QString &pref : prefixes) {
        if (!up.startsWith(pref))
            continue;
        const QString rest = up.mid(pref.size());
        if (rest.contains(QLatin1Char('_')))
            return up;
        // Digits only → append _00
        bool ok = false;
        rest.toULongLong(&ok);
        if (ok && !rest.isEmpty())
            return pref + rest + QStringLiteral("_00");
        return up;
    }
    return sku;
}

static QString extractTitleSku(const QString &product_id)
{
    const QString pid = product_id.trimmed();
    if (pid.isEmpty())
        return {};
    // EP0001-CUSA12345_00-FOO → CUSA12345_00
    const int dash = pid.indexOf(QLatin1Char('-'));
    if (dash >= 0 && dash + 1 < pid.size()) {
        const QString rest = pid.mid(dash + 1);
        const int nextDash = rest.indexOf(QLatin1Char('-'));
        return normalizeTitleSku(nextDash > 0 ? rest.left(nextDash) : rest);
    }
    return normalizeTitleSku(pid);
}

static QString chihiroCoverUrl(const QString &product_id, const QString &locale = QStringLiteral("en-GB"))
{
    const QString pid = product_id.trimmed();
    if (pid.isEmpty())
        return {};
    // /container/{full-NP-id}/image 404s; /titlecontainer/{CUSA#####_00}/image returns JPEG.
    Q_UNUSED(locale);
    QString country = QStringLiteral("GB");
    const QString prefix = pid.left(2).toUpper();
    if (prefix == QStringLiteral("UP") || prefix == QStringLiteral("HP") || prefix == QStringLiteral("HN"))
        country = QStringLiteral("US");
    const QString sku = extractTitleSku(pid);
    if (sku.isEmpty())
        return {};
    return QStringLiteral("https://store.playstation.com/store/api/chihiro/00_09_000/titlecontainer/%1/en/999/%2/image?w=440&h=440")
        .arg(country, sku);
}

static QString pickImageUrl(const QJsonObject &g, const QString &locale = QStringLiteral("en-GB"))
{
    const QJsonObject extracted = g.value(QStringLiteral("extracted_images")).toObject();
    QString url = extracted.value(QStringLiteral("cover")).toString();
    if (url.isEmpty())
        url = extracted.value(QStringLiteral("landscape")).toString();
    if (!url.isEmpty())
        return url;
    url = g.value(QStringLiteral("imageUrl")).toString();
    // Legacy chihiro /container/{full-id}/image URLs 404 — rewrite to titlecontainer.
    if (!url.isEmpty() && url.contains(QLatin1String("/chihiro/"))
        && url.contains(QLatin1String("/container/"))
        && !url.contains(QLatin1String("/titlecontainer/")))
        url.clear();
    if (!url.isEmpty() && !url.contains(QLatin1String("/chihiro/")))
        return url; // CDN / apollo / other real cover URL from DB
    const QJsonArray images = g.value(QStringLiteral("images")).toArray();
    for (const QJsonValue &v : images) {
        const QJsonObject img = v.toObject();
        if (img.value(QStringLiteral("type")).toInt() == 10) {
            const QString u = img.value(QStringLiteral("url")).toString();
            if (!u.isEmpty())
                return u;
        }
    }
    for (const QJsonValue &v : images) {
        const QString u = v.toObject().value(QStringLiteral("url")).toString();
        if (!u.isEmpty())
            return u;
    }
    QString pid = g.value(QStringLiteral("productId")).toString();
    if (pid.isEmpty())
        pid = g.value(QStringLiteral("streamIdentifier")).toString();
    if (!pid.isEmpty())
        return chihiroCoverUrl(pid, locale);
    return url; // may still be a rewritten-empty path → empty
}

QString CloudCatalogBackend::rowLookupKey(const CatalogDisplayRow &row)
{
    if (!row.streamIdentifier.isEmpty())
        return row.streamIdentifier;
    if (!row.productId.isEmpty())
        return row.productId;
    return row.id;
}

CloudCatalogBackend::CatalogDisplayRow CloudCatalogBackend::catalogRowFromJson(const QJsonObject &g, const QString &storeLocale)
{
    CatalogDisplayRow row;
    row.name = g.value(QStringLiteral("name")).toString();
    if (row.name.isEmpty()) {
        const QJsonObject meta = g.value(QStringLiteral("game_meta")).toObject();
        row.name = meta.value(QStringLiteral("name")).toString();
    }
    row.productId = g.value(QStringLiteral("productId")).toString();
    if (row.productId.isEmpty())
        row.productId = g.value(QStringLiteral("product_id")).toString();
    row.id = g.value(QStringLiteral("id")).toString();
    row.category = g.value(QStringLiteral("category")).toString();
    row.serviceType = g.value(QStringLiteral("serviceType")).toString();
    row.platform = g.value(QStringLiteral("platform")).toString();
    row.streamIdentifier = g.value(QStringLiteral("streamIdentifier")).toString();
    row.streamServiceType = g.value(QStringLiteral("streamServiceType")).toString();
    row.conceptUrl = g.value(QStringLiteral("conceptUrl")).toString();
    if (row.conceptUrl.isEmpty())
        row.conceptUrl = g.value(QStringLiteral("concept_url")).toString();
    row.isOwned = g.value(QStringLiteral("isOwned")).toBool(false);
    row.plusCatalog = g.value(QStringLiteral("plusCatalog")).toBool(false);
    row.sourceList = g.value(QStringLiteral("sourceList")).toString();
    if (row.sourceList.isEmpty())
        row.sourceList = g.value(QStringLiteral("source_list")).toString();
    row.imageUrl = pickImageUrl(g, storeLocale);
    return row;
}

QVariantMap CloudCatalogBackend::catalogRowToVariant(const CatalogDisplayRow &row)
{
    QVariantMap m;
    m[QStringLiteral("name")] = row.name;
    m[QStringLiteral("productId")] = row.productId;
    m[QStringLiteral("product_id")] = row.productId;
    m[QStringLiteral("id")] = row.id;
    m[QStringLiteral("category")] = row.category;
    m[QStringLiteral("serviceType")] = row.serviceType;
    m[QStringLiteral("platform")] = row.platform;
    m[QStringLiteral("streamIdentifier")] = row.streamIdentifier;
    m[QStringLiteral("streamServiceType")] = row.streamServiceType;
    m[QStringLiteral("conceptUrl")] = row.conceptUrl;
    m[QStringLiteral("concept_url")] = row.conceptUrl;
    m[QStringLiteral("isOwned")] = row.isOwned;
    m[QStringLiteral("plusCatalog")] = row.plusCatalog;
    if (!row.imageUrl.isEmpty())
        m[QStringLiteral("imageUrl")] = row.imageUrl;
    if (!row.sourceList.isEmpty())
        m[QStringLiteral("sourceList")] = row.sourceList;
    return m;
}

QVector<CloudCatalogBackend::CatalogDisplayRow> CloudCatalogBackend::buildCatalogDisplayRows(const QJsonArray &games)
{
    const QString locale = settings ? settings->GetCloudStoreLocale() : QStringLiteral("en-GB");
    QVector<CatalogDisplayRow> rows;
    rows.reserve(games.size());
    for (const QJsonValue &v : games) {
        if (!v.isObject())
            continue;
        rows.append(catalogRowFromJson(v.toObject(), locale));
    }
    return rows;
}

int CloudCatalogBackend::catalogGameCount() const
{
    return catalogTotalGames_;
}

static QString catalogTitleKey(QString name)
{
    name = name.toLower();
    const QStringList junk = {
        QStringLiteral("(playstation plus)"),
        QStringLiteral("playstation plus"),
        QStringLiteral("director's cut"),
        QStringLiteral("directors cut"),
        QStringLiteral("digital deluxe edition"),
        QStringLiteral("digital deluxe"),
        QStringLiteral("deluxe edition"),
        QStringLiteral("standard edition"),
        QStringLiteral("game of the year edition"),
        QStringLiteral("game of the year"),
        QStringLiteral("goty"),
        QStringLiteral("remastered"),
    };
    for (const QString &j : junk)
        name.remove(j);
    QString out;
    out.reserve(name.size());
    for (const QChar &c : name) {
        if (c.isLetterOrNumber())
            out.append(c);
    }
    return out;
}

static bool isLegacyClassicStreamId(const QString &pid)
{
    const int dash = pid.indexOf(QLatin1Char('-'));
    if (dash < 0)
        return true;
    const QString title = pid.mid(dash + 1);
    return !title.startsWith(QLatin1String("CUSA"), Qt::CaseInsensitive)
        && !title.startsWith(QLatin1String("PPSA"), Qt::CaseInsensitive);
}

bool CloudCatalogBackend::isBillingRentalPlayableRow(const CatalogDisplayRow &row)
{
    if (row.category == QLatin1String("owned"))
        return true;
    if (row.category == QLatin1String("purchaseable"))
        return false;
    if (row.category != QLatin1String("streamable"))
        return false;
    if (row.serviceType == QLatin1String("pscloud")) {
        // Native PS Now catalog marks Plus/F2P with plusCatalog; billing DB uses sourceList.
        if (row.plusCatalog)
            return true;
        const QString src = row.sourceList;
        return src == QLatin1String("free-to-play-list")
            || src == QLatin1String("plus-games-list")
            || src == QLatin1String("plus-monthly-games-list")
            || src == QLatin1String("plus-classics-list")
            || src == QLatin1String("ubisoft-classics-list");
    }
    if (row.serviceType != QLatin1String("psnow"))
        return false;
    const QString pid = row.productId.isEmpty() ? row.streamIdentifier : row.productId;
    if (isLegacyClassicStreamId(pid))
        return false;
    const QString pref = pid.left(2).toUpper();
    if (pref == QLatin1String("UP") || pref == QLatin1String("HP") || pref == QLatin1String("HN"))
        return false;
    if (pid.startsWith(QLatin1String("NPUB"), Qt::CaseInsensitive)
        || pid.startsWith(QLatin1String("NPUG"), Qt::CaseInsensitive)
        || pid.startsWith(QLatin1String("NPUA"), Qt::CaseInsensitive))
        return false;
    return true;
}

void CloudCatalogBackend::purgeStaleBillingCatalogCaches()
{
    const QStringList stale = {
        QStringLiteral("billing_catalog_v6"),
        QStringLiteral("billing_catalog_v7"),
        QStringLiteral("billing_catalog_v8"),
        QStringLiteral("billing_catalog_v9"),
        QStringLiteral("billing_catalog_v10"),
        QStringLiteral("billing_catalog_v11"),
        QStringLiteral("billing_catalog_v12"),
        QStringLiteral("billing_catalog_v13"),
    };
    for (const QString &key : stale)
        QFile::remove(getCacheFilePath(key));
}

QString CloudCatalogBackend::billingCatalogCacheKey()
{
    return QStringLiteral("billing_catalog_v14");
}

QVariantMap CloudCatalogBackend::filterDisplayCatalog(const QString &query, const QVariantList &categoryFilters,
                                                      const QVariantList &favoriteIds, int sortState,
                                                      bool billingRental, int limit) const
{
    QStringList categories;
    categories.reserve(categoryFilters.size());
    for (const QVariant &v : categoryFilters)
        categories.append(v.toString());

    QSet<QString> favorites;
    for (const QVariant &v : favoriteIds)
        favorites.insert(v.toString());

    const QString q = query.trimmed().toLower();
    const bool filterCategories = !categories.isEmpty();
    const bool filterFavorites = !favorites.isEmpty();
    const bool filterSearch = !q.isEmpty();
    // Billing rental: search hits come from PS Now (native), not MySQL.
    const bool usePsNowSearch = billingRental && filterSearch && !psnowSearchRows_.isEmpty();
    const QVector<CatalogDisplayRow> &sourceRows = usePsNowSearch ? psnowSearchRows_ : catalogDisplayRows_;

    QVector<const CatalogDisplayRow *> matches;
    matches.reserve(sourceRows.size());
    for (const CatalogDisplayRow &row : sourceRows) {
        if (billingRental && !isBillingRentalPlayableRow(row))
            continue;
        // Default grid: account-owned titles only. Full rental catalog is search-only (PS Now).
        if (billingRental && !filterSearch && row.category != QLatin1String("owned"))
            continue;
        if (filterCategories) {
            bool category_ok = false;
            for (const QString &cat : categories) {
                if (row.category == cat) {
                    category_ok = true;
                    break;
                }
            }
            if (!category_ok)
                continue;
        }
        if (filterFavorites) {
            const QString pid = row.productId.isEmpty() ? row.id : row.productId;
            if (!favorites.contains(pid))
                continue;
        }
        if (filterSearch) {
            if (!row.name.toLower().contains(q) && !row.productId.toLower().contains(q))
                continue;
        }
        matches.append(&row);
    }

    const auto playable = [billingRental](const CatalogDisplayRow &row) {
        if (billingRental)
            return isBillingRentalPlayableRow(row);
        return row.category != QLatin1String("purchaseable");
    };

    const auto hasCover = [](const CatalogDisplayRow &row) {
        return !row.imageUrl.isEmpty();
    };

    if (sortState == 1) {
        std::sort(matches.begin(), matches.end(), [](const CatalogDisplayRow *a, const CatalogDisplayRow *b) {
            return QString::localeAwareCompare(a->name, b->name) < 0;
        });
    } else if (sortState == 2) {
        std::sort(matches.begin(), matches.end(), [](const CatalogDisplayRow *a, const CatalogDisplayRow *b) {
            return QString::localeAwareCompare(b->name, a->name) < 0;
        });
    } else {
        std::stable_sort(matches.begin(), matches.end(), [&](const CatalogDisplayRow *a, const CatalogDisplayRow *b) {
            const bool ca = hasCover(*a);
            const bool cb = hasCover(*b);
            if (ca != cb)
                return ca > cb;
            const bool pa = playable(*a);
            const bool pb = playable(*b);
            if (pa != pb)
                return pa > pb;
            if (billingRental) {
                const bool psnow_a = a->serviceType == QLatin1String("psnow");
                const bool psnow_b = b->serviceType == QLatin1String("psnow");
                if (psnow_a != psnow_b)
                    return psnow_a > psnow_b;
            }
            return QString::localeAwareCompare(a->name, b->name) < 0;
        });
    }

    const int cap = limit > 0 ? limit : static_cast<int>(matches.size());
    const int take = std::min(static_cast<int>(matches.size()), cap);

    QVariantList out;
    out.reserve(take);
    for (int i = 0; i < take; ++i)
        out.append(catalogRowToVariant(*matches[i]));

    QVariantMap result;
    result[QStringLiteral("games")] = out;
    result[QStringLiteral("totalFiltered")] = matches.size();
    result[QStringLiteral("truncated")] = matches.size() > cap;
    result[QStringLiteral("totalGames")] = usePsNowSearch ? psnowSearchTotalGames_ : catalogTotalGames_;
    result[QStringLiteral("searchSource")] = usePsNowSearch
        ? QStringLiteral("psnow")
        : (billingRental ? QStringLiteral("billing_db") : QStringLiteral("unified"));
    return result;
}

bool CloudCatalogBackend::psNowSearchCatalogReady() const
{
    return !psnowSearchRows_.isEmpty();
}

static QString readNewestUnifiedCatalogFile(const QString &dirPath)
{
    QDir dir(dirPath);
    const QFileInfoList matches = dir.entryInfoList(
        {QStringLiteral("unified_catalog_v*.json")}, QDir::Files, QDir::Time);
    if (matches.isEmpty())
        return {};
    QFile f(matches.first().absoluteFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

void CloudCatalogBackend::ensurePsNowSearchCatalog(const QJSValue &callback)
{
    if (!useBillingCatalogSource(settings)) {
        if (callback.isCallable())
            callback.call({ true, QStringLiteral("not_billing"), 0 });
        return;
    }
    // Session memory cache: one PS Now fetch per Chiaki run (until invalidateCache).
    if (!psnowSearchRows_.isEmpty()) {
        if (callback.isCallable())
            callback.call({ true, QStringLiteral("ready"), psnowSearchTotalGames_ });
        return;
    }

    // Warm from on-disk unified cache written earlier this session (no network).
    const QString searchCacheDir = cacheDirectory + QStringLiteral("/psnow_search");
    QDir().mkpath(searchCacheDir);
    {
        const QString disk = readNewestUnifiedCatalogFile(searchCacheDir);
        if (!disk.isEmpty()) {
            const QJsonObject root = QJsonDocument::fromJson(disk.toUtf8()).object();
            const QJsonArray games = root.value(QStringLiteral("games")).toArray();
            if (!games.isEmpty()) {
                psnowSearchRows_ = buildCatalogDisplayRows(games);
                psnowSearchTotalGames_ = psnowSearchRows_.size();
                CloudLogMessage(QStringLiteral("Catalog"),
                    QStringLiteral("PS Now search catalog from disk cache: %1 games")
                        .arg(psnowSearchTotalGames_));
                if (callback.isCallable())
                    callback.call({ true, QStringLiteral("disk_cache"), psnowSearchTotalGames_ });
                return;
            }
        }
    }

    bool expected = false;
    if (!psnowSearchFetchInFlight.compare_exchange_strong(expected, true)) {
        if (callback.isCallable())
            pendingPsNowSearchCallbacks.push_back(callback);
        return;
    }

    // Park on GUI thread only — QJSValue must not cross worker threads.
    if (callback.isCallable())
        pendingPsNowSearchCallbacks.push_back(callback);

    const quint64 gen = catalogGeneration;
    const QString host = settings->GetCloudBillingHost();
    const quint16 port = settings->GetCloudBillingPort();
    const QString email = settings->GetFourCloudEmail();
    const QString cachedNpsso = sessionCatalogNpsso_.trimmed();
    const QByteArray locale =
        (settings ? settings->GetCloudStoreLocale() : QStringLiteral("en-US")).toUtf8();
    const QByteArray cacheDir = searchCacheDir.toUtf8();
    QPointer<CloudCatalogBackend> self(this);

    if (email.trimmed().isEmpty() && cachedNpsso.isEmpty()) {
        psnowSearchFetchInFlight.store(false);
        std::vector<QJSValue> parked;
        parked.swap(pendingPsNowSearchCallbacks);
        for (QJSValue &cb : parked) {
            if (cb.isCallable())
                cb.call({ false, QStringLiteral("Нет email 4cloud"), 0 });
        }
        return;
    }

    std::thread([self, gen, host, port, email, cachedNpsso, locale, cacheDir]() mutable {
        bool success = false;
        QString message;
        QString jsonPayload;
        QString npssoForSession;
        QString npsso = cachedNpsso;

        if (npsso.isEmpty()) {
            CloudLogMessage(QStringLiteral("Catalog"),
                QStringLiteral("PS Now search catalog: requesting assigned-account NPSSO"));
            const auto npssoRes = CloudBillingClient::catalogNpsso(host, port, email);
            if (!npssoRes.ok) {
                message = npssoRes.ui_message.isEmpty() ? npssoRes.error : npssoRes.ui_message;
                CloudLogMessage(QStringLiteral("Catalog"),
                    QStringLiteral("catalog_npsso failed: %1").arg(message));
            } else {
                npsso = npssoRes.data.value(QStringLiteral("npsso")).toString().trimmed();
                if (npsso.isEmpty())
                    message = QStringLiteral("Пустой NPSSO у назначенного аккаунта");
                else
                    npssoForSession = npsso;
            }
        } else {
            CloudLogMessage(QStringLiteral("Catalog"),
                QStringLiteral("PS Now search catalog: reusing in-memory NPSSO (skip catalog_npsso)"));
        }

        if (!npsso.isEmpty()) {
            CloudChiakiLog file_log(CHIAKI_LOG_INFO | CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR, "Catalog");
            ChiakiLog *log = file_log.GetChiakiLog();
            CHIAKI_LOGI(log, "PS Now search unified fetch started (locale=%s)", locale.constData());

            ChiakiCloudCatalogConfig cfg;
            memset(&cfg, 0, sizeof(cfg));
            const QByteArray npssoBytes = npsso.toUtf8();
            cfg.npsso = npssoBytes.constData();
            cfg.locale = locale.constData();
            cfg.cache_dir = cacheDir.constData();
            cfg.force_refresh = false;

            ChiakiCloudCatalogResult res;
            ChiakiErrorCode err = chiaki_cloudcatalog_fetch_unified(&cfg, &res, log);
            success = (err == CHIAKI_ERR_SUCCESS && res.json);
            if (success) {
                jsonPayload = QString::fromUtf8(res.json);
                message = QStringLiteral("Success");
                CloudLogMessage(QStringLiteral("Catalog"),
                    QStringLiteral("PS Now search catalog fetch ok"));
            } else {
                message = QString::fromUtf8(
                    res.error_message ? res.error_message : "Failed to fetch PS Now catalog");
                CloudLogMessage(QStringLiteral("Catalog"),
                    QStringLiteral("PS Now search fetch failed: %1").arg(message));
            }
            chiaki_cloudcatalog_result_fini(&res);
        }

        QCoreApplication *app = QCoreApplication::instance();
        if (!app)
            return;
        QMetaObject::invokeMethod(app, [self, gen, success, message, jsonPayload, npssoForSession]() mutable {
            if (!self)
                return;
            std::vector<QJSValue> parked;
            parked.swap(self->pendingPsNowSearchCallbacks);
            self->psnowSearchFetchInFlight.store(false);

            if (self->catalogGeneration != gen) {
                self->psnowSearchRows_.clear();
                self->psnowSearchTotalGames_ = 0;
                for (QJSValue &pcb : parked)
                    if (pcb.isCallable())
                        self->ensurePsNowSearchCatalog(pcb);
                return;
            }

            if (!npssoForSession.isEmpty())
                self->sessionCatalogNpsso_ = npssoForSession;

            if (success) {
                const QJsonObject root = QJsonDocument::fromJson(jsonPayload.toUtf8()).object();
                self->psnowSearchRows_ = self->buildCatalogDisplayRows(root.value(QStringLiteral("games")).toArray());
                self->psnowSearchTotalGames_ = self->psnowSearchRows_.size();
                CloudLogMessage(QStringLiteral("Catalog"),
                    QStringLiteral("PS Now search catalog ready: %1 games").arg(self->psnowSearchTotalGames_));
            }

            for (QJSValue &pcb : parked) {
                if (pcb.isCallable())
                    pcb.call({ success, message, self->psnowSearchTotalGames_ });
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void CloudCatalogBackend::recordRecentPlay(const QString &streamIdentifier, const QString &serviceType,
                                           const QString &gameName)
{
    if (!settings || streamIdentifier.trimmed().isEmpty())
        return;
    const QString sid = streamIdentifier.trimmed();
    QJsonArray recent = QJsonDocument::fromJson(settings->GetCloudRecentPlays().toUtf8()).array();
    QJsonArray next;
    next.append(QJsonObject{
        {QStringLiteral("streamIdentifier"), sid},
        {QStringLiteral("serviceType"), serviceType.trimmed().toLower()},
        {QStringLiteral("name"), gameName.trimmed()},
        {QStringLiteral("playedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
    });
    for (const QJsonValue &v : recent) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("streamIdentifier")).toString() == sid)
            continue;
        next.append(o);
        if (next.size() >= 24)
            break;
    }
    settings->SetCloudRecentPlays(QString::fromUtf8(QJsonDocument(next).toJson(QJsonDocument::Compact)));
}

QVariantList CloudCatalogBackend::recentDisplayGames(bool billingRental, int limit) const
{
    if (!settings || limit <= 0)
        return {};
    const QJsonArray recent = QJsonDocument::fromJson(settings->GetCloudRecentPlays().toUtf8()).array();
    if (recent.isEmpty())
        return {};

    QHash<QString, const CatalogDisplayRow *> byStream;
    byStream.reserve(catalogDisplayRows_.size());
    for (const CatalogDisplayRow &row : catalogDisplayRows_) {
        const QString key = rowLookupKey(row);
        if (!key.isEmpty())
            byStream.insert(key, &row);
    }

    QVariantList out;
    out.reserve(qMin(limit, recent.size()));
    for (const QJsonValue &v : recent) {
        if (out.size() >= limit)
            break;
        if (!v.isObject())
            continue;
        const QString sid = v.toObject().value(QStringLiteral("streamIdentifier")).toString();
        const CatalogDisplayRow *row = byStream.value(sid);
        if (!row)
            continue;
        if (billingRental && !isBillingRentalPlayableRow(*row))
            continue;
        out.append(catalogRowToVariant(*row));
    }
    return out;
}

void CloudCatalogBackend::fetchUnifiedCatalog(const QJSValue &callback)
{
    CloudLogMessage("Catalog", "fetchUnifiedCatalog requested");
    // Single source of truth: libchiaki owns the entire fetch/merge/cross-reference/
    // assemble pipeline and every cache file under cacheDirectory. This client does ZERO
    // catalog derivation -- it forwards npsso/locale/cache_dir and hands the returned
    // display-and-stream-ready JSON envelope straight to QML (see chiaki/cloudcatalog.h).
    QJSValue cb = callback;

    // Serialize: a second concurrent fetch would race the same cache files. Instead
    // of rejecting the overlap (which surfaced a spurious "fetch already in progress"
    // error when navigating back to the catalog mid-fetch), coalesce it: park this
    // caller's callback and resolve it with the SAME result when the running fetch
    // finishes. No duplicate fetch, no error toast. (GUI-thread only: this method is
    // Q_INVOKABLE from QML and the completion handler is a queued call on this object.)
    bool expected = false;
    if (!unifiedFetchInFlight.compare_exchange_strong(expected, true)) {
        if (cb.isCallable())
            pendingUnifiedCallbacks.push_back(cb);
        return;
    }

    const quint64 reqId = ++next_request_id;
    pending_callbacks.insert(reqId, cb);
    QPointer<CloudCatalogBackend> self(this);

    // Snapshot the invalidation generation with the npsso/locale inputs: if
    // invalidateCache() runs while the worker is in flight, the completion handler
    // discards this fetch's result and restarts with the then-current inputs.
    const quint64 gen = catalogGeneration;

    const bool billingCatalog = useBillingCatalogSource(settings);
    if (settings && settings->GetCloudBillingEnabled() && !billingCatalog) {
        CloudLogMessage(QStringLiteral("Catalog"),
            QStringLiteral("billing catalog skipped: set cloud_billing_host in settings"));
    }
    const QByteArray npsso = billingCatalog ? QByteArray() : getNpSsoToken().toUtf8();
    const QByteArray locale =
        (settings ? settings->GetCloudStoreLocale() : QStringLiteral("en-US")).toUtf8();
    const QByteArray cacheDir = cacheDirectory.toUtf8();
    const QString billingHost = billingCatalog && settings ? settings->GetCloudBillingHost() : QString();
    const quint16 billingPort = billingCatalog && settings ? settings->GetCloudBillingPort() : 0;
    {
        const QString startMsg = billingCatalog
            ? QStringLiteral("fetch start billing_db host=%1").arg(billingHost)
            : QStringLiteral("fetch start locale=%1 npsso=%2")
                  .arg(QString::fromUtf8(locale), npsso.isEmpty() ? QStringLiteral("missing") : QStringLiteral("present"));
        CloudLogMessage(QStringLiteral("Catalog"), startMsg);
    }

    std::thread([self, reqId, gen, billingCatalog, billingHost, billingPort, npsso, locale, cacheDir]() mutable {
        bool success = false;
        QString message;
        QString json;

        if (billingCatalog) {
            CloudLogMessage(QStringLiteral("Catalog"), QStringLiteral("billing catalog fetch started"));
            if (self)
                self->purgeStaleBillingCatalogCaches();
            const qint64 cacheTtlMs = 60 * 60 * 1000;
            const QString cacheKey = billingCatalogCacheKey();
            if (self) {
                const QString cached = self->getCachedData(cacheKey, cacheTtlMs);
                if (!cached.isEmpty()) {
                    json = cached;
                    success = true;
                    message = QStringLiteral("Cached");
                    CloudLogMessage(QStringLiteral("Catalog"),
                        QStringLiteral("[CACHE HIT] %1").arg(cacheKey));
                }
            }
            if (!success) {
                const auto res = CloudBillingClient::fetchCatalog(billingHost, billingPort);
                if (res.ok) {
                    const QJsonArray games = res.data.value(QStringLiteral("games")).toArray();
                    const QJsonObject root = billingCatalogEnvelope(
                        games, QString::fromUtf8(locale));
                    json = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
                    success = true;
                    message = QStringLiteral("Success");
                    if (self)
                        self->setCachedData(cacheKey, QJsonDocument(root));
                    CloudLogMessage(QStringLiteral("Catalog"),
                        QStringLiteral("billing catalog fetch finished: %1 games").arg(games.size()));
                } else {
                    message = res.ui_message.isEmpty() ? res.error : res.ui_message;
                    CloudLogMessage(QStringLiteral("Catalog"),
                        QStringLiteral("billing catalog fetch failed: %1").arg(message));
                }
            }
        } else {
        CloudChiakiLog file_log(CHIAKI_LOG_INFO | CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR, "Catalog");
        ChiakiLog *log = file_log.GetChiakiLog();
        CHIAKI_LOGI(log, "unified fetch started (locale=%s, npsso=%s)",
            locale.constData(), npsso.isEmpty() ? "missing" : "present");

        ChiakiCloudCatalogConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.npsso = npsso.constData();
        cfg.locale = locale.constData();
        cfg.cache_dir = cacheDir.constData();
        cfg.force_refresh = false;

        ChiakiCloudCatalogResult res;
        ChiakiErrorCode err = chiaki_cloudcatalog_fetch_unified(&cfg, &res, log);
        success = (err == CHIAKI_ERR_SUCCESS && res.json);
        json = res.json ? QString::fromUtf8(res.json) : QString();
        message = success
            ? QStringLiteral("Success")
            : QString::fromUtf8(res.error_message ? res.error_message : "Failed to fetch cloud catalog");
        chiaki_cloudcatalog_result_fini(&res);
        }

        // QJSValue must be invoked on the engine (main) thread. Route through qApp so
        // the callback is fetched and invoked on the GUI thread even if `self` is
        // destroyed before the worker finishes.
        QCoreApplication *app = QCoreApplication::instance();
        if (!app)
            return; // user quit mid-fetch: the app object is gone, nothing to deliver to
        QMetaObject::invokeMethod(app, [self, reqId, gen, success, message, json]() mutable {
            if (!self)
                return; // backend destroyed while the worker ran
            const QJSValue cb = self->pending_callbacks.take(reqId);
            std::vector<QJSValue> parked;
            parked.swap(self->pendingUnifiedCallbacks);
            self->unifiedFetchInFlight.store(false);

            // Stale fetch: invalidateCache() ran while the worker was in flight
            // (account/profile/locale switch). The result was computed with the OLD
            // npsso/locale, and worse, the lib re-wrote the cache files AFTER the
            // wipe. Never surface or persist it: wipe the cache again and restart
            // the fetch for every waiting callback — the first restart spawns a
            // fresh worker with current inputs, the rest coalesce onto it.
            if (self->catalogGeneration != gen) {
                const QByteArray staleCacheDir = self->cacheDirectory.toUtf8();
                chiaki_cloudcatalog_invalidate_cache(staleCacheDir.constData());
                QFile::remove(self->getCacheFilePath(billingCatalogCacheKey()));
                qInfo() << "[CACHE] Discarding stale unified fetch (generation"
                        << gen << "!=" << self->catalogGeneration << "); refetching";
                if (cb.isCallable())
                    self->fetchUnifiedCatalog(cb);
                for (QJSValue &pcb : parked)
                    if (pcb.isCallable())
                        self->fetchUnifiedCatalog(pcb);
                return;
            }

            CloudLogMessage(QStringLiteral("Catalog"),
                success ? QStringLiteral("fetch finished: success")
                        : QStringLiteral("fetch finished: %1").arg(message));

            // Persist the locale the lib actually settled on (region detection now lives
            // entirely in libchiaki: it re-bases the locale on the account's Kamaji-session
            // country and resolves the imagic store-locale chain, returning "settledLocale").
            // Mirrors iOS noteSettledLocale / Android noteCloudStoreLocaleSettled. Uses the core
            // Settings setter (NOT QmlSettings), so it does NOT invalidate the cache the lib
            // just wrote; otherwise an international account would thrash the catalog.
            QJsonObject root;
            if (success)
                root = QJsonDocument::fromJson(json.toUtf8()).object();
            if (success) {
                self->catalogDisplayRows_ = self->buildCatalogDisplayRows(root.value(QStringLiteral("games")).toArray());
                self->catalogTotalGames_ = self->catalogDisplayRows_.size();
            } else {
                self->catalogDisplayRows_.clear();
                self->catalogTotalGames_ = 0;
            }
            if (success && self->settings) {
                const QString settled = root.value(QStringLiteral("settledLocale")).toString();
                if (!settled.isEmpty() && settled != self->settings->GetCloudStoreLocale())
                    self->settings->SetCloudStoreLocale(settled);
                self->settings->SetCloudResolvedStoreCountry(root.value(QStringLiteral("fallbackRegion")).toString());
                self->settings->SetCloudResolvedStoreLang(root.value(QStringLiteral("resolvedStoreLang")).toString());
                self->settings->SetCloudCatalogNativeMode(root.value(QStringLiteral("nativeMode")).toBool(true));
            }

            QJSValue payload;
            if (success) {
                QJsonObject meta;
                meta[QStringLiteral("totalGames")] = self->catalogTotalGames_;
                meta[QStringLiteral("fallbackRegion")] = root.value(QStringLiteral("fallbackRegion"));
                meta[QStringLiteral("nativeMode")] = root.value(QStringLiteral("nativeMode"));
                meta[QStringLiteral("warning")] = root.value(QStringLiteral("warning"));
                meta[QStringLiteral("settledLocale")] = root.value(QStringLiteral("settledLocale"));
                if (QJSEngine *eng = qjsEngine(self.data()))
                    payload = eng->toScriptValue(meta);
                else
                    payload = QJSValue(QString::fromUtf8(QJsonDocument(meta).toJson(QJsonDocument::Compact)));
            }
            if (cb.isCallable())
                cb.call({ success, message, payload });
            for (QJSValue &pcb : parked)
                if (pcb.isCallable())
                    pcb.call({ success, message, payload });
        }, Qt::QueuedConnection);
    }).detach();
}

void CloudCatalogBackend::fetchGameDetails(const QString &productId, const QJSValue &callback)
{
    // Check cache first
    QString cacheKey = QString("game_details_%1").arg(productId);
    qInfo() << "[fetchGameDetails] Checking cache for:" << productId << "cache key:" << cacheKey;
    QString cached = getCachedData(cacheKey, CACHE_DURATION_DETAILS);
    if (!cached.isEmpty()) {
        qInfo() << "[CACHE] Using cached game details for:" << productId << "(cache key:" << cacheKey << ")";
        QJsonDocument doc = QJsonDocument::fromJson(cached.toUtf8());
        if (callback.isCallable()) {
            callback.call({true, "Cached", QJSValue(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)))});
        }
        return;
    }
    
    qInfo() << "[API CALL] Fetching game details from API for:" << productId << "(cache key:" << cacheKey << ", cache miss)";
    
    gameDetailsState.callback = callback;
    gameDetailsState.productId = productId;
    
    // Apply 100ms cooldown before making API call
    QTimer::singleShot(100, this, [this, productId]() {
        executeGameDetailsFetch(productId);
    });
}

void CloudCatalogBackend::executeGameDetailsFetch(const QString &productId)
{
    // Get locale from unified language setting
    QString localeSetting = settings ? settings->GetCloudStoreLocale() : "en-US";
    QString locale = localeSetting.toLower(); // Convert "en-US" to "en-us"
    
    // Extract country and language from locale (e.g., "en-us" -> "US", "en")
    QStringList localeParts = locale.split("-");
    QString country = localeParts.size() > 1 ? localeParts[1].toUpper() : "US";
    QString language = localeParts[0].toLower();
    
    // Check if productId looks like a title ID (ends with _00) or is a full product ID
    QString url;
    bool isTitleId = productId.contains("_00") && productId.length() <= 15; // Title IDs are short like "PPSA01325_00"
    
    if (isTitleId) {
        // It's a title ID, use store API directly
        url = QString("https://store.playstation.com/store/api/chihiro/00_09_000/container/%1/%2/999/%3/0")
            .arg(country, language, productId);
    } else {
        // It's a product ID, try PSNOW API first
        url = QString("https://psnow.playstation.com/store/api/pcnow/00_09_000/container/%1/%2/19/%3?useOffers=true&gkb=1&gkb2=1")
            .arg(country, language, productId);
    }
    
    if (settings && settings->GetLogVerbose()) {
        qInfo() << "=== CloudCatalogBackend: Fetching game details ===";
        qInfo() << "  Product/Title ID:" << productId;
        qInfo() << "  URL:" << url;
        qInfo() << "  Method: GET";
    }
    
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    
    QNetworkReply *reply = networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, &CloudCatalogBackend::handleGameDetailsResponse);
}

void CloudCatalogBackend::handleGameDetailsResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    
    if (settings && settings->GetLogVerbose()) {
        qInfo() << "=== CloudCatalogBackend: Game Details Response ===";
        qInfo() << "  Product ID:" << gameDetailsState.productId;
        qInfo() << "  Status:" << statusCode;
    }
    
    reply->deleteLater();
    
    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "Game details fetch error:" << reply->errorString();
        if (gameDetailsState.callback.isCallable()) {
            gameDetailsState.callback.call({false, reply->errorString(), QJSValue()});
        }
        return;
    }
    
    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    
    if (!doc.isObject()) {
        if (gameDetailsState.callback.isCallable()) {
            gameDetailsState.callback.call({false, "Invalid response format", QJSValue()});
        }
        return;
    }
    
    QJsonObject gameData = doc.object();
    
    // Check if images are in links[0].images (store API format)
    QJsonArray imagesArray;
    if (gameData.contains("images") && gameData["images"].isArray()) {
        imagesArray = gameData["images"].toArray();
    } else if (gameData.contains("links") && gameData["links"].isArray()) {
        QJsonArray links = gameData["links"].toArray();
        if (!links.isEmpty() && links[0].isObject()) {
            QJsonObject firstLink = links[0].toObject();
            if (firstLink.contains("images") && firstLink["images"].isArray()) {
                imagesArray = firstLink["images"].toArray();
                if (settings && settings->GetLogVerbose()) {
                    qInfo() << "  Found images in links[0].images, count:" << imagesArray.size();
                }
            }
        }
    }
    
    // If we found images, add them to gameData for extraction
    if (!imagesArray.isEmpty()) {
        gameData["images"] = imagesArray;
    }
    
    // Extract and organize images
    QJsonObject images = extractGameImages(gameData);
    gameData["extracted_images"] = images;
    
    if (settings && settings->GetLogVerbose()) {
        qInfo() << "  Game name:" << gameData["name"].toString();
        qInfo() << "  Cover image:" << (images["cover"].toString().isEmpty() ? "None" : "Found");
        qInfo() << "  Landscape image:" << (images["landscape"].toString().isEmpty() ? "None" : "Found");
    }
    
    QJsonDocument resultDoc(gameData);
    
    // Cache the result
    QString cacheKey = QString("game_details_%1").arg(gameDetailsState.productId);
    qInfo() << "[API CALL] Saving game details to cache for:" << gameDetailsState.productId << "(cache key:" << cacheKey << ")";
    setCachedData(cacheKey, resultDoc);
    qInfo() << "[API CALL] Game details saved to cache successfully";
    
    // Call callback
    if (gameDetailsState.callback.isCallable()) {
        QString jsonStr = QString::fromUtf8(resultDoc.toJson(QJsonDocument::Compact));
        qInfo() << "[API CALL] Calling callback with fetched game details for:" << gameDetailsState.productId;
        gameDetailsState.callback.call({true, "Success", QJSValue(jsonStr)});
    }
}

QJsonObject CloudCatalogBackend::extractGameImages(const QJsonObject &gameData)
{
    QJsonObject images;
    QString coverUrl;
    QString landscapeUrl;
    
    if (gameData.contains("images") && gameData["images"].isArray()) {
        QJsonArray imagesArray = gameData["images"].toArray();
        
        for (const QJsonValue &img : imagesArray) {
            if (img.isObject()) {
                QJsonObject imgObj = img.toObject();
                int type = imgObj["type"].toInt();
                QString url = imgObj["url"].toString();
                
                // Type 10 = cover/box art
                if (type == 10 && coverUrl.isEmpty()) {
                    coverUrl = url;
                }
                // Type 12 = landscape 1080p (preferred)
                else if (type == 12 && landscapeUrl.isEmpty()) {
                    landscapeUrl = url;
                }
                // Type 13 = landscape 720p (fallback)
                else if (type == 13 && landscapeUrl.isEmpty()) {
                    landscapeUrl = url;
                }
            }
        }
    }
    
    images["cover"] = coverUrl;
    images["landscape"] = landscapeUrl;
    
    return images;
}

QString CloudCatalogBackend::findProductIdForEntitlement(const QString &entitlementId)
{
    QString libraryCached = getCachedData("ps5_cloud_library", INT_MAX);
    if (libraryCached.isEmpty())
        return entitlementId;
    QJsonDocument libraryDoc = QJsonDocument::fromJson(libraryCached.toUtf8());
    if (!libraryDoc.isObject())
        return entitlementId;
    QJsonObject libraryRoot = libraryDoc.object();
    if (!libraryRoot.contains("games") || !libraryRoot["games"].isArray())
        return entitlementId;
    QJsonArray libraryGames = libraryRoot["games"].toArray();
    for (const QJsonValue &gameValue : libraryGames) {
        if (!gameValue.isObject()) continue;
        QJsonObject game = gameValue.toObject();
        if (!game.contains("id") || game["id"].toString() != entitlementId) continue;
        if (game.contains("product_id")) {
            QString productId = game["product_id"].toString();
            if (!productId.isEmpty())
                return productId;
        }
        // Fallback: id itself (same as input — return it explicitly so callers can detect no-op)
        if (game.contains("id")) {
            QString id = game["id"].toString();
            if (!id.isEmpty())
                return id;
        }
    }
    return entitlementId;
}

QString CloudCatalogBackend::getGameLandscapeImageFromCache(const QString &serviceType, const QString &gameIdentifier)
{
    if (gameIdentifier.isEmpty()) {
        return QString();
    }
    
    // Determine cache file based on service type
    QString cacheKey;
    QString productIdForCatalog; // For PSCloud: productId to use in catalog lookup
    
    if (serviceType.toLower() == "psnow") {
        // The lib owns the unified catalog filename and bumps its version suffix, so
        // resolve it by glob (newest unified_catalog_v*.json) like getOwnedPsnowEntitlement
        // does, rather than hard-coding the current version.
        QDir dir(cacheDirectory);
        QFileInfoList matches = dir.entryInfoList({QStringLiteral("unified_catalog_v*.json")},
                                                  QDir::Files, QDir::Time);
        if (matches.isEmpty()) {
            qInfo() << "getGameLandscapeImage: no unified catalog cache file present";
            return QString();
        }
        cacheKey = matches.first().completeBaseName();
    } else if (serviceType.toLower() == "pscloud") {
        // For PSCloud, gameIdentifier is an entitlement ID; resolve productId from library.
        productIdForCatalog = findProductIdForEntitlement(gameIdentifier);
        qInfo() << "getGameLandscapeImage: resolved productId" << productIdForCatalog << "for entitlement ID" << gameIdentifier;

        // Try game details cache first (has landscape images from API)
        // Use very large maxAge to never invalidate cache (read-only operation)
        QString lookupId = productIdForCatalog.isEmpty() ? gameIdentifier : productIdForCatalog;
        QString gameDetailsCacheKey = QString("game_details_%1").arg(lookupId);
        QString gameDetailsCached = getCachedData(gameDetailsCacheKey, INT_MAX);
        if (!gameDetailsCached.isEmpty()) {
            qInfo() << "getGameLandscapeImage: Found game details cache for" << lookupId;
            QJsonDocument gameDetailsDoc = QJsonDocument::fromJson(gameDetailsCached.toUtf8());
            if (gameDetailsDoc.isObject()) {
                QJsonObject gameDetailsObj = gameDetailsDoc.object();
                if (gameDetailsObj.contains("extracted_images")) {
                    QJsonObject extracted = gameDetailsObj["extracted_images"].toObject();
                    QString landscape = extracted["landscape"].toString();
                    if (!landscape.isEmpty()) {
                        qInfo() << "getGameLandscapeImage: Using landscape image from game details cache:" << landscape;
                        return landscape;
                    }
                    // Fallback to cover if landscape not available
                    QString cover = extracted["cover"].toString();
                    if (!cover.isEmpty()) {
                        qInfo() << "getGameLandscapeImage: Using cover image from game details cache (landscape not available):" << cover;
                        return cover;
                    }
                }
            }
            qInfo() << "getGameLandscapeImage: Game details cache found but no images, falling back to catalog";
        } else {
            qInfo() << "getGameLandscapeImage: Game details cache not found for" << lookupId << ", falling back to catalog";
        }
        
        // Fallback to catalog (may not have landscape images)
        cacheKey = "ps5_cloud_catalog_v6";
    } else {
        qWarning() << "getGameLandscapeImage: Unknown service type:" << serviceType;
        return QString();
    }
    
    // Load cache - use very large maxAge to never invalidate cache (read-only operation)
    QString cached = (cacheKey == QLatin1String("ps5_cloud_catalog_v6"))
                         ? getCachedPs5CatalogV3(INT_MAX)
                         : getCachedData(cacheKey, INT_MAX);
    if (cached.isEmpty()) {
        qInfo() << "getGameLandscapeImage: Cache not available for" << cacheKey;
        return QString();
    }
    
    // Parse JSON
    QJsonDocument doc = QJsonDocument::fromJson(cached.toUtf8());
    if (!doc.isObject()) {
        qWarning() << "getGameLandscapeImage: Invalid cache format for" << cacheKey;
        return QString();
    }
    
    QJsonObject root = doc.object();
    if (!root.contains("games") || !root["games"].isArray()) {
        qWarning() << "getGameLandscapeImage: No games array in cache";
        return QString();
    }
    
    QJsonArray games = root["games"].toArray();
    
    // Find game by identifier
    QJsonObject gameObj;
    bool found = false;
    
    for (const QJsonValue &gameValue : games) {
        if (!gameValue.isObject()) continue;
        
        QJsonObject game = gameValue.toObject();
        
        // Match based on service type
        if (serviceType.toLower() == "psnow") {
            // PSNOW: unified catalog rows may carry the identifier under several keys.
            if ((game.contains("id") && game["id"].toString() == gameIdentifier)
                || (game.contains("storeProductId") && game["storeProductId"].toString() == gameIdentifier)
                || (game.contains("productId") && game["productId"].toString() == gameIdentifier)) {
                gameObj = game;
                found = true;
                break;
            }
        } else if (serviceType.toLower() == "pscloud") {
            // PSCloud catalog: Match by "productId" field
            // Use productIdForCatalog if we found it from library, otherwise try gameIdentifier directly
            QString lookupId = productIdForCatalog.isEmpty() ? gameIdentifier : productIdForCatalog;
            if (game.contains("productId") && game["productId"].toString() == lookupId) {
                gameObj = game;
                found = true;
                break;
            }
        }
    }
    
    if (!found) {
        qInfo() << "getGameLandscapeImage: Game not found in cache:" << cacheKey << "with identifier:" << gameIdentifier;
        if (!productIdForCatalog.isEmpty()) {
            qInfo() << "getGameLandscapeImage: Tried productId:" << productIdForCatalog << "from library lookup";
        }
        return QString();
    }
    
    qInfo() << "getGameLandscapeImage: Found game in" << cacheKey << "for identifier:" << gameIdentifier;
    
    // Extract landscape image using priority order
    // Priority 1: images array (type 12 → 13 → 10 → any)
    if (gameObj.contains("images") && gameObj["images"].isArray()) {
        QJsonArray images = gameObj["images"].toArray();
        qInfo() << "getGameLandscapeImage: Found images array with" << images.size() << "images for" << gameIdentifier;
        
        QString type12, type13, type10, anyType;
        QList<int> foundTypes;
        
        for (const QJsonValue &img : images) {
            if (!img.isObject()) continue;
            
            QJsonObject imgObj = img.toObject();
            int type = imgObj["type"].toInt();
            QString url = imgObj["url"].toString();
            foundTypes.append(type);
            
            qInfo() << "getGameLandscapeImage: Image type" << type << "URL:" << url;
            
            if (type == 12 && type12.isEmpty()) {
                type12 = url;
            } else if (type == 13 && type13.isEmpty()) {
                type13 = url;
            } else if (type == 10 && type10.isEmpty()) {
                type10 = url;
            } else if (anyType.isEmpty()) {
                anyType = url;
            }
        }
        
        qInfo() << "getGameLandscapeImage: Available image types:" << foundTypes;
        
        if (!type12.isEmpty()) {
            qInfo() << "getGameLandscapeImage: Using type 12 (landscape 1080p) for" << gameIdentifier << "URL:" << type12;
            return type12;
        }
        if (!type13.isEmpty()) {
            qInfo() << "getGameLandscapeImage: Using type 13 (landscape 720p) for" << gameIdentifier << "URL:" << type13;
            return type13;
        }
        if (!type10.isEmpty()) {
            qInfo() << "getGameLandscapeImage: Using type 10 (cover) for" << gameIdentifier << "URL:" << type10;
            return type10;
        }
        if (!anyType.isEmpty()) {
            qInfo() << "getGameLandscapeImage: Using any image type for" << gameIdentifier << "URL:" << anyType;
            return anyType;
        }
        qInfo() << "getGameLandscapeImage: No valid images found in images array for" << gameIdentifier;
    } else {
        qInfo() << "getGameLandscapeImage: No images array found in game object for" << gameIdentifier;
    }
    
    // Priority 2: imageUrl (cover image)
    if (gameObj.contains("imageUrl")) {
        QString imageUrl = gameObj["imageUrl"].toString();
        if (!imageUrl.isEmpty()) {
            qInfo() << "getGameLandscapeImage: Using imageUrl (fallback) for" << gameIdentifier << "URL:" << imageUrl;
            return imageUrl;
        }
    }
    
    qInfo() << "getGameLandscapeImage: No image found for" << gameIdentifier << "in catalog:" << cacheKey;
    return QString();
}

void CloudCatalogBackend::invalidateCache()
{
    // Mark any in-flight unified fetch stale FIRST: its completion handler compares
    // its snapshot against this counter and discards + restarts instead of serving
    // (and re-persisting) a result computed with the pre-invalidation account/locale.
    catalogGeneration++;
    catalogDisplayRows_.clear();
    catalogTotalGames_ = 0;
    psnowSearchRows_.clear();
    psnowSearchTotalGames_ = 0;
    sessionCatalogNpsso_.clear();
    // libchiaki owns every cache file and its versioned key (current + legacy), so
    // delegate to it. This is the single source of truth for cache naming and keeps
    // the client from drifting out of sync when the cache schema/version bumps.
    const QByteArray cacheDir = cacheDirectory.toUtf8();
    chiaki_cloudcatalog_invalidate_cache(cacheDir.constData());
    const QString searchCacheDir = cacheDirectory + QStringLiteral("/psnow_search");
    chiaki_cloudcatalog_invalidate_cache(searchCacheDir.toUtf8().constData());
    purgeStaleBillingCatalogCaches();
    QFile::remove(getCacheFilePath(billingCatalogCacheKey()));
    qInfo() << "[CACHE INVALIDATED] Delegated cache invalidation to libchiaki for" << cacheDirectory;
    // Tell the cloud view to drop its stale in-memory list and re-fetch (the cache files are gone,
    // so the next fetch is a guaranteed network refresh for the now-current account).
    emit cacheInvalidated();
}

QPixmap CloudCatalogBackend::downloadImageFromUrl(const QString &url, int timeoutMs)
{
    if (url.isEmpty()) {
        return QPixmap();
    }
    
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    
    // Configure SSL
    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone); // Accept any certificate for CDN images
    request.setSslConfiguration(sslConfig);
    
    QNetworkReply *reply = networkManager->get(request);
    
    QEventLoop loop;
    QTimer timeout_timer;
    timeout_timer.setSingleShot(true);
    
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeout_timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    
    timeout_timer.start(timeoutMs);
    loop.exec();
    
    QPixmap pixmap;
    if (timeout_timer.isActive() && reply->error() == QNetworkReply::NoError) {
        timeout_timer.stop();
        QByteArray data = reply->readAll();
        pixmap.loadFromData(data);
        qInfo() << "Downloaded image from" << url << "size:" << pixmap.size();
    } else {
        if (!timeout_timer.isActive()) {
            qWarning() << "Timeout downloading image from" << url;
        } else {
            qWarning() << "Failed to download image from" << url << "error:" << reply->error() << reply->errorString();
        }
    }
    
    reply->deleteLater();
    return pixmap;
}

QPixmap CloudCatalogBackend::resizeImageToFit(const QPixmap &source, int targetWidth, int targetHeight)
{
    // Return empty pixmap if source is null/empty (graceful handling)
    if (source.isNull() || source.width() == 0 || source.height() == 0) {
        return QPixmap();
    }
    
    // Create heavily blurred background using multiple-pass downscale/upscale technique
    // First, scale to fill the target dimensions (stretched)
    QPixmap stretched = source.scaled(targetWidth, targetHeight, 
                                      Qt::IgnoreAspectRatio, 
                                      Qt::SmoothTransformation);
    
    // Create extreme blur effect with multiple passes for smooth result
    // Pass 1: Aggressive downscale for extreme blur
    int blurSize1 = qMax(targetWidth, targetHeight) / 80;  // Very small for extreme blur
    QPixmap downscaled1 = stretched.scaled(blurSize1, blurSize1, 
                                           Qt::IgnoreAspectRatio, 
                                           Qt::SmoothTransformation);
    
    // Pass 2: Intermediate upscale for smoother blur
    int blurSize2 = qMax(targetWidth, targetHeight) / 40;
    QPixmap intermediate = downscaled1.scaled(blurSize2, blurSize2, 
                                              Qt::IgnoreAspectRatio, 
                                              Qt::SmoothTransformation);
    
    // Pass 3: Another intermediate pass for extra smoothness
    int blurSize3 = qMax(targetWidth, targetHeight) / 20;
    QPixmap intermediate2 = intermediate.scaled(blurSize3, blurSize3, 
                                                Qt::IgnoreAspectRatio, 
                                                Qt::SmoothTransformation);
    
    // Final upscale to target size
    QPixmap blurredBackground = intermediate2.scaled(targetWidth, targetHeight, 
                                                     Qt::IgnoreAspectRatio, 
                                                     Qt::SmoothTransformation);
    
    // Darken the background extremely for minimal distraction
    QPainter bgPainter(&blurredBackground);
    bgPainter.setCompositionMode(QPainter::CompositionMode_Darken);
    bgPainter.fillRect(blurredBackground.rect(), QColor(0, 0, 0, 210));  // ~90% darker, nearly black
    bgPainter.end();
    
    // Scale source maintaining aspect ratio for the centered foreground
    QPixmap scaled = source.scaled(targetWidth, targetHeight, 
                                    Qt::KeepAspectRatio, 
                                    Qt::SmoothTransformation);
    
    // Calculate position to center the scaled image
    int x = (targetWidth - scaled.width()) / 2;
    int y = (targetHeight - scaled.height()) / 2;
    
    // Draw scaled image centered on blurred background
    QPainter painter(&blurredBackground);
    painter.drawPixmap(x, y, scaled);
    painter.end();
    
    qInfo() << "Resized image from" << source.size() 
           << "to" << blurredBackground.size() 
           << "(scaled:" << scaled.size() << ", with blurred background)";
    
    return blurredBackground;
}

void CloudCatalogBackend::createCloudSteamShortcut(const QString &gameIdentifier, const QString &gameName, 
                                                   const QString &command, const QJSValue &callback, 
                                                   const QString &steamDir)
{
    qInfo() << "=== CREATE CLOUD STEAM SHORTCUT START ===";
    qInfo() << "Game Identifier:" << gameIdentifier;
    qInfo() << "Game Name:" << gameName;
    qInfo() << "Command:" << command;
    qInfo() << "Steam Dir:" << steamDir;
    
    QJSValue cb = callback;
    
    auto infoLambda = [callback](const QString &infoMessage) {
        qInfo() << "[INFO]" << infoMessage;
        QJSValue icb = callback;
        if (icb.isCallable())
            icb.call({infoMessage, true, false});
    };

    auto errorLambda = [callback](const QString &errorMessage) {
        qWarning() << "[ERROR]" << errorMessage;
        QJSValue icb = callback;
        if (icb.isCallable())
            icb.call({errorMessage, false, true});
    };

#ifndef CHIAKI_GUI_ENABLE_STEAM_SHORTCUT
    if (cb.isCallable())
        cb.call({QString("[E] Steam shortcuts are not available in this build."), false, true});
    return;
#else

    // Validate command
    if (command != "cloudGameCatalog" && command != "cloudGameLibrary") {
        errorLambda("[E] Invalid command. Must be 'cloudGameCatalog' or 'cloudGameLibrary'");
        return;
    }
    
    // For PSCloud (cloudGameLibrary), gameIdentifier is entitlement ID, need to look up product_id.
    // For PSNOW (cloudGameCatalog), gameIdentifier is already the product ID.
    QString productIdForCache = (command == "cloudGameLibrary")
        ? findProductIdForEntitlement(gameIdentifier)
        : gameIdentifier;
    if (command == "cloudGameLibrary")
        qInfo() << "createCloudSteamShortcut: resolved productId" << productIdForCache << "for entitlement ID" << gameIdentifier;
    
    // Get cached game details using product ID
    QString cacheKey = QString("game_details_%1").arg(productIdForCache);
    QString cachedDetails = getCachedData(cacheKey, 7 * 24 * 60 * 60 * 1000); // 7 days cache
    
    if (cachedDetails.isEmpty()) {
        qWarning() << "No cached game details for" << productIdForCache << "(looked up from gameIdentifier:" << gameIdentifier << ")";
        if (cb.isCallable())
            cb.call({QString("[E] No cached game details for %1. Please wait for game details to load first.").arg(gameName), false, true});
        return;
    }
    
    infoLambda(QString("[I] Fetching artwork for %1...").arg(gameName));
    
    // Parse cached game details
    QJsonDocument doc = QJsonDocument::fromJson(cachedDetails.toUtf8());
    if (!doc.isObject()) {
        errorLambda("[E] Failed to parse cached game details JSON");
        return;
    }
    
    QJsonObject gameData = doc.object();
    QJsonObject extractedImages = gameData["extracted_images"].toObject();
    
    QString coverUrl = extractedImages["cover"].toString();
    QString landscapeUrl = extractedImages["landscape"].toString();
    
    qInfo() << "Cover URL:" << coverUrl;
    qInfo() << "Landscape URL:" << landscapeUrl;
    
    // Download images
    infoLambda("[I] Downloading hero image...");
    QPixmap hero;
    if (!landscapeUrl.isEmpty()) {
        hero = downloadImageFromUrl(landscapeUrl);
    }
    if (hero.isNull() && !coverUrl.isEmpty()) {
        hero = downloadImageFromUrl(coverUrl);
    }
    if (!hero.isNull()) {
        infoLambda("[I] Resizing hero image to 1920x620...");
        hero = resizeImageToFit(hero, 1920, 620);
    }
    
    infoLambda("[I] Downloading landscape image...");
    QPixmap landscape;
    if (!landscapeUrl.isEmpty()) {
        landscape = downloadImageFromUrl(landscapeUrl);
    }
    if (landscape.isNull() && !coverUrl.isEmpty()) {
        landscape = downloadImageFromUrl(coverUrl);
    }
    if (!landscape.isNull()) {
        infoLambda("[I] Resizing landscape image to 920x430...");
        landscape = resizeImageToFit(landscape, 920, 430);
    }
    
    infoLambda("[I] Downloading portrait image...");
    QPixmap portrait;
    if (!coverUrl.isEmpty()) {
        portrait = downloadImageFromUrl(coverUrl);
    }
    if (!portrait.isNull()) {
        infoLambda("[I] Resizing portrait image to 600x900...");
        portrait = resizeImageToFit(portrait, 600, 900);
    }
    
    // Load fixed assets
    qInfo() << "Loading fixed assets...";
    QPixmap icon(":/icons/game_shortcut_icon.png");
    // Same Pylux logo the app-level startup shortcut uses (the old game_shortcut_logo.png
    // still carried PS Stream branding)
    QPixmap logo(":/icons/steam_logo.png");
    
    if (icon.isNull()) {
        qWarning() << "Failed to load game shortcut icon, using fallback";
        icon = QPixmap(":/icons/steam_icon.png");
    }
    if (logo.isNull())
        qWarning() << "Failed to load game shortcut logo";

    // Create artwork map
    QMap<QString, const QPixmap*> artwork;
    
    if (landscape.isNull()) {
        auto fallback = QPixmap(":/icons/steam_landscape.png");
        artwork.insert("landscape", new QPixmap(fallback));
    } else {
        artwork.insert("landscape", new QPixmap(landscape));
    }
    
    if (portrait.isNull()) {
        auto fallback = QPixmap(":/icons/steam_portrait.png");
        artwork.insert("portrait", new QPixmap(fallback));
    } else {
        artwork.insert("portrait", new QPixmap(portrait));
    }
    
    if (hero.isNull()) {
        QImageReader reader;
        reader.setAllocationLimit(512);
        reader.setFileName(":/icons/steam_hero.png");
        auto fallback = QPixmap::fromImageReader(&reader);
        artwork.insert("hero", new QPixmap(fallback));
    } else {
        artwork.insert("hero", new QPixmap(hero));
    }
    
    artwork.insert("icon", new QPixmap(icon));
    artwork.insert("logo", new QPixmap(logo));
    
    // Build launch options based on command
    qInfo() << "Building launch options with" << command << "command...";
    QString escaped_identifier = gameIdentifier;
    escaped_identifier.replace("\"", "\\\"");  // Escape quotes for shell safety
    
    QString launch_options;
    if (command == "cloudGameCatalog") {
        launch_options = QString("--product-id \"%1\" cloudGameCatalog").arg(escaped_identifier);
    } else { // cloudGameLibrary
        launch_options = QString("--entitlement-id \"%1\" cloudGameLibrary").arg(escaped_identifier);
    }
    
    qInfo() << "Launch options:" << launch_options;
    infoLambda(QString("[I] Creating Steam shortcut with launch options: %1").arg(launch_options));
    
    // Initialize SteamTools
    qInfo() << "Initializing SteamTools with steamDir:" << steamDir;
    SteamTools* steam_tools = new SteamTools(infoLambda, errorLambda, steamDir);
    
    qInfo() << "Checking if Steam exists...";
    bool steamExists = steam_tools->steamExists();
    qInfo() << "Steam exists:" << steamExists;
    
    if (!steamExists) {
        qWarning() << "Steam does not exist, cannot create shortcut";
        if (cb.isCallable())
            cb.call({QString("[E] Steam does not exist, cannot create Steam Shortcut"), false, true});
        
        // Clean up artwork
        for (auto it = artwork.begin(); it != artwork.end(); ++it) {
            delete it.value();
        }
        delete steam_tools;
        return;
    }
    
    // Get executable path
    QString executable = QCoreApplication::applicationFilePath();
    qInfo() << "Application executable path:" << executable;
    
    #ifdef Q_OS_LINUX
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        // In a Flatpak sandbox applicationFilePath() is /app/bin/... which doesn't
        // exist on the host; Steam must invoke the host's flatpak command instead
        // (same as QmlBackend::getExecutable())
        if (!env.value("FLATPAK_ID").isEmpty()) {
            executable = QStringLiteral("flatpak");
        } else if (env.contains("APPIMAGE")) {
            executable = env.value("APPIMAGE");
            qInfo() << "Running as AppImage, using:" << executable;
        }
    #endif
    
    // Check for Flatpak
    if (executable == "flatpak") {
        const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QString flatpakId = env.value("FLATPAK_ID");
        launch_options.prepend(QString("run %1 ").arg(flatpakId));
        qInfo() << "Running as Flatpak, updated launch options:" << launch_options;
    }
    
    // If running from extracted pylux directory, use launch.sh instead of direct executable
    if (executable != "flatpak" && !executable.endsWith(".AppImage"))
    {
        QFileInfo exeInfo(executable);
        QString exePath = exeInfo.absoluteFilePath();
        
        if (exePath.contains("/usr/bin/"))
        {
            QDir exeDir(exeInfo.absolutePath());
            if (exeDir.cdUp() && exeDir.cdUp())
            {
                QString launchScript = exeDir.absoluteFilePath("launch.sh");
                if (QFile::exists(launchScript))
                {
                    qInfo() << "Using launch.sh for cloud game Steam shortcut:" << launchScript;
                    executable = launchScript;
                }
            }
        }
    }
    
    // Build the shortcut
    qInfo() << "Building shortcut entry...";
    QString shortcut_name = gameName;
    SteamShortcutEntry newShortcut = steam_tools->buildShortcutEntry(
        std::move(shortcut_name), 
        std::move(executable), 
        std::move(launch_options), 
        std::move(artwork)
    );
    qInfo() << "Shortcut entry built successfully";
    
    // Parse existing shortcuts
    qInfo() << "Parsing existing shortcuts...";
    QVector<SteamShortcutEntry> shortcuts = steam_tools->parseShortcuts();
    qInfo() << "Found" << shortcuts.size() << "existing shortcuts";
    
    bool found = false;
    
    // Check if shortcut already exists
    qInfo() << "Checking if shortcut already exists...";
    for (int i = 0; i < shortcuts.size(); ++i) {
        if (shortcuts[i].getAppName() == newShortcut.getAppName()) {
            qInfo() << "Found existing shortcut at index" << i << ", updating...";
            infoLambda(QString("[I] Updating existing shortcut for %1").arg(newShortcut.getAppName()));
            shortcuts[i] = newShortcut;
            found = true;
            break;
        }
    }
    
    if (!found) {
        qInfo() << "No existing shortcut found, adding new one";
        infoLambda(QString("[I] Adding new shortcut for %1").arg(newShortcut.getAppName()));
        shortcuts.append(newShortcut);
    }
    
    // Update shortcuts
    qInfo() << "Updating shortcuts file with" << shortcuts.size() << "total shortcuts...";
    steam_tools->updateShortcuts(shortcuts);
    qInfo() << "Shortcuts updated successfully";
    
    // Update controller config for Steam Deck
    QString controller_layout_workshop_id = "3049833406";
    qInfo() << "Updating Steam Deck controller config with workshop ID:" << controller_layout_workshop_id;
    try {
        steam_tools->updateControllerConfig(newShortcut.getAppName(), std::move(controller_layout_workshop_id));
    } catch (const std::exception& e) {
        qWarning() << "Failed to update Steam controller config:" << e.what();
    }
    
    infoLambda("[I] Successfully created Steam shortcut!");
    infoLambda("");
    infoLambda("══════════════════════════════════════════════════════");
    infoLambda("✓ SHORTCUT CREATED SUCCESSFULLY!");
    infoLambda("══════════════════════════════════════════════════════");
    infoLambda("");
    infoLambda(QString("→ Game: %1").arg(gameName));
    infoLambda("");
    infoLambda("⚠ IMPORTANT: Please restart Steam for the shortcut to appear!");
    infoLambda("");
    qInfo() << "Calling final callback with done=true, ok=true";
    qInfo() << "Callback is callable:" << cb.isCallable();
    if (cb.isCallable()) {
        QJSValue result = cb.call({QString("Shortcut created successfully for %1").arg(gameName), true, true});
        qInfo() << "Callback call result:" << (result.isError() ? result.toString() : "success");
        if (result.isError()) {
            qWarning() << "Callback error:" << result.toString();
        }
    } else {
        qWarning() << "Callback is not callable!";
    }
    
    // Clean up artwork
    for (auto it = artwork.begin(); it != artwork.end(); ++it) {
        delete it.value();
    }
    delete steam_tools;

#endif // CHIAKI_GUI_ENABLE_STEAM_SHORTCUT
}


