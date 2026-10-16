// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#include <QString>

class ChipCli;

// Controller turns the low level thermostat cluster attributes of the AC into
// a friendly state model and exposes it on the session bus as
//   org.kde.hisenseac  /HisenseAC  org.kde.hisenseac.Control
//
// Temperatures on the wire are hundredths of a degree Celsius (2500 == 25.00 C);
// the public interface uses degrees Celsius.
class Controller : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.hisenseac.Control")

    Q_PROPERTY(bool Connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(bool Power READ power NOTIFY stateChanged)
    Q_PROPERTY(QString Mode READ mode NOTIFY stateChanged)
    Q_PROPERTY(uint SystemMode READ systemMode NOTIFY stateChanged)
    Q_PROPERTY(double LocalTemperature READ localTemperature NOTIFY stateChanged)
    Q_PROPERTY(double CoolingSetpoint READ coolingSetpoint NOTIFY stateChanged)
    Q_PROPERTY(double HeatingSetpoint READ heatingSetpoint NOTIFY stateChanged)
    Q_PROPERTY(double MinSetpoint READ minSetpoint NOTIFY stateChanged)
    Q_PROPERTY(double MaxSetpoint READ maxSetpoint NOTIFY stateChanged)
    Q_PROPERTY(uint NodeId READ nodeId CONSTANT)
    Q_PROPERTY(QString LastError READ lastError NOTIFY stateChanged)
    Q_PROPERTY(bool Provisioning READ provisioning NOTIFY stateChanged)
    Q_PROPERTY(QString ProvisionError READ provisionError NOTIFY stateChanged)
    Q_PROPERTY(bool Unpairing READ unpairing NOTIFY stateChanged)
    // True once a fabric exists locally; independent of the AC being reachable.
    Q_PROPERTY(bool Commissioned READ commissioned NOTIFY stateChanged)

public:
    Controller(ChipCli *cli, quint64 nodeId, QObject *parent = nullptr);

    bool connected() const { return m_connected; }
    bool power() const { return m_systemMode != 0; }
    QString mode() const { return systemToMode(m_systemMode); }
    uint systemMode() const { return m_systemMode; }
    double localTemperature() const { return m_localTemperature; }
    double coolingSetpoint() const { return m_coolingSetpoint; }
    double heatingSetpoint() const { return m_heatingSetpoint; }
    double minSetpoint() const { return m_minSetpoint; }
    double maxSetpoint() const { return m_maxSetpoint; }
    uint nodeId() const { return static_cast<uint>(m_nodeId); }
    QString lastError() const { return m_lastError; }
    bool provisioning() const { return m_provisioning; }
    QString provisionError() const { return m_provisionError; }
    bool unpairing() const { return m_unpairing; }
    bool commissioned() const { return m_commissioned; }

    static int modeToSystem(const QString &mode);
    static QString systemToMode(int systemMode);

public slots:
    void Refresh();
    void SetPower(bool on);
    void SetMode(const QString &mode);
    void SetSetpoint(double celsius);
    void SetCoolingSetpoint(double celsius);
    void SetHeatingSetpoint(double celsius);
    // Commission the AC onto Wi-Fi and into the Matter fabric. Runs
    // asynchronously; watch Provisioning/LastError/Connected for the result.
    void Provision(const QString &ssid, const QString &password,
                   const QString &code);
    // Remove the fabric: best-effort unpair on the device plus a local wipe, so
    // the daemon returns to the uncommissioned state. Result in ProvisionError.
    void Unpair();

signals:
    void stateChanged();

private:
    void readAll();
    void readOne(const QString &attrName, const QString &attrLabel, double *target);
    void writeMode(int systemMode);
    void writeCooling(double celsius);
    void writeHeating(double celsius);
    void finishRead();
    void setError(const QString &error);

    ChipCli *m_cli;
    quint64 m_nodeId = 1;

    bool m_connected = false;
    int m_systemMode = 0;
    int m_lastNonOffMode = 3; // cool
    double m_localTemperature = 0.0;
    double m_coolingSetpoint = 0.0;
    double m_heatingSetpoint = 0.0;
    double m_minSetpoint = 16.0;
    double m_maxSetpoint = 32.0;
    QString m_lastError;
    bool m_provisioning = false;
    QString m_provisionError;
    bool m_unpairing = false;
    // A requested unpair could not reach the device; retry when it reappears.
    bool m_unpairPending = false;
    bool m_commissioned = false;

    int m_pendingReads = 0;
};
