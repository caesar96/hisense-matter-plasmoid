// SPDX-License-Identifier: MIT
//
// hisense-matterd: a small session-bus daemon that keeps a single warm
// `chip-tool interactive` process alive and exposes the AC's thermostat state
// over D-Bus. The Plasma plasmoid talks to it through the `hisense-ac` client.
#include "chipcli.h"
#include "controller.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDebug>
#include <QDir>
#include <QMetaProperty>
#include <QStandardPaths>
#include <QTimer>
#include <QVariantMap>

namespace {
constexpr const char *kServiceName = "org.kde.hisenseac";
constexpr const char *kObjectPath = "/HisenseAC";
constexpr int kRefreshIntervalMs = 30000;
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("hisense-matterd"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("D-Bus daemon controlling a Hisense Matter air conditioner."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QString defaultStore =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/chip-tool");

    QCommandLineOption storeOpt(
        QStringList{QStringLiteral("s"), QStringLiteral("storage-directory")},
        QStringLiteral("Directory holding chip-tool's commissioned fabric state."),
        QStringLiteral("path"), defaultStore);
    QCommandLineOption nodeOpt(
        QStringList{QStringLiteral("n"), QStringLiteral("node-id")},
        QStringLiteral("Matter node id of the AC."), QStringLiteral("id"),
        QStringLiteral("1"));
    QCommandLineOption chipToolOpt(
        QStringList{QStringLiteral("c"), QStringLiteral("chip-tool")},
        QStringLiteral("Path to the chip-tool binary."), QStringLiteral("path"),
        QStringLiteral("chip-tool"));

    parser.addOption(storeOpt);
    parser.addOption(nodeOpt);
    parser.addOption(chipToolOpt);
    parser.process(app);

    bool nodeOk = false;
    const quint64 nodeId = parser.value(nodeOpt).toULongLong(&nodeOk);
    if (!nodeOk || nodeId == 0) {
        qCritical() << "invalid --node-id" << parser.value(nodeOpt);
        return 2;
    }

    const QString store = parser.value(storeOpt);
    if (!QDir().mkpath(store)) {
        qWarning() << "could not create storage directory" << store;
    }

    ChipCli cli;
    cli.setChipToolPath(parser.value(chipToolOpt));
    cli.setStorageDirectory(store);
    cli.setNodeId(nodeId);
    cli.start();

    Controller controller(&cli, nodeId);

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

    const auto options = QDBusConnection::ExportAllSlots
                         | QDBusConnection::ExportAllProperties
                         | QDBusConnection::ExportAllSignals;
    if (!bus.registerObject(QLatin1String(kObjectPath), &controller, options)) {
        qCritical() << "cannot register object" << kObjectPath << ":"
                    << bus.lastError().message();
        return 5;
    }

    // QtDBus exports the org.freedesktop.DBus.Properties interface but never
    // emits PropertiesChanged for a plain (non-adaptor) QObject. Publish it
    // ourselves whenever the controller signals a state change so generic
    // clients (e.g. the QML org.kde.plasma.workspace.dbus "Properties" type)
    // can update without polling.
    QObject::connect(&controller, &Controller::stateChanged, &app, [&bus, &controller]() {
        QVariantMap changed;
        const QMetaObject *meta = controller.metaObject();
        const QByteArray notify = QByteArrayLiteral("stateChanged");
        for (int i = 0; i < meta->propertyCount(); ++i) {
            const QMetaProperty property = meta->property(i);
            if (property.hasNotifySignal()
                && property.notifySignal().name() == notify) {
                changed.insert(QString::fromUtf8(property.name()),
                               property.read(&controller));
            }
        }
        if (changed.isEmpty())
            return;
        QDBusMessage signal = QDBusMessage::createSignal(
            QLatin1String(kObjectPath),
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"));
        signal << QStringLiteral("org.kde.hisenseac.Control") << changed
               << QStringList();
        bus.send(signal);
    });

    QTimer refreshTimer;
    refreshTimer.setInterval(kRefreshIntervalMs);
    QObject::connect(&refreshTimer, &QTimer::timeout, &controller, &Controller::Refresh);
    refreshTimer.start();

    controller.Refresh();

    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&cli]() { cli.stop(); });

    qInfo() << "hisense-matterd ready: node" << nodeId << "store" << store;
    return app.exec();
}
