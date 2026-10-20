// SPDX-License-Identifier: MIT
#include "connectlifeapi.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QVariantList>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

namespace {

constexpr const char *kBaseUrl = "https://clife-eu-gateway.hijuconn.com";
constexpr const char *kGigyaUrl = "https://accounts.eu1.gigya.com";
constexpr const char *kOauthUrl = "https://oauth.hijuconn.com";
constexpr const char *kRedirectUri =
    "https://api.connectlife.io/swagger/oauth2-redirect.html";

// Values lifted from the official mobile app (see
// github.com/bilan/connectlife-api-connector). These are public client
// identifiers, not user secrets.
constexpr const char *kApiKey = "4_yhTWQmHFpZkQZDSV1uV-_A";
constexpr const char *kGmid =
    "gmid.ver4.AtLt3mZAMA.C8m5VqSTEQDrTRrkYYDgOaJWcyQ-XHow5nzQSXJF3EO3TnqTJ8tKUmQaaQ6z8p0s."
    "zcTbHe6Ax6lHfvTN7JUj7VgO4x8Vl-vk1u0kZcrkKmKWw8K9r0shyut_at5Q0ri6zTewnAv2g1Dc8dauuyd-Sw.sc3";
constexpr const char *kClientId = "5065059336212";
constexpr const char *kClientSecret =
    "07swfKgvJhC3ydOUS9YV_SwVz0i4LKqlOLGNUukYHVMsJRF1b-iWeUGcNlXyYCeK";
constexpr const char *kAppId = "47110565134383";
constexpr const char *kAppSecret =
    "yOzhz6junYno-nmULM3Wr7PU_dpSZN22ZdluvVWZ4uW5ZwwG8fIGCHTbrhcnU-iv";
constexpr const char *kSignSalt = "D9519A4B756946F081B7BB5B5E8D1197";
constexpr const char *kUserAgent = "Runner/2.0.6 (iPhone; iOS 17.2.1; Scale/3.00)";

// The gateway throttles aggressive clients ("Api rate limit exceeded"). After
// a rate-limited reply we fail fast for a while instead of sending requests:
// 60 s, doubling per consecutive hit, capped at 30 min. A clean reply resets
// the backoff.
constexpr int kBackoffBaseSec = 60;
constexpr int kBackoffMaxSec = 30 * 60;

// Gigya blocks password logins per IP for much longer, and every retry can
// extend the block. Start at 15 min and double up to 6 h; the refresh token
// path is unaffected and normally avoids Gigya entirely.
constexpr int kLoginBackoffBaseSec = 15 * 60;
constexpr int kLoginBackoffMaxSec = 6 * 3600;

// Gateway error code for "access token invalid/expired". The request can be
// retried once after obtaining a new token.
constexpr int kGatewayInvalidAccessToken = 100026;

// Assumed token lifetime when the token endpoint does not say otherwise.
constexpr int kDefaultTokenLifetimeSec = 12 * 3600;
// Re-login shortly before the token actually dies so no request is sent with
// an expired token.
constexpr int kTokenSafetyMarginSec = 120;

constexpr const char *kPublicKeyPem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAyyWrNG6q475HIHu7sMVu\n"
    "vHof6vlgPeixmxa4EL/UsvVvHPz33NnWoQetQqit9TBNzUjMXw0KlY9PXM4iqHUU\n"
    "U+dSyNDq1jZWIiJ2C2FccppswJtIKL3NRMFvT9PFh6NlP/4FUcQKojgKFbF7Kacc\n"
    "JPKYHlwaO7qgoIjLxAHlSOXGpucJcOkPzT2EqsSVnW8sn8kenvNmghXDayhgxsh6\n"
    "AyxK4kehJplEnmX/iYCfNoFXknGcLqFWYccgBz3fybvx30C/0IgU1980L8QsUAv5\n"
    "esZmN8ugnbRgLRxKRlkQQLxQAiZMZdKTAx665YflT3YMHJvEFE8c2XFgoxHzSMc4\n"
    "BwIDAQAB\n"
    "-----END PUBLIC KEY-----\n";

QJsonObject parseObject(const QByteArray &data)
{
    return QJsonDocument::fromJson(data).object();
}

} // namespace

