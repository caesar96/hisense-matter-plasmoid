// SPDX-License-Identifier: MIT
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <functional>

class ConnectLifeApi;

// ConnectLifeController exposes the cloud (ConnectLife app) view of the AC on
// the session bus as
//   org.kde.hisenseac.connectlife  /HisenseAC  org.kde.hisenseac.ConnectLife
//
// Unlike the Matter daemon this knows about every mode the unit supports
// (fan/dry/auto/...) plus fan speed, swing, eco and friends. Temperatures are
// always exposed in degrees Celsius; the device's own display unit is preserved
// when writing.
class ConnectLifeController : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.hisenseac.ConnectLife")

    Q_PROPERTY(bool Connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(bool Power READ power NOTIFY stateChanged)
    Q_PROPERTY(QString Mode READ mode NOTIFY stateChanged)
    Q_PROPERTY(double Temperature READ temperature NOTIFY stateChanged)
    Q_PROPERTY(double LocalTemperature READ localTemperature NOTIFY stateChanged)
    Q_PROPERTY(QString FanSpeed READ fanSpeed NOTIFY stateChanged)
    Q_PROPERTY(bool Swing READ swing NOTIFY stateChanged)
    Q_PROPERTY(bool Dimmer READ dimmer NOTIFY stateChanged)
    Q_PROPERTY(bool Eco READ eco NOTIFY stateChanged)
    Q_PROPERTY(bool Super READ superMode NOTIFY stateChanged)
    Q_PROPERTY(bool FanMute READ fanMute NOTIFY stateChanged)
    Q_PROPERTY(bool Sleep READ sleep NOTIFY stateChanged)
    Q_PROPERTY(double Humidity READ humidity NOTIFY stateChanged)
    Q_PROPERTY(QString LastError READ lastError NOTIFY stateChanged)
    Q_PROPERTY(bool RefreshingLogin READ refreshingLogin NOTIFY stateChanged)
    Q_PROPERTY(int LoginRetryIn READ loginRetryIn NOTIFY stateChanged)
    Q_PROPERTY(QString DeviceId READ deviceId CONSTANT)

public:
    explicit ConnectLifeController(ConnectLifeApi *api, QObject *parent = nullptr);

    bool connected() const { return m_connected; }
    bool power() const { return m_power; }
    QString mode() const { return m_mode; }
    double temperature() const { return m_temperature; }
    double localTemperature() const { return m_localTemperature; }
    QString fanSpeed() const { return m_fanSpeed; }
    bool swing() const { return m_swing; }
    bool dimmer() const { return m_dimmer; }
    bool eco() const { return m_eco; }
    bool superMode() const { return m_super; }
    bool fanMute() const { return m_fanMute; }
    bool sleep() const { return m_sleep; }
    double humidity() const { return m_humidity; }
    QString lastError() const { return m_lastError; }
    bool refreshingLogin() const { return m_refreshingLogin; }
    // Seconds until Gigya allows a password login again (0 = not blocked).
    // The widget renders this as a live countdown while the daemon refuses
    // to send more login attempts.
    int loginRetryIn() const;
    QString deviceId() const;

public slots:
    void Refresh();
    // Drops the cached access token and signs in again. The gateway can start
    // answering any request with a rate-limit error for a token it no longer
    // accepts; a fresh login is the way out, so the widget exposes this as a
    // manual action.
    void RefreshLogin();
    void SetPower(bool on);
    void SetMode(const QString &mode);
    void SetTemperature(double celsius);
    void SetFanSpeed(const QString &speed);
    void SetSwing(bool on);
    void SetDimmer(bool on);
    void SetEco(bool on);
    void SetSuper(bool on);
    void SetFanMute(bool on);
    void SetSleep(bool on);

signals:
    void stateChanged();

private:
    void applyStatus(const QJsonObject &device);
    void write(const QVariantMap &properties);
    // Sends everything accumulated by write() (debounced so a slider drag
    // becomes one request instead of one per step).
    void flushWrite();
    void scheduleRefresh(int ms = 2500);
    void setError(const QString &error);
    // Keeps a lightweight timer running while a Gigya login block is active so
    // the exported LoginRetryIn countdown stays fresh.
    void syncLoginRetryTimer();

    static int modeToCode(const QString &mode);
    static QString codeToMode(int code);
    static int fanToCode(const QString &speed);
    static QString codeToFan(int code);

    ConnectLifeApi *m_api;

    bool m_connected = false;
    bool m_power = false;
    QString m_mode = QStringLiteral("off");
    int m_lastNonOffMode = 2; // cool
    double m_temperature = 24.0;
    double m_localTemperature = 0.0;
    QString m_fanSpeed = QStringLiteral("auto");
    // Swing is a plain on/off oscillation on most units ("t_up_down"); some
    // split units instead expose a direction/angle pair. Whichever the device
    // reports is used.
    bool m_swing = false;
    bool m_dimmer = false;
    bool m_hasUpDown = false;
    bool m_hasSwingDirection = false;
    bool m_hasDimmer = false;
    bool m_eco = false;
    bool m_super = false;
    bool m_fanMute = false;
    bool m_sleep = false;
    double m_humidity = 0.0;
    QString m_lastError;
    bool m_refreshingLogin = false;
    QString m_unit = QStringLiteral("0"); // device display unit, 0 = celsius

    // Debounced, merged outgoing writes. Dragging the temperature arc fires a
    // change per step; sending each one immediately produced a request burst
    // that the cloud gateway rate-limits.
    QTimer m_writeTimer;
    QVariantMap m_pendingWrite;

    // Ticks only while Gigya has us blocked, so the countdown exported to the
    // widget advances without waking the daemon all the time.
    QTimer m_loginRetryTimer;
};
