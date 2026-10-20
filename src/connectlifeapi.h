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
// this class defends itself on four fronts:
//  - status reads are single-flight: overlapping fetchStatus() calls are
//    coalesced and share one request,
//  - token renewals are single-flight and normally use the OAuth refresh
//    token; Gigya password logins run only when no usable refresh token
//    exists (Gigya rate-limits repeated logins per IP),
//  - gateway rate limits (HTTP 429 or a rate-limit message in the body)
//    trigger an exponential backoff during which requests fail fast instead
//    of being sent, so the block window can actually expire,
//  - Gigya login rate limits get a separate, much longer backoff that is
//    persisted across restarts and respected by the manual retry action, so
//    repeated clicks cannot extend the block.
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

    // Obtains a new access token without discarding the current one until the
    // renewal succeeds. The OAuth refresh token is used when available, so this
    // normally does not hit Gigya's password-login endpoint at all; otherwise a
    // full login runs. The login rate-limit backoff is respected (unlike the
    // gateway backoff, which is cleared so the retry is not swallowed).
    void refreshToken(const TokenCallback &cb);

    // Seconds until a Gigya login is allowed again (0 = not blocked).
    int loginRetrySeconds() const;

private:
    void withToken(const TokenCallback &cb);
    // Single-flight entry point: waits for the refresh/login chain already in
    // progress or starts one (refresh token first, full login as fallback).
    void acquireToken(const TokenCallback &cb);
    void startTokenRefresh();
    void startLogin();
    // Delivers a pending auth result to every waiter collected by
    // acquireToken().
    void finishAuth(bool ok, const QString &token, const QString &error);
    // Stores a token response and its metadata, returns false when the reply
    // carries no access token.
    bool applyTokenResponse(const QJsonObject &response, QString *error);
    // One status round-trip. `retryOnAuth` allows a single retry with a fresh
    // token when the gateway answers 100026 (invalid access token).
    void fetchStatusOnce(bool retryOnAuth);
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
    // path, used to separate Gigya/OAuth logins from gateway traffic.
    static bool looksRateLimited(int httpStatus, const QByteArray &body);
    static bool isAuthRequest(const QString &path);
    void noteRequestOutcome(const QString &what, int httpStatus, const QByteArray &body);
    bool rateLimited(QString *reason) const;
    bool loginRateLimited(QString *reason) const;

    void loadTokenCache();
    void saveTokenCache() const;
    static QString tokenCachePath();

    QNetworkAccessManager *m_net;
    QString m_login;
    QString m_password;
    QString m_deviceId;
    QString m_token;
    QString m_refreshToken;
    QDateTime m_tokenExpiry;
    QDateTime m_refreshExpiry;
    // Gateway throttling (status/write endpoints).
    int m_rateLimitHits = 0;
    qint64 m_backoffUntilMs = 0;
    // Gigya password-login throttling; kept separate because a login block can
    // last much longer and must not be reset by the widget's retry button.
    int m_loginRateLimitHits = 0;
    qint64 m_loginBackoffUntilMs = 0;
    bool m_authInFlight = false;
    bool m_statusInFlight = false;
    QList<TokenCallback> m_authWaiters;
    QList<StatusCallback> m_statusWaiters;
};
