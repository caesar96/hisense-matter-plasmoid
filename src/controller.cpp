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
    // A prior commissioning is recorded by our own marker, so even if the AC
    // is offline we know whether we can talk to it.
    if (m_cli)
        m_commissioned = m_cli->commissioned();
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
    // The interactive chip-tool is offline during pairing/unpairing.
    if (m_provisioning || m_unpairing)
        return;
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
                                 // A successful read proves the device is
                                 // commissioned (also self-heals an adopted store).
                                 if (!m_commissioned) {
                                     m_commissioned = true;
                                     if (m_cli)
                                         m_cli->setCommissioned(true);
                                 }
                                 if (isMode) {
                                     m_systemMode = value.toInt();
                                     if (m_systemMode != 0)
                                         m_lastNonOffMode = m_systemMode;
                                     m_connected = true;
                                     setError(QString());
                                 } else if (target) {
                                     *target = value.toDouble() * kRawToCelsius;
                                 }
                             } else if (m_commissioned && m_lastError.isEmpty()) {
                                 // Only surface read errors once we know a device
                                 // is actually paired.
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
        if (m_unpairPending && !m_unpairing && m_connected) {
            // The AC is reachable again; finish the deferred unpair.
            QTimer::singleShot(0, this, &Controller::Unpair);
        }
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

void Controller::Provision(const QString &ssid, const QString &password,
                           const QString &code)
{
    if (m_provisioning || m_unpairing)
        return;
    if (ssid.isEmpty() || password.isEmpty() || code.isEmpty()) {
        m_provisionError = QStringLiteral("SSID, Wi-Fi password and pairing code are required");
        emit stateChanged();
        return;
    }

    m_provisioning = true;
    m_provisionError.clear();
    emit stateChanged();

    m_cli->runPairing(ssid, password, code, m_nodeId,
                      [this](bool ok, const QString &message) {
                          m_provisioning = false;
                          if (ok) {
                              m_commissioned = true;
                              if (m_cli)
                                  m_cli->setCommissioned(true);
                          } else {
                              const QString tail = message.section(QLatin1Char('\n'), -1).simplified();
                              m_provisionError = tail.isEmpty()
                                                     ? QStringLiteral("Pairing failed")
                                                     : QStringLiteral("Pairing failed: ") + tail;
                          }
                          emit stateChanged();
                          QTimer::singleShot(500, this, &Controller::Refresh);
                      });
}

void Controller::Unpair()
{
    if (m_provisioning || m_unpairing)
        return;
    if (!m_commissioned) {
        // Already uncommissioned; nothing to remove.
        if (m_unpairPending || !m_provisionError.isEmpty()) {
            m_unpairPending = false;
            m_provisionError.clear();
            emit stateChanged();
        }
        return;
    }

    m_unpairing = true;
    m_provisionError.clear();
    emit stateChanged();

    m_cli->runUnpair(m_nodeId, [this](bool ok, const QString &message) {
        m_unpairing = false;
        if (ok) {
            // Device dropped our fabric and the local store was wiped.
            m_unpairPending = false;
            m_commissioned = false;
            if (m_cli)
                m_cli->setCommissioned(false);
            m_connected = false;
            m_systemMode = 0;
            m_localTemperature = 0.0;
            m_coolingSetpoint = 0.0;
            m_heatingSetpoint = 0.0;
            m_lastError.clear();
            m_provisionError.clear();
        } else {
            // The AC did not answer; nothing was removed. Keep the fabric and
            // retry automatically once the device is reachable again.
            m_unpairPending = true;
            const QString tail = message.section(QLatin1Char('\n'), -1).simplified();
            m_provisionError =
                QStringLiteral("Air conditioner unreachable \u2014 will retry automatically")
                + (tail.isEmpty() ? QString() : QStringLiteral(" (") + tail + QLatin1Char(')'));
        }
        emit stateChanged();
    });
}

void Controller::writeMode(int systemMode)
{
    if (m_provisioning || m_unpairing)
        return;
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
    if (m_provisioning || m_unpairing)
        return;
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
    if (m_provisioning || m_unpairing)
        return;
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
