// SPDX-License-Identifier: MIT
#include "controller.h"

#include "chipcli.h"

#include <QDebug>
#include <QTimer>
#include <QtGlobal>

namespace {
constexpr const char *kCluster = "thermostat";

constexpr double kRawToCelsius = 0.01;
} // namespace

Controller::Controller(ChipCli *cli, quint64 nodeId, QObject *parent)
    : QObject(parent)
    , m_cli(cli)
    , m_nodeId(nodeId)
{
}

int Controller::modeToSystem(const QString &mode)
{
    const QString m = mode.trimmed().toLower();
    if (m == QLatin1String("off"))
        return 0;
    if (m == QLatin1String("auto"))
        return 1;
    if (m == QLatin1String("cool"))
        return 3;
    if (m == QLatin1String("heat"))
        return 4;
    if (m == QLatin1String("fan"))
        return 7;
    if (m == QLatin1String("dry"))
        return 8;
    return -1;
}

QString Controller::systemToMode(int systemMode)
{
    switch (systemMode) {
    case 0:
        return QStringLiteral("off");
    case 1:
        return QStringLiteral("auto");
    case 3:
        return QStringLiteral("cool");
    case 4:
        return QStringLiteral("heat");
    case 7:
        return QStringLiteral("fan");
    case 8:
        return QStringLiteral("dry");
    default:
        return QStringLiteral("unknown");
    }
}

void Controller::setError(const QString &error)
{
    m_lastError = error;
}

void Controller::Refresh()
{
    readAll();
}

void Controller::readAll()
{
    m_pendingReads = 6;

    readOne(QStringLiteral("system-mode"), QStringLiteral("SystemMode"), nullptr);
    readOne(QStringLiteral("local-temperature"), QStringLiteral("LocalTemperature"),
            &m_localTemperature);
    readOne(QStringLiteral("occupied-cooling-setpoint"),
            QStringLiteral("OccupiedCoolingSetpoint"), &m_coolingSetpoint);
    readOne(QStringLiteral("occupied-heating-setpoint"),
            QStringLiteral("OccupiedHeatingSetpoint"), &m_heatingSetpoint);
    readOne(QStringLiteral("min-cool-setpoint-limit"),
            QStringLiteral("MinCoolSetpointLimit"), &m_minSetpoint);
    readOne(QStringLiteral("max-cool-setpoint-limit"),
            QStringLiteral("MaxCoolSetpointLimit"), &m_maxSetpoint);
}

void Controller::readOne(const QString &attrName, const QString &attrLabel,
                         double *target)
{
    // system-mode is stored as an integer, handled specially.
    const bool isMode = (attrName == QLatin1String("system-mode"));

    m_cli->readAttribute(QLatin1String(kCluster), attrName, attrLabel,
                         [this, target, isMode](bool ok, const QString &value) {
                             if (ok) {
                                 if (isMode) {
                                     m_systemMode = value.toInt();
                                     if (m_systemMode != 0)
                                         m_lastNonOffMode = m_systemMode;
                                     m_connected = true;
                                     setError(QString());
                                 } else if (target) {
                                     *target = value.toDouble() * kRawToCelsius;
                                 }
                             } else if (m_lastError.isEmpty()) {
                                 setError(value);
                                 m_connected = false;
                             }
                             finishRead();
                         });
}

void Controller::finishRead()
{
    if (--m_pendingReads <= 0) {
        m_pendingReads = 0;
        emit stateChanged();
    }
}

void Controller::SetPower(bool on)
{
    if (on) {
        const int target = m_lastNonOffMode != 0 ? m_lastNonOffMode : 3;
        writeMode(target);
    } else {
        writeMode(0);
    }
}

void Controller::SetMode(const QString &mode)
{
    const int systemMode = modeToSystem(mode);
    if (systemMode < 0) {
        setError(QStringLiteral("unknown mode: ") + mode);
        emit stateChanged();
        return;
    }
    writeMode(systemMode);
}

void Controller::SetSetpoint(double celsius)
{
    // The AC is a cooling device; drive the cooling setpoint unless we are
    // explicitly heating.
    if (m_systemMode == 4)
        writeHeating(celsius);
    else
        writeCooling(celsius);
}

void Controller::SetCoolingSetpoint(double celsius)
{
    writeCooling(celsius);
}

void Controller::SetHeatingSetpoint(double celsius)
{
    writeHeating(celsius);
}

void Controller::writeMode(int systemMode)
{
    // Known quirk: setting a setpoint while the unit is off can be reverted on
    // power-on, so power/mode changes are applied first, then a refresh reads
    // the authoritative state back.
    m_cli->writeAttribute(QLatin1String(kCluster), QStringLiteral("system-mode"),
                          QString::number(systemMode),
                          [this, systemMode](bool ok, const QString &value) {
                              if (ok) {
                                  m_systemMode = systemMode;
                                  if (systemMode != 0)
                                      m_lastNonOffMode = systemMode;
                                  m_connected = true;
                                  setError(QString());
                                  emit stateChanged();
                              } else {
                                  setError(value);
                                  m_connected = false;
                                  emit stateChanged();
                              }
                              QTimer::singleShot(300, this, &Controller::Refresh);
                          });
}

void Controller::writeCooling(double celsius)
{
    const int raw = qRound(celsius / kRawToCelsius);
    m_cli->writeAttribute(QLatin1String(kCluster),
                          QStringLiteral("occupied-cooling-setpoint"),
                          QString::number(raw),
                          [this, raw](bool ok, const QString &value) {
                              if (ok) {
                                  m_coolingSetpoint = raw * kRawToCelsius;
                                  setError(QString());
                              } else {
                                  setError(value);
                              }
                              emit stateChanged();
                              QTimer::singleShot(300, this, &Controller::Refresh);
                          });
}

void Controller::writeHeating(double celsius)
{
    const int raw = qRound(celsius / kRawToCelsius);
    m_cli->writeAttribute(QLatin1String(kCluster),
                          QStringLiteral("occupied-heating-setpoint"),
                          QString::number(raw),
                          [this, raw](bool ok, const QString &value) {
                              if (ok) {
                                  m_heatingSetpoint = raw * kRawToCelsius;
                                  setError(QString());
                              } else {
                                  setError(value);
                              }
                              emit stateChanged();
                              QTimer::singleShot(300, this, &Controller::Refresh);
                          });
}