ConnectLifeApi::ConnectLifeApi(const QString &login, const QString &password,
                               QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
    , m_login(login)
    , m_password(password)
{
    loadTokenCache();
}

QString ConnectLifeApi::tokenCachePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
           + QStringLiteral("/hisenseac/connectlife-token.json");
}

void ConnectLifeApi::loadTokenCache()
{
    QFile file(tokenCachePath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    const QDateTime now = QDateTime::currentDateTimeUtc();

    const QString token = json.value(QStringLiteral("accessToken")).toString();
    const QDateTime expiry = QDateTime::fromString(
        json.value(QStringLiteral("expiry")).toString(), Qt::ISODate);
    if (!token.isEmpty() && expiry.isValid() && expiry > now) {
        m_token = token;
        m_tokenExpiry = expiry;
    }

    // The refresh token is independent of the access token: keep it even when
    // the access token has expired, since it is what avoids another login.
    m_refreshToken = json.value(QStringLiteral("refreshToken")).toString();
    m_refreshExpiry = QDateTime::fromString(
        json.value(QStringLiteral("refreshExpiry")).toString(), Qt::ISODate);

    // Persist the Gigya login block too, so restarting the daemon does not
    // immediately hit the rate-limited login endpoint again.
    const qint64 backoff = json.value(QStringLiteral("loginBackoffUntil"))
                               .toVariant()
                               .toLongLong();
    if (backoff > now.toMSecsSinceEpoch()) {
        m_loginBackoffUntilMs = backoff;
        m_loginRateLimitHits = json.value(QStringLiteral("loginRateLimitHits")).toInt();
    }
}

void ConnectLifeApi::saveTokenCache() const
{
    const QString path = tokenCachePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonObject json;
    json.insert(QStringLiteral("accessToken"), m_token);
    json.insert(QStringLiteral("expiry"), m_tokenExpiry.toString(Qt::ISODate));
    json.insert(QStringLiteral("refreshToken"), m_refreshToken);
    if (m_refreshExpiry.isValid())
        json.insert(QStringLiteral("refreshExpiry"), m_refreshExpiry.toString(Qt::ISODate));
    json.insert(QStringLiteral("loginRateLimitHits"), m_loginRateLimitHits);
    json.insert(QStringLiteral("loginBackoffUntil"), m_loginBackoffUntilMs);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        file.write(QJsonDocument(json).toJson(QJsonDocument::Compact));
    }
}

void ConnectLifeApi::setDeviceId(const QString &puid)
{
    m_deviceId = puid;
}

QVariantMap ConnectLifeApi::commonData()
{
    const qint64 ms = QDateTime::currentMSecsSinceEpoch();
    const QString stamp = QString::number(ms);
    QVariantMap data;
    data.insert(QStringLiteral("appId"), QString::fromLatin1(kAppId));
    data.insert(QStringLiteral("appSecret"), QString::fromLatin1(kAppSecret));
    data.insert(QStringLiteral("languageId"), QStringLiteral("12"));
    data.insert(
        QStringLiteral("randStr"),
        QString::fromLatin1(
            QCryptographicHash::hash(stamp.toUtf8(), QCryptographicHash::Md5).toHex()));
    data.insert(QStringLiteral("timeStamp"), stamp);
    data.insert(QStringLiteral("timezone"), QStringLiteral("1.0"));
    data.insert(QStringLiteral("version"), QStringLiteral("5.0"));
    return data;
}

QString ConnectLifeApi::valueForSign(const QVariant &value)
{
    if (value.typeId() == QMetaType::QVariantMap || value.typeId() == QMetaType::QVariantHash) {
        return QString::fromUtf8(
            QJsonDocument(QJsonObject::fromVariantMap(value.toMap()))
                .toJson(QJsonDocument::Compact));
    }
    if (value.typeId() == QMetaType::QVariantList) {
        return QString::fromUtf8(
            QJsonDocument(QJsonArray::fromVariantList(value.toList()))
                .toJson(QJsonDocument::Compact));
    }
    return value.toString();
}

