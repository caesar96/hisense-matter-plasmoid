// SPDX-License-Identifier: MIT
#include "connectlifecontroller.h"

#include "connectlifeapi.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QTimer>
#include <QVariantMap>
#include <QtMath>

ConnectLifeController::ConnectLifeController(ConnectLifeApi *api, QObject *parent)
    : QObject(parent)
    , m_api(api)
{
    // Debounce outgoing writes: rapid changes (temperature arc drags, repeated
    // button taps) are merged into a single property set. Keeps us under the
    // gateway's rate limit and is friendlier to the device too.
    m_writeTimer.setSingleShot(true);
    m_writeTimer.setInterval(800);
    connect(&m_writeTimer, &QTimer::timeout, this, &ConnectLifeController::flushWrite);
}

QString ConnectLifeController::deviceId() const
{
    return m_api->deviceId();
}

int ConnectLifeController::modeToCode(const QString &mode)
{
    if (mode == QLatin1String("fan"))
        return 0;
    if (mode == QLatin1String("heat"))
        return 1;
    if (mode == QLatin1String("dry"))
        return 3;
    if (mode == QLatin1String("auto"))
        return 4;
    return 2; // cool
}

QString ConnectLifeController::codeToMode(int code)
{
    switch (code) {
    case 0:
        return QStringLiteral("fan");
    case 1:
        return QStringLiteral("heat");
    case 3:
        return QStringLiteral("dry");
    case 4:
        return QStringLiteral("auto");
    default:
        return QStringLiteral("cool");
    }
}

int ConnectLifeController::fanToCode(const QString &speed)
{
    // Devices differ in the numeric ladder they accept. The portable units
    // (type 006/008) observed use the three-speed ladder 5/7/9; some other
    // firmwares expose a five-speed 5..9 ladder. We expose four speeds
    // (auto/low/medium/high) and always write the 5/7/9 values, which are
    // valid across both ladders. "super_low"/"super_high" are kept as aliases
    // so a stale widget cannot send an out-of-range code.
    if (speed == QLatin1String("low") || speed == QLatin1String("super_low"))
        return 5;
    if (speed == QLatin1String("medium"))
        return 7;
    if (speed == QLatin1String("high") || speed == QLatin1String("super_high"))
        return 9;
    return 0; // auto
}

QString ConnectLifeController::codeToFan(int code)
{
    switch (code) {
    case 2:
    case 5:
    case 6:
        return QStringLiteral("low");
    case 3:
    case 7:
        return QStringLiteral("medium");
    case 4:
    case 8:
    case 9:
        return QStringLiteral("high");
    default:
        return QStringLiteral("auto");
    }
}

void ConnectLifeController::setError(const QString &error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    emit stateChanged();
}

void ConnectLifeController::applyStatus(const QJsonObject &device)
{
    const QJsonObject status = device.value(QStringLiteral("statusList")).toObject();
    const auto str = [&status](const char *key) {
        return status.value(QLatin1String(key)).toString();
    };

    m_unit = str("t_temp_type") == QLatin1String("1") ? QStringLiteral("1")
                                                      : QStringLiteral("0");
    const auto toCelsius = [this](double value) {
        return m_unit == QLatin1String("1") ? (value - 32.0) * 5.0 / 9.0 : value;
    };

    m_connected = device.value(QStringLiteral("offlineState")).toInt() != 0;
    m_power = str("t_power") == QLatin1String("1");

    const int workMode = str("t_work_mode").toInt();
    if (m_power) {
        m_mode = codeToMode(workMode);
        m_lastNonOffMode = workMode;
    } else {
        m_mode = QStringLiteral("off");
    }

    m_temperature = toCelsius(str("t_temp").toDouble());
    m_localTemperature = toCelsius(str("f_temp_in").toDouble());
    m_fanSpeed = codeToFan(str("t_fan_speed").toInt());

    // Different chassis expose swing differently: portable/tower units use a
    // single up/down oscillation flag, while wall splits use a direction/angle
    // pair. Dimmer (display light) is likewise optional.
    m_hasUpDown = status.contains(QStringLiteral("t_up_down"));
    m_hasSwingDirection = status.contains(QStringLiteral("t_swing_direction"))
                          && status.contains(QStringLiteral("t_swing_angle"));
    m_hasDimmer = status.contains(QStringLiteral("t_dimmer"));
    if (m_hasUpDown) {
        m_swing = str("t_up_down") == QLatin1String("1");
    } else if (m_hasSwingDirection) {
        m_swing = str("t_swing_direction").toUInt() != 0
                  || str("t_swing_angle").toUInt() != 0;
    } else {
        m_swing = false;
    }
    m_dimmer = str("t_dimmer") == QLatin1String("1");

    m_eco = str("t_eco") == QLatin1String("1");
    m_super = str("t_super") == QLatin1String("1");
    m_fanMute = str("t_fan_mute") == QLatin1String("1");
    m_sleep = str("t_sleep") == QLatin1String("1");
    m_humidity = str("f_humidity").toDouble();
    m_lastError.clear();

    emit stateChanged();
}

