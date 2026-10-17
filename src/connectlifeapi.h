// SPDX-License-Identifier: MIT
#pragma once

#include <QDateTime>
#include <QJsonObject>
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

private:
    void withToken(const TokenCallback &cb);
    void login(const TokenCallback &cb);
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

    void loadTokenCache();
    void saveTokenCache() const;
    static QString tokenCachePath();

    QNetworkAccessManager *m_net;
    QString m_login;
    QString m_password;
    QString m_deviceId;
    QString m_token;
    QDateTime m_tokenExpiry;
};