QByteArray ConnectLifeApi::sign(const QVariantMap &data) const
{
    // Params are sorted by key, joined as "k=v&k=v", salted, hashed with
    // SHA-256 and RSA-encrypted (PKCS#1 v1.5) with the app's public key.
    QMap<QString, QVariant> sorted;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it)
        sorted.insert(it.key(), it.value());

    QString toHash;
    for (auto it = sorted.constBegin(); it != sorted.constEnd(); ++it) {
        toHash += QLatin1Char('&') + it.key() + QLatin1Char('=') + valueForSign(it.value());
    }
    toHash = toHash.mid(1) + QString::fromLatin1(kSignSalt);

    const QByteArray digest =
        QCryptographicHash::hash(toHash.toUtf8(), QCryptographicHash::Sha256); // raw

    BIO *bio = BIO_new_mem_buf(kPublicKeyPem, -1);
    EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey)
        return {};

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, nullptr);
    QByteArray out;
    if (ctx && EVP_PKEY_encrypt_init(ctx) > 0) {
        EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING);
        size_t len = 0;
        if (EVP_PKEY_encrypt(ctx, nullptr, &len,
                             reinterpret_cast<const unsigned char *>(digest.constData()),
                             digest.size())
            > 0) {
            out.resize(static_cast<int>(len));
            if (EVP_PKEY_encrypt(ctx, reinterpret_cast<unsigned char *>(out.data()), &len,
                                 reinterpret_cast<const unsigned char *>(digest.constData()),
                                 digest.size())
                <= 0) {
                out.clear();
            } else {
                out.resize(static_cast<int>(len));
            }
        }
    }
    if (ctx)
        EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    return out.toBase64();
}

bool ConnectLifeApi::looksRateLimited(int httpStatus, const QByteArray &body)
{
    if (httpStatus == 429)
        return true;
    // The gateway and Gigya both report throttling inside an HTTP 200 body
    // (e.g. "Api rate limit exceeded", "Exceeded API call rate limit").
    const QByteArray lower = body.toLower();
    return lower.contains("rate limit") || lower.contains("too many requests");
}

bool ConnectLifeApi::isAuthRequest(const QString &path)
{
    // Both the Gigya endpoints (/accounts.login, /accounts.getJWT) and the
    // OAuth token exchange are subject to the login rate limit, which is much
    // stricter and longer-lived than the gateway's.
    return path.startsWith(QLatin1String("/accounts."))
           || path.startsWith(QLatin1String("/oauth/"));
}

void ConnectLifeApi::noteRequestOutcome(const QString &what, int httpStatus,
                                        const QByteArray &body)
{
    const bool auth = isAuthRequest(what);
    if (!looksRateLimited(httpStatus, body)) {
        if (!auth) {
            m_rateLimitHits = 0;
            m_backoffUntilMs = 0;
        }
        // A successful auth request clears the login backoff in
        // applyTokenResponse(). Do not clear it for other auth replies: an
        // invalid refresh token must not bypass an active Gigya block.
        return;
    }

    // Surface what the server actually said: the message hides in different
    // fields depending on which service (Gigya / OAuth / gateway) throttled us.
    QString detail;
    const QJsonObject obj = parseObject(body);
    const QJsonObject response = obj.value(QStringLiteral("response")).toObject();
    for (const char *key : {"message", "errorMessage", "error_description", "errorDesc"}) {
        QString value = obj.value(QLatin1String(key)).toString();
        if (value.isEmpty())
            value = response.value(QLatin1String(key)).toString();
        if (!value.isEmpty()) {
            detail = value;
            break;
        }
    }
    if (detail.isEmpty())
        detail = QString::fromUtf8(body.left(300));

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (auth) {
        ++m_loginRateLimitHits;
        const int shift = qMin(m_loginRateLimitHits - 1, 6);
        const int seconds = qMin(kLoginBackoffBaseSec << shift, kLoginBackoffMaxSec);
        m_loginBackoffUntilMs = now + qint64(seconds) * 1000;
        saveTokenCache();
    } else {
        ++m_rateLimitHits;
        const int shift = qMin(m_rateLimitHits - 1, 6);
        const int seconds = qMin(kBackoffBaseSec << shift, kBackoffMaxSec);
        m_backoffUntilMs = now + qint64(seconds) * 1000;
    }

    qWarning().noquote() << "ConnectLife rate limited by" << what << "(HTTP" << httpStatus
                         << "):" << detail;
}

