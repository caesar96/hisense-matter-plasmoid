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
    const QString token = json.value(QStringLiteral("accessToken")).toString();
    const QDateTime expiry = QDateTime::fromString(
        json.value(QStringLiteral("expiry")).toString(), Qt::ISODate);
    if (!token.isEmpty() && expiry.isValid() && expiry > QDateTime::currentDateTimeUtc()) {
        m_token = token;
        m_tokenExpiry = expiry;
    }
}

void ConnectLifeApi::saveTokenCache() const
{
    const QString path = tokenCachePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonObject json;
    json.insert(QStringLiteral("accessToken"), m_token);
    json.insert(QStringLiteral("expiry"), m_tokenExpiry.toString(Qt::ISODate));
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
    connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
        const QByteArray body = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        cb(ok, parseObject(body), ok ? QString() : tr("network error"));
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
    connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
        const QByteArray data = reply->readAll();
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
    connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
        const QByteArray body = reply->readAll();
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
    connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
        const QByteArray data = reply->readAll();
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
    login(cb);
}

void ConnectLifeApi::login(const TokenCallback &cb)
{
    if (m_login.isEmpty() || m_password.isEmpty()) {
        cb(false, {}, QStringLiteral("ConnectLife credentials are not configured"));
        return;
    }

    QVariantMap loginForm;
    loginForm.insert(QStringLiteral("loginID"), m_login);
    loginForm.insert(QStringLiteral("password"), m_password);
    loginForm.insert(QStringLiteral("APIKey"), QString::fromLatin1(kApiKey));
    loginForm.insert(QStringLiteral("gmid"), QString::fromLatin1(kGmid));

    postForm(QString::fromLatin1(kGigyaUrl) + QStringLiteral("/accounts.login"), loginForm,
             [this, cb](bool ok, const QJsonObject &json, const QString &error) {
                 if (!ok) {
                     cb(false, {}, QStringLiteral("login failed: ") + error);
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
                     cb(false, {}, detail);
                     return;
                 }

                 QVariantMap jwtForm;
                 jwtForm.insert(QStringLiteral("APIKey"), QString::fromLatin1(kApiKey));
                 jwtForm.insert(QStringLiteral("gmid"), QString::fromLatin1(kGmid));
                 jwtForm.insert(QStringLiteral("login_token"), loginToken);

                 postForm(QString::fromLatin1(kGigyaUrl) + QStringLiteral("/accounts.getJWT"),
                          jwtForm,
                          [this, cb, uid](bool ok2, const QJsonObject &jwt, const QString &e2) {
                              if (!ok2) {
                                  cb(false, {}, QStringLiteral("JWT failed: ") + e2);
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
                                       [this, cb, uid](bool ok3, const QJsonObject &authResp,
                                                       const QString &e3) {
                                           if (!ok3) {
                                               cb(false, {},
                                                  QStringLiteral("authorize failed: ") + e3);
                                               return;
                                           }
                                           const QString code =
                                               authResp.value(QStringLiteral("code")).toString();
                                           if (code.isEmpty()) {
                                               cb(false, {},
                                                  QStringLiteral("authorize failed: no code"));
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
                                                    [this, cb](bool ok4,
                                                               const QJsonObject &tokenResp,
                                                               const QString &e4) {
                                                        if (!ok4) {
                                                            cb(false, {},
                                                               QStringLiteral(
                                                                   "token exchange failed: ")
                                                                   + e4);
                                                            return;
                                                        }
                                                        const QString token =
                                                            tokenResp
                                                                .value(QStringLiteral(
                                                                    "access_token"))
                                                                .toString();
                                                        if (token.isEmpty()) {
                                                            cb(false, {},
                                                               QStringLiteral(
                                                                   "token exchange failed: no "
                                                                   "access_token"));
                                                            return;
                                                        }
                                                        m_token = token;
                                                        m_tokenExpiry =
                                                            QDateTime::currentDateTimeUtc()
                                                                .addSecs(12 * 3600);
                                                        saveTokenCache();
                                                        cb(true, token, QString());
                                                    });
                                       });
                          });
             });
}

void ConnectLifeApi::fetchStatus(const StatusCallback &cb)
{
    withToken([this, cb](bool ok, const QString &token, const QString &error) {
        if (!ok) {
            cb(false, {}, error);
            return;
        }
        QVariantMap params = commonData();
        params.insert(QStringLiteral("accessToken"), token);
        getSigned(
            QStringLiteral("/clife-svc/pu/get_device_status_list"), params,
            [this, cb](bool ok2, const QJsonObject &json, const QString &error2) {
                if (!ok2) {
                    cb(false, {}, error2);
                    return;
                }
                if (qEnvironmentVariableIsSet("HISENSE_CONNECTLIFE_DEBUG"))
                    qDebug().noquote() << "status response:"
                                       << QJsonDocument(json).toJson(QJsonDocument::Compact);
                const QJsonObject response =
                    json.value(QStringLiteral("response")).toObject();
                const QJsonArray devices =
                    response.value(QStringLiteral("deviceList")).toArray();
                if (devices.isEmpty()) {
                    const int code = response.value(QStringLiteral("code")).toInt();
                    const QString message =
                        response.value(QStringLiteral("message")).toString();
                    QString detail = QStringLiteral("no devices returned");
                    if (code != 0 || !message.isEmpty())
                        detail += QStringLiteral(" (%1: %2)").arg(code).arg(message);
                    cb(false, {}, detail);
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
                            cb(true, device, QString());
                            return;
                        }
                    } else if (supported) {
                        m_deviceId = puid;
                        cb(true, device, QString());
                        return;
                    }
                }
                cb(false, {}, QStringLiteral("air conditioner not found in account"));
            });
    });
}

void ConnectLifeApi::setProperties(const QVariantMap &properties, const SimpleCallback &cb)
{
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
