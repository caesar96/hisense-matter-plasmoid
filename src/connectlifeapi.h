// SPDX-License-Identifier: MIT
#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

class QNetworkAccessManager;

// ConnectLifeApi talks to the (reverse engineered) ConnectLife cloud gateway
// that the official mobile app uses. It performs the Gigya -> OAuth token
// exchange, signs every request with the app's RSA public key and exposes the
// two calls we need: read the device status and write device properties.
//
// The gateway throttles aggressive clients ("Api rate limit exceeded"), so
// this class defends itself on three fronts:
//  - status reads are single-flight: overlapping fetchStatus() calls are
//    coalesced and share one request,
//  - logins are single-flight: overlapping token requests wait for the one
//    Gigya login chain already in progress instead of starting another
//    (Gigya rate-limits repeated logins per IP),
//  - a rate-limited reply (HTTP 429 or a rate-limit message in the body)
//    triggers an exponential backoff during which requests fail fast instead
//    of being sent, so the block window can actually expire.
//
// All calls are asynchronous; results are delivered through callbacks. This
// class is deliberately UI/DBus agnostic.
class ConnectLifeApi : public QObject
{
    Q_OBJECT

public:
    // ok == false carries a human readable error.
    using StatusCallback =
        std::function<void(bool ok, const QJsonObject &device, const QString &error)>;
    using SimpleCallback = std::function<void(bool ok, const QString &error)>;
    using TokenCallback =
        std::function<void(bool ok, const QString &token, const QString &error)>;

    ConnectLifeApi(const QString &login, const QString &password,
                   QObject *parent = nullptr);

    // Optional. When empty the first supported AC in the account is used.
    void setDeviceId(const QString &puid);
    QString deviceId() const { return m_deviceId; }

    // Reads the status of every device and returns ours.
    void fetchStatus(const StatusCallback &cb);

    // Writes a subset of the AC properties (t_power, t_work_mode, t_temp, ...).
    void setProperties(const QVariantMap &properties, const SimpleCallback &cb);

    // Discards the cached access token and logs in again. The gateway sometimes
    // answers with a rate-limit error when the cached token is no longer
    // accepted; a fresh login recovers immediately, so this also clears the
    // defensive backoff and lets the UI offer a manual "log in again" action.
    void refreshToken(const TokenCallback &cb);

private:
    void withToken(const TokenCallback &cb);
    void login(const TokenCallback &cb);
    // Delivers a pending login result to every waiter collected by login().
    void finishLogin(bool ok, const QString &token, const QString &error);
    // Delivers a pending status result to every waiter collected by
    // fetchStatus().
    void finishStatus(bool ok, const QJsonObject &device, const QString &error);
    void postForm(const QString &url, const QVariantMap &form,
                  const std::function<void(bool, const QJsonObject &, const QString &)> &cb);
    void postJson(const QString &url, const QJsonObject &body,
                  const std::function<void(bool, const QJsonObject &, const QString &)> &cb);
    void getSigned(const QString &path, const QVariantMap &params,
                   const std::function<void(bool, const QJsonObject &, const QString &)> &cb);
    void postSigned(const QString &path, const QVariantMap &params,
                    const std::function<void(bool, const QJsonObject &, const QString &)> &cb);

    QByteArray sign(const QVariantMap &data) const;
    static QVariantMap commonData();
    static QString valueForSign(const QVariant &value);

    // Rate-limit bookkeeping (see the class comment). `what` is the endpoint
    // path, used for logging which service throttled us.
    static bool looksRateLimited(int httpStatus, const QByteArray &body);
    void noteRequestOutcome(const QString &what, int httpStatus, const QByteArray &body);
    bool rateLimited(QString *reason) const;

    void loadTokenCache();
    void saveTokenCache() const;
    void clearTokenCache() const;
    static QString tokenCachePath();

    QNetworkAccessManager *m_net;
    QString m_login;
    QString m_password;
    QString m_deviceId;
    QString m_token;
    QDateTime m_tokenExpiry;
    int m_rateLimitHits = 0;
    qint64 m_backoffUntilMs = 0;
    bool m_loginInFlight = false;
    bool m_statusInFlight = false;
    QList<TokenCallback> m_loginWaiters;
    QList<StatusCallback> m_statusWaiters;
};