bool ConnectLifeApi::rateLimited(QString *reason) const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_backoffUntilMs <= now)
        return false;
    if (reason) {
        const int secs = static_cast<int>((m_backoffUntilMs - now + 999) / 1000);
        *reason = QStringLiteral("cloud API rate limited; backing off %1 s").arg(secs);
    }
    return true;
}

int ConnectLifeApi::loginRetrySeconds() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_loginBackoffUntilMs <= now)
        return 0;
    return static_cast<int>((m_loginBackoffUntilMs - now + 999) / 1000);
}

bool ConnectLifeApi::loginRateLimited(QString *reason) const
{
    const int secs = loginRetrySeconds();
    if (secs <= 0)
        return false;
    if (reason) {
        if (secs >= 120)
            *reason = QStringLiteral("Gigya login rate limited; retry in %1 min")
                          .arg((secs + 59) / 60);
        else
            *reason = QStringLiteral("Gigya login rate limited; retry in %1 s").arg(secs);
    }
    return true;
}

void ConnectLifeApi::postForm(
    const QString &url, const QVariantMap &form,
    const std::function<void(bool, const QJsonObject &, const QString &)> &cb)
{
    QUrlQuery query;
    for (auto it = form.constBegin(); it != form.constEnd(); ++it)
        query.addQueryItem(it.key(), it.value().toString());

    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("User-Agent", kUserAgent);

    QNetworkReply *reply = m_net->post(request, query.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply, cb, url]() {
        const QByteArray body = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        QString error = reply->errorString();
        noteRequestOutcome(QUrl(url).path(),
                           reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                           body);
        reply->deleteLater();
        if (error.isEmpty())
            error = tr("network error");
        cb(ok, parseObject(body), ok ? QString() : error);
    });
}

void ConnectLifeApi::postJson(
    const QString &url, const QJsonObject &body,
    const std::function<void(bool, const QJsonObject &, const QString &)> &cb)
{
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));

    QNetworkReply *reply = m_net->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, cb, url]() {
        const QByteArray data = reply->readAll();
        noteRequestOutcome(QUrl(url).path(),
                           reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                           data);
        if (reply->error() != QNetworkReply::NoError) {
            cb(false, {}, reply->errorString());
        } else {
            cb(true, parseObject(data), QString());
        }
        reply->deleteLater();
    });
}

void ConnectLifeApi::getSigned(
    const QString &path, const QVariantMap &params,
    const std::function<void(bool, const QJsonObject &, const QString &)> &cb)
{
    QVariantMap data = params;
    const QString signature = QString::fromLatin1(sign(data));
    if (signature.isEmpty()) {
        cb(false, {}, QStringLiteral("failed to sign request"));
        return;
    }

    // The signature is base64 and therefore full of '+' which the gateway
    // form-decodes back to spaces. QUrlQuery leaves '+' untouched, so build the
    // percent-encoded query by hand and hand QUrl the already-encoded bytes.
    QByteArray encoded;
    const auto append = [&encoded](const QString &key, const QString &value) {
        if (!encoded.isEmpty())
            encoded += '&';
        encoded += QUrl::toPercentEncoding(key);
        encoded += '=';
        encoded += QUrl::toPercentEncoding(value);
    };
    for (auto it = data.constBegin(); it != data.constEnd(); ++it)
        append(it.key(), valueForSign(it.value()));
    append(QStringLiteral("sign"), signature);

    QUrl url = QUrl::fromEncoded(QByteArray(kBaseUrl) + path.toUtf8() + '?' + encoded);

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", kUserAgent);
    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, cb, path]() {
        const QByteArray body = reply->readAll();
        noteRequestOutcome(path, reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                           body);
        if (reply->error() != QNetworkReply::NoError) {
            cb(false, {}, reply->errorString());
        } else {
            cb(true, parseObject(body), QString());
        }
        reply->deleteLater();
    });
}

void ConnectLifeApi::postSigned(
    const QString &path, const QVariantMap &params,
    const std::function<void(bool, const QJsonObject &, const QString &)> &cb)
{
    QVariantMap data = params;
    const QString signature = QString::fromLatin1(sign(data));
    if (signature.isEmpty()) {
        cb(false, {}, QStringLiteral("failed to sign request"));
        return;
    }

    QJsonObject body = QJsonObject::fromVariantMap(data);
    body.insert(QStringLiteral("sign"), signature);

    QNetworkRequest request(QUrl(QString::fromLatin1(kBaseUrl) + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("User-Agent", kUserAgent);
    QNetworkReply *reply =
        m_net->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, cb, path]() {
        const QByteArray data = reply->readAll();
        noteRequestOutcome(path, reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                           data);
        if (reply->error() != QNetworkReply::NoError) {
            cb(false, {}, reply->errorString());
        } else {
            cb(true, parseObject(data), QString());
        }
        reply->deleteLater();
    });
}