void ConnectLifeController::Refresh()
{
    m_api->fetchStatus(
        [this](bool ok, const QJsonObject &device, const QString &error) {
            if (ok) {
                applyStatus(device);
            } else {
                m_connected = false;
                setError(error);
            }
        });
}

void ConnectLifeController::RefreshLogin()
{
    if (m_refreshingLogin)
        return;

    m_refreshingLogin = true;
    m_lastError.clear();
    emit stateChanged();

    m_api->refreshToken([this](bool ok, const QString &, const QString &error) {
        m_refreshingLogin = false;
        if (!ok) {
            m_lastError = error;
            emit stateChanged();
            return;
        }
        emit stateChanged();
        Refresh();
    });
}

void ConnectLifeController::write(const QVariantMap &properties)
{
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it)
        m_pendingWrite.insert(it.key(), it.value());
    m_writeTimer.start();
}

void ConnectLifeController::flushWrite()
{
    if (m_pendingWrite.isEmpty())
        return;
    QVariantMap props;
    props.swap(m_pendingWrite);
    m_api->setProperties(props, [this](bool ok, const QString &error) {
        if (!ok)
            setError(error);
        scheduleRefresh();
    });
}

void ConnectLifeController::scheduleRefresh(int ms)
{
    QTimer::singleShot(ms, this, [this]() { Refresh(); });
}

void ConnectLifeController::SetPower(bool on)
{
    if (m_power == on)
        return;
    m_power = on;
    if (on) {
        m_mode = codeToMode(m_lastNonOffMode);
    } else {
        m_mode = QStringLiteral("off");
    }
    emit stateChanged();

    QVariantMap props;
    props.insert(QStringLiteral("t_power"), on ? 1 : 0);
    write(props);
}

void ConnectLifeController::SetMode(const QString &mode)
{
    const QString normalized = mode.toLower();
    if (normalized == QLatin1String("off")) {
        SetPower(false);
        return;
    }

    const int code = modeToCode(normalized);
    m_power = true;
    m_mode = codeToMode(code);
    m_lastNonOffMode = code;
    emit stateChanged();

    QVariantMap props;
    props.insert(QStringLiteral("t_power"), 1);
    props.insert(QStringLiteral("t_work_mode"), code);
    write(props);
}

void ConnectLifeController::SetTemperature(double celsius)
{
    m_temperature = celsius;
    emit stateChanged();

    const double value =
        m_unit == QLatin1String("1") ? celsius * 9.0 / 5.0 + 32.0 : celsius;

    QVariantMap props;
    props.insert(QStringLiteral("t_temp"), qRound(value));
    props.insert(QStringLiteral("t_temp_type"), m_unit.toInt());
    write(props);
}

void ConnectLifeController::SetFanSpeed(const QString &speed)
{
    const QString normalized = speed.toLower();
    m_fanSpeed = codeToFan(fanToCode(normalized));
    emit stateChanged();

    QVariantMap props;
    props.insert(QStringLiteral("t_fan_speed"), fanToCode(normalized));
    write(props);
}

void ConnectLifeController::SetSwing(bool on)
{
    m_swing = on;
    emit stateChanged();

    QVariantMap props;
    if (m_hasUpDown || !m_hasSwingDirection) {
        // Portable/tower chassis: a single oscillation flag.
        props.insert(QStringLiteral("t_up_down"), on ? 1 : 0);
    } else {
        // Wall splits: direction 3 is the full-range swing position.
        props.insert(QStringLiteral("t_swing_direction"), on ? 3 : 0);
        props.insert(QStringLiteral("t_swing_angle"), 0);
    }
    write(props);
}

void ConnectLifeController::SetDimmer(bool on)
{
    m_dimmer = on;
    emit stateChanged();

    QVariantMap props;
    props.insert(QStringLiteral("t_dimmer"), on ? 1 : 0);
    write(props);
}

void ConnectLifeController::SetEco(bool on)
{
    m_eco = on;
    emit stateChanged();
    QVariantMap props;
    props.insert(QStringLiteral("t_eco"), on ? 1 : 0);
    write(props);
}

void ConnectLifeController::SetSuper(bool on)
{
    m_super = on;
    emit stateChanged();
    QVariantMap props;
    props.insert(QStringLiteral("t_super"), on ? 1 : 0);
    write(props);
}

void ConnectLifeController::SetFanMute(bool on)
{
    m_fanMute = on;
    emit stateChanged();
    QVariantMap props;
    props.insert(QStringLiteral("t_fan_mute"), on ? 1 : 0);
    write(props);
}

void ConnectLifeController::SetSleep(bool on)
{
    m_sleep = on;
    emit stateChanged();
    QVariantMap props;
    props.insert(QStringLiteral("t_sleep"), on ? 1 : 0);
    write(props);
}
