// SPDX-License-Identifier: MIT
//
// hisense-connectlife: a session-bus daemon that talks to the ConnectLife
// cloud the way the official phone app does, so the plasmoid can control the
// full feature set of the AC (all modes, fan speed, swing, eco, ...) instead
// of the small subset Matter exposes.
#include "connectlifeapi.h"
#include "connectlifecontroller.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QMetaProperty>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVariantMap>

namespace {
constexpr const char *kServiceName = "org.kde.hisenseac.connectlife";
constexpr const char *kObjectPath = "/HisenseAC";
constexpr const char *kInterface = "org.kde.hisenseac.ConnectLife";
// The ConnectLife gateway rate-limits aggressive clients ("Api rate limit
// exceeded"); the phone app only reads on demand, so 5 min is a polite default
// that still keeps the widget fresh. Tune with --interval.
constexpr int kRefreshIntervalMs = 5 * 60 * 1000;

QString defaultConfigPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return dir + QStringLiteral("/hisenseac/connectlife.conf");
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("hisense-connectlife"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Controls a Hisense air conditioner through the ConnectLife cloud."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption configOption(
        {QStringLiteral("c"), QStringLiteral("config")},
        QStringLiteral("Path to the credentials file."), QStringLiteral("path"),
        defaultConfigPath());
    const QCommandLineOption loginOption(
        QStringLiteral("login"), QStringLiteral("ConnectLife account e-mail."),
        QStringLiteral("email"));
    const QCommandLineOption passwordOption(
        QStringLiteral("password"), QStringLiteral("ConnectLife account password."),
        QStringLiteral("password"));
    const QCommandLineOption deviceOption(
        QStringLiteral("device"),
        QStringLiteral("Device puid to control (default: the only AC in the account)."),
        QStringLiteral("puid"));
    const QCommandLineOption intervalOption(
        {QStringLiteral("i"), QStringLiteral("interval")},
        QStringLiteral("Status poll interval in seconds."), QStringLiteral("seconds"),
        QString::number(kRefreshIntervalMs / 1000));

    parser.addOption(configOption);
    parser.addOption(loginOption);
    parser.addOption(passwordOption);
    parser.addOption(deviceOption);
    parser.addOption(intervalOption);
    parser.process(app);

    QSettings config(parser.value(configOption), QSettings::IniFormat);
    config.beginGroup(QStringLiteral("connectlife"));

    const QString login = parser.isSet(loginOption)
                              ? parser.value(loginOption)
                              : config.value(QStringLiteral("login")).toString();
    const QString password = parser.isSet(passwordOption)
                                 ? parser.value(passwordOption)
                                 : config.value(QStringLiteral("password")).toString();
    const QString device = parser.isSet(deviceOption)
                               ? parser.value(deviceOption)
                               : config.value(QStringLiteral("deviceId")).toString();

    const QFileInfo configInfo(parser.value(configOption));
    if (configInfo.exists() && configInfo.permission(QFile::ReadGroup)) {
        qWarning() << "credentials file" << configInfo.absoluteFilePath()
                   << "is group readable; run `chmod 600` on it";
    }

    if (login.isEmpty() || password.isEmpty()) {
        qCritical() << "no ConnectLife credentials; edit" << parser.value(configOption)
                    << "and fill in the [connectlife] section";
        return 2;
    }

    bool intervalOk = false;
    const int seconds = parser.value(intervalOption).toInt(&intervalOk);
    const int intervalMs = (intervalOk && seconds > 0 ? seconds : kRefreshIntervalMs / 1000)
                           * 1000;

    ConnectLifeApi api(login, password);
    if (!device.isEmpty())
        api.setDeviceId(device);

    ConnectLifeController controller(&api);

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical() << "cannot connect to session bus:" << bus.lastError().message();
        return 3;
    }

    if (!bus.registerService(QLatin1String(kServiceName))) {
        qCritical() << "cannot register service" << kServiceName << ":"
                    << bus.lastError().message();
        return 4;
    }

    const auto options = QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties
                         | QDBusConnection::ExportAllSignals;
    if (!bus.registerObject(QLatin1String(kObjectPath), &controller, options)) {
        qCritical() << "cannot register object" << kObjectPath << ":" << bus.lastError().message();
        return 5;
    }

    // Same workaround as the Matter daemon: QtDBus never emits
    // PropertiesChanged for a plain exported QObject, so we broadcast it
    // ourselves whenever the controller reports a state change.
    QObject::connect(&controller, &ConnectLifeController::stateChanged, &app, [&bus, &controller]() {
        QVariantMap changed;
        const QMetaObject *meta = controller.metaObject();
        for (int i = 0; i < meta->propertyCount(); ++i) {
            const QMetaProperty property = meta->property(i);
            if (!property.hasNotifySignal())
                continue;
            if (property.notifySignal().name() != QByteArrayLiteral("stateChanged"))
                continue;
            changed.insert(QString::fromUtf8(property.name()), property.read(&controller));
        }
        QDBusMessage signal = QDBusMessage::createSignal(
            QLatin1String(kObjectPath), QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"));
        signal << QString::fromLatin1(kInterface) << changed << QStringList();
        bus.send(signal);
    });

    QTimer refreshTimer;
    refreshTimer.setInterval(intervalMs);
    QObject::connect(&refreshTimer, &QTimer::timeout, &controller, &ConnectLifeController::Refresh);
    refreshTimer.start();

    controller.Refresh();

    qInfo() << "hisense-connectlife ready: polling every" << intervalMs / 1000 << "s";
    return app.exec();
}