void ConnectLifeApi::withToken(const TokenCallback &cb)
{
    if (!m_token.isEmpty() && m_tokenExpiry > QDateTime::currentDateTimeUtc()) {
        cb(true, m_token, QString());
        return;
    }
    acquireToken(cb);
}

void ConnectLifeApi::acquireToken(const TokenCallback &cb)
{
    // Single flight: a second request arriving while a token chain is already
    // in progress just waits for its result instead of starting another one.
    // Concurrent Gigya logins are rate-limited per IP and only slow everyone
    // down (and can lock the daemon out of logging in at all).
    if (m_authInFlight) {
        m_authWaiters.append(cb);
        return;
    }
    m_authInFlight = true;
    m_authWaiters.append(cb);

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const bool canRefresh = !m_refreshToken.isEmpty()
                            && (!m_refreshExpiry.isValid() || m_refreshExpiry > now);
    if (canRefresh)
        startTokenRefresh();
    else
        startLogin();
}

void ConnectLifeApi::startTokenRefresh()
{
    // Refresh tokens are issued by the OAuth endpoint alongside the access
    // token. Using the refresh_token grant avoids Gigya's strict password
    // login limit for the ~24 min lifetime of a gateway access token.
    QVariantMap form;
    form.insert(QStringLiteral("client_id"), QString::fromLatin1(kClientId));
    form.insert(QStringLiteral("client_secret"), QString::fromLatin1(kClientSecret));
    form.insert(QStringLiteral("redirect_uri"), QString::fromLatin1(kRedirectUri));
    form.insert(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    form.insert(QStringLiteral("refresh_token"), m_refreshToken);

    postForm(QString::fromLatin1(kOauthUrl) + QStringLiteral("/oauth/token"), form,
             [this](bool ok, const QJsonObject &json, const QString &error) {
                 if (!ok) {
                     // Expired/revoked refresh token, or a transport error.
                     // Keep it: the failure may be transient, and a successful
                     // fallback login replaces it with a fresh one anyway.
                     qWarning().noquote()
                         << "ConnectLife token refresh failed, logging in again:" << error;
                     startLogin();
                     return;
                 }
                 QString detail;
                 if (!applyTokenResponse(json, &detail)) {
                     // A 200 reply without a token means the refresh token is
                     // no longer usable; retry with the full login flow.
                     qWarning().noquote()
                         << "ConnectLife token refresh returned no token, logging in again:"
                         << detail;
                     startLogin();
                     return;
                 }
                 finishAuth(true, m_token, QString());
             });
}

void ConnectLifeApi::startLogin()
{
    if (m_login.isEmpty() || m_password.isEmpty()) {
        finishAuth(false, {}, QStringLiteral("ConnectLife credentials are not configured"));
        return;
    }

    // Repeated attempts extend Gigya's IP block; report the remaining wait
    // instead of sending another password login.
    QString reason;
    if (loginRateLimited(&reason)) {
        qInfo().noquote() << "ConnectLife login skipped:" << reason;
        finishAuth(false, {}, reason);
        return;
    }

    QVariantMap loginForm;
    loginForm.insert(QStringLiteral("loginID"), m_login);
    loginForm.insert(QStringLiteral("password"), m_password);
    loginForm.insert(QStringLiteral("APIKey"), QString::fromLatin1(kApiKey));
    loginForm.insert(QStringLiteral("gmid"), QString::fromLatin1(kGmid));

    postForm(QString::fromLatin1(kGigyaUrl) + QStringLiteral("/accounts.login"), loginForm,
             [this](bool ok, const QJsonObject &json, const QString &error) {
                 if (!ok) {
                     finishAuth(false, {}, QStringLiteral("login failed: ") + error);
                     return;
                 }
                 const QString loginToken =
                     json.value(QStringLiteral("sessionInfo"))
                         .toObject()
                         .value(QStringLiteral("cookieValue"))
                         .toString();
                 const QString uid = json.value(QStringLiteral("UID")).toString();
                 if (loginToken.isEmpty() || uid.isEmpty()) {
                     const int code = json.value(QStringLiteral("errorCode")).toInt();
                     const QString message = json.value(QStringLiteral("errorMessage")).toString();
                     QString detail = QStringLiteral("login failed");
                     if (code != 0 || !message.isEmpty())
                         detail += QStringLiteral(" (%1: %2)").arg(code).arg(message);
                     finishAuth(false, {}, detail);
                     return;
                 }

                 QVariantMap jwtForm;
                 jwtForm.insert(QStringLiteral("APIKey"), QString::fromLatin1(kApiKey));
                 jwtForm.insert(QStringLiteral("gmid"), QString::fromLatin1(kGmid));
                 jwtForm.insert(QStringLiteral("login_token"), loginToken);

                 postForm(QString::fromLatin1(kGigyaUrl) + QStringLiteral("/accounts.getJWT"),
                          jwtForm,
                          [this, uid](bool ok2, const QJsonObject &jwt, const QString &e2) {
                              if (!ok2) {
                                  finishAuth(false, {}, QStringLiteral("JWT failed: ") + e2);
                                  return;
                              }
                              QJsonObject auth;
                              auth.insert(QStringLiteral("client_id"),
                                          QString::fromLatin1(kClientId));
                              auth.insert(QStringLiteral("idToken"),
                                          jwt.value(QStringLiteral("id_token")));
                              auth.insert(QStringLiteral("response_type"),
                                          QStringLiteral("code"));
                              auth.insert(QStringLiteral("redirect_uri"),
                                          QString::fromLatin1(kRedirectUri));
                              auth.insert(QStringLiteral("thirdType"), QStringLiteral("CDC"));
                              auth.insert(QStringLiteral("thirdClientId"), uid);

                              postJson(QString::fromLatin1(kOauthUrl)
                                           + QStringLiteral("/oauth/authorize"),
                                       auth,
                                       [this](bool ok3, const QJsonObject &authResp,
                                              const QString &e3) {
                                           if (!ok3) {
                                               finishAuth(false, {},
                                                           QStringLiteral("authorize failed: ")
                                                               + e3);
                                               return;
                                           }
                                           const QString code =
                                               authResp.value(QStringLiteral("code")).toString();
                                           if (code.isEmpty()) {
                                               finishAuth(false, {},
                                                           QStringLiteral(
                                                               "authorize failed: no code"));
                                               return;
                                           }
                                           QVariantMap tokenForm;
                                           tokenForm.insert(QStringLiteral("client_id"),
                                                            QString::fromLatin1(kClientId));
                                           tokenForm.insert(QStringLiteral("code"), code);
                                           tokenForm.insert(QStringLiteral("grant_type"),
                                                            QStringLiteral("authorization_code"));
                                           tokenForm.insert(
                                               QStringLiteral("client_secret"),
                                               QString::fromLatin1(kClientSecret));
                                           tokenForm.insert(QStringLiteral("redirect_uri"),
                                                            QString::fromLatin1(kRedirectUri));

                                           postForm(QString::fromLatin1(kOauthUrl)
                                                        + QStringLiteral("/oauth/token"),
                                                    tokenForm,
                                                    [this](bool ok4,
                                                           const QJsonObject &tokenResp,
                                                           const QString &e4) {
                                                        if (!ok4) {
                                                            finishAuth(
                                                                false, {},
                                                                QStringLiteral(
                                                                    "token exchange failed: ")
                                                                    + e4);
                                                            return;
                                                        }
                                                        QString detail;
                                                        if (!applyTokenResponse(tokenResp,
                                                                                &detail)) {
                                                            finishAuth(
                                                                false, {},
                                                                QStringLiteral(
                                                                    "token exchange failed: ")
                                                                    + detail);
                                                            return;
                                                        }
                                                        finishAuth(true, m_token, QString());
                                                    });
                                       });
                          });
             });
}

void ConnectLifeApi::finishAuth(bool ok, const QString &token, const QString &error)
{
    m_authInFlight = false;
    QList<TokenCallback> waiters;
    waiters.swap(m_authWaiters);
    for (const TokenCallback &waiter : waiters)
        waiter(ok, token, error);
}

bool ConnectLifeApi::applyTokenResponse(const QJsonObject &response, QString *error)
{
    const QString token = response.value(QStringLiteral("access_token")).toString();
    if (token.isEmpty()) {
        if (error)
            *error = QStringLiteral("no access_token");
        return false;
    }

    m_token = token;
    // Honour the advertised lifetime (the gateway currently returns 1440 s)
    // so the token is refreshed before it actually expires.
    const qint64 expiresIn =
        response.value(QStringLiteral("expires_in")).toVariant().toLongLong();
    const qint64 lifetime = expiresIn > 0 ? expiresIn : kDefaultTokenLifetimeSec;
    m_tokenExpiry = QDateTime::currentDateTimeUtc().addSecs(qMax<qint64>(
        kTokenSafetyMarginSec + 60, lifetime - kTokenSafetyMarginSec));

    // Keep the refresh token; it lets later renewals skip Gigya entirely.
    const QString refresh = response.value(QStringLiteral("refresh_token")).toString();
    if (!refresh.isEmpty())
        m_refreshToken = refresh;

    // refreshTokenExpiredTime is epoch milliseconds on some servers and an ISO
    // timestamp on others. Missing/invalid means "no known expiry".
    const QJsonValue refreshExpiry =
        response.value(QStringLiteral("refreshTokenExpiredTime"));
    if (!refreshExpiry.isUndefined() && !refreshExpiry.isNull()) {
        QDateTime expiry;
        if (refreshExpiry.isDouble()) {
            expiry = QDateTime::fromMSecsSinceEpoch(
                         refreshExpiry.toVariant().toLongLong())
                         .toUTC();
        } else {
            const QString text = refreshExpiry.toString();
            bool numeric = false;
            const qint64 ms = text.toLongLong(&numeric);
            if (numeric)
                expiry = QDateTime::fromMSecsSinceEpoch(ms).toUTC();
            else
                expiry = QDateTime::fromString(text, Qt::ISODate).toUTC();
        }
        if (expiry.isValid())
            m_refreshExpiry = expiry;
    }

    // Any successful token acquisition proves the login block has lifted.
    m_loginRateLimitHits = 0;
    m_loginBackoffUntilMs = 0;

    saveTokenCache();
    return true;
}

void ConnectLifeApi::refreshToken(const TokenCallback &cb)
{
    // Do not discard the current access token before the new one is in hand.
    // If the renewal fails (for example because Gigya blocks the login), the
    // daemon keeps working with the token it already has. applyTokenResponse()
    // replaces it on success.
    //
    // The user asked explicitly, so the gateway backoff must not swallow the
    // retry. The Gigya login backoff is deliberately kept: overriding it with
    // every click would only extend the block.
    m_rateLimitHits = 0;
    m_backoffUntilMs = 0;

    acquireToken(cb);
}

void ConnectLifeApi::finishStatus(bool ok, const QJsonObject &device, const QString &error)
{
    m_statusInFlight = false;
    QList<StatusCallback> waiters;
    waiters.swap(m_statusWaiters);
    for (const StatusCallback &waiter : waiters)
        waiter(ok, device, error);
}

void ConnectLifeApi::fetchStatus(const StatusCallback &cb)
{
    // While backing off from a rate limit, fail fast instead of sending a
    // request that would only extend the block.
    QString reason;
    if (rateLimited(&reason)) {
        cb(false, {}, reason);
        return;
    }
    // Single flight: overlapping reads (daemon timer, widget refresh,
    // post-write reconcile) are coalesced onto one request.
    if (m_statusInFlight) {
        m_statusWaiters.append(cb);
        return;
    }
    m_statusInFlight = true;
    m_statusWaiters.append(cb);

    fetchStatusOnce(true);
}

void ConnectLifeApi::fetchStatusOnce(bool retryOnAuth)
{
    withToken([this, retryOnAuth](bool ok, const QString &token, const QString &error) {
        if (!ok) {
            finishStatus(false, {}, error);
            return;
        }
        QVariantMap params = commonData();
        params.insert(QStringLiteral("accessToken"), token);
        getSigned(
            QStringLiteral("/clife-svc/pu/get_device_status_list"), params,
            [this, retryOnAuth](bool ok2, const QJsonObject &json, const QString &error2) {
                if (!ok2) {
                    finishStatus(false, {}, error2);
                    return;
                }
                if (qEnvironmentVariableIsSet("HISENSE_CONNECTLIFE_DEBUG"))
                    qDebug().noquote() << "status response:"
                                       << QJsonDocument(json).toJson(QJsonDocument::Compact);
                const QJsonObject response =
                    json.value(QStringLiteral("response")).toObject();
                const QJsonArray devices =
                    response.value(QStringLiteral("deviceList")).toArray();
                const int resultCode =
                    response.value(QStringLiteral("resultCode")).toInt(0);
                const int errorCode = response.value(QStringLiteral("errorCode")).toInt(0);
                if (resultCode != 0 || errorCode != 0) {
                    if (retryOnAuth && errorCode == kGatewayInvalidAccessToken) {
                        // The cached token is no longer accepted. Drop it so
                        // withToken() uses the refresh token (or logs in) and
                        // retry this one request.
                        qWarning() << "ConnectLife gateway rejected the access token;"
                                      " refreshing and retrying";
                        m_token.clear();
                        m_tokenExpiry = QDateTime();
                        saveTokenCache();
                        QString reason;
                        if (rateLimited(&reason)) {
                            finishStatus(false, {}, reason);
                            return;
                        }
                        fetchStatusOnce(false);
                        return;
                    }
                    const QString desc = response.value(QStringLiteral("errorDesc")).toString();
                    finishStatus(false, {},
                                 QStringLiteral("gateway error (%1: %2)")
                                     .arg(errorCode != 0 ? errorCode : resultCode)
                                     .arg(desc.isEmpty() ? QStringLiteral("unknown") : desc));
                    return;
                }
                if (devices.isEmpty()) {
                    finishStatus(false, {}, QStringLiteral("no devices returned"));
                    return;
                }
                for (const QJsonValue &value : devices) {
                    const QJsonObject device = value.toObject();
                    const QString puid = device.value(QStringLiteral("puid")).toString();
                    const QString type =
                        device.value(QStringLiteral("deviceTypeCode")).toString();
                    const bool supported = type == QLatin1String("009")
                                           || type == QLatin1String("006")
                                           || type == QLatin1String("008");
                    if (!m_deviceId.isEmpty()) {
                        if (puid == m_deviceId) {
                            finishStatus(true, device, QString());
                            return;
                        }
                    } else if (supported) {
                        m_deviceId = puid;
                        finishStatus(true, device, QString());
                        return;
                    }
                }
                finishStatus(false, {}, QStringLiteral("air conditioner not found in account"));
            });
    });
}

void ConnectLifeApi::setProperties(const QVariantMap &properties, const SimpleCallback &cb)
{
    // Writes respect the backoff too: a rejected write would just extend the
    // rate-limit window and the widget rolls back anyway.
    QString reason;
    if (rateLimited(&reason)) {
        cb(false, reason);
        return;
    }
    withToken([this, properties, cb](bool ok, const QString &token, const QString &error) {
        if (!ok) {
            cb(false, error);
            return;
        }
        if (m_deviceId.isEmpty()) {
            cb(false, QStringLiteral("device id unknown; refresh first"));
            return;
        }
        QVariantMap params = commonData();
        params.insert(QStringLiteral("puid"), m_deviceId);
        params.insert(QStringLiteral("properties"), properties);
        params.insert(QStringLiteral("accessToken"), token);
        postSigned(
            QStringLiteral("/device/pu/property/set"), params,
            [cb](bool ok2, const QJsonObject &json, const QString &error2) {
                if (!ok2) {
                    cb(false, error2);
                    return;
                }
                // The gateway replies with { "response": { "resultCode": ..,
                // "errorCode": .., "errorDesc": .., "commandId": .. } }.
                const QJsonObject response = json.value(QStringLiteral("response")).toObject();
                const int code = response.value(QStringLiteral("errorCode")).toInt(0);
                if (code != 0) {
                    const QString desc = response.value(QStringLiteral("errorDesc")).toString();
                    cb(false, QStringLiteral("device rejected the update (%1: %2)")
                                  .arg(code)
                                  .arg(desc));
                    return;
                }
                cb(true, QString());
            });
    });
}
