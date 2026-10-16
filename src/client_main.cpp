// SPDX-License-Identifier: MIT
//
// hisense-ac: thin D-Bus client for the hisense-matterd daemon. Used both
// interactively and by the Plasma plasmoid (through Plasma5Support's
// "executable" data engine, which expects a one-shot command).
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace {
constexpr const char *kServiceName = "org.kde.hisenseac";
constexpr const char *kObjectPath = "/HisenseAC";
constexpr const char *kInterface = "org.kde.hisenseac.Control";

QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}
QTextStream &err()
{
    static QTextStream s(stderr);
    return s;
}

QDBusInterface *iface()
{
    static QDBusInterface i(QString::fromLatin1(kServiceName),
                            QString::fromLatin1(kObjectPath),
                            QString::fromLatin1(kInterface),
                            QDBusConnection::sessionBus());
    return &i;
}

QVariant prop(const char *name)
{
    return iface()->property(name);
}

int printStatus(bool json)
{
    if (!iface()->isValid()) {
        err() << "hisense-matterd is not running (session bus service "
              << kServiceName << " unavailable)\n";
        return 1;
    }

    const QVariant power = prop("Power");
    const QVariant mode = prop("Mode");
    const QVariant systemMode = prop("SystemMode");
    const QVariant localTemp = prop("LocalTemperature");
    const QVariant cool = prop("CoolingSetpoint");
    const QVariant heat = prop("HeatingSetpoint");
    const QVariant minS = prop("MinSetpoint");
    const QVariant maxS = prop("MaxSetpoint");
    const QVariant connected = prop("Connected");
    const QVariant lastError = prop("LastError");
    const QVariant nodeId = prop("NodeId");

    if (json) {
        QJsonObject o;
        o.insert(QStringLiteral("connected"), connected.toBool());
        o.insert(QStringLiteral("power"), power.toBool());
        o.insert(QStringLiteral("mode"), mode.toString());
        o.insert(QStringLiteral("systemMode"), static_cast<int>(systemMode.toUInt()));
        o.insert(QStringLiteral("localTemperature"), localTemp.toDouble());
        o.insert(QStringLiteral("coolingSetpoint"), cool.toDouble());
        o.insert(QStringLiteral("heatingSetpoint"), heat.toDouble());
        o.insert(QStringLiteral("minSetpoint"), minS.toDouble());
        o.insert(QStringLiteral("maxSetpoint"), maxS.toDouble());
        o.insert(QStringLiteral("nodeId"), int(nodeId.toUInt()));
        o.insert(QStringLiteral("lastError"), lastError.toString());
        o.insert(QStringLiteral("provisioning"), prop("Provisioning").toBool());
        o.insert(QStringLiteral("provisionError"), prop("ProvisionError").toString());
        o.insert(QStringLiteral("unpairing"), prop("Unpairing").toBool());
        o.insert(QStringLiteral("commissioned"), prop("Commissioned").toBool());
        out() << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << '\n';
    } else {
        out() << "connected:        " << (connected.toBool() ? "yes" : "no") << '\n';
        out() << "commissioned:     " << (prop("Commissioned").toBool() ? "yes" : "no") << '\n';
        out() << "power:            " << (power.toBool() ? "on" : "off") << '\n';
        out() << "mode:             " << mode.toString() << " (" << systemMode.toUInt() << ")\n";
        out() << "local temp:       " << localTemp.toDouble() << " C\n";
        out() << "cooling setpoint: " << cool.toDouble() << " C\n";
        out() << "heating setpoint: " << heat.toDouble() << " C\n";
        out() << "setpoint range:   " << minS.toDouble() << " .. " << maxS.toDouble() << " C\n";
        if (!lastError.toString().isEmpty())
            out() << "last error:       " << lastError.toString() << '\n';
    }
    out().flush();
    return 0;
}

int call(const char *method, const QVariantList &args)
{
    if (!iface()->isValid()) {
        err() << "hisense-matterd is not running\n";
        return 1;
    }
    QDBusReply<void> reply = iface()->callWithArgumentList(
        QDBus::BlockWithGui, QString::fromLatin1(method), args);
    if (!reply.isValid()) {
        err() << "call " << method << " failed: " << reply.error().message() << '\n';
        return 1;
    }
    out() << "ok\n";
    out().flush();
    return 0;
}

void usage()
{
    out() << "usage: hisense-ac <command>\n\n"
             "  status [--json]      print current AC state\n"
             "  on | off             turn the AC on (last mode) / off\n"
             "  mode <mode>          off|auto|cool|heat|fan|dry\n"
             "  setpoint <celsius>   set the active setpoint\n"
             "  refresh              force the daemon to re-read the device\n"
             "  provision <ssid> <wifi-password> <pairing-code>\n"
             "                       commission the AC (needs it in pairing mode)\n"
             "  unpair               remove the pairing and forget local state\n";
    out().flush();
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("hisense-ac"));

    const QStringList args = app.arguments();
    if (args.size() < 2) {
        usage();
        return 2;
    }

    const QString cmd = args.at(1);

    if (cmd == QLatin1String("status"))
        return printStatus(args.contains(QStringLiteral("--json")));
    if (cmd == QLatin1String("on"))
        return call("SetPower", {true});
    if (cmd == QLatin1String("off"))
        return call("SetPower", {false});
    if (cmd == QLatin1String("refresh"))
        return call("Refresh", {});
    if (cmd == QLatin1String("unpair"))
        return call("Unpair", {});
    if (cmd == QLatin1String("mode")) {
        if (args.size() < 3) {
            err() << "mode requires an argument\n";
            return 2;
        }
        return call("SetMode", {args.at(2)});
    }
    if (cmd == QLatin1String("setpoint")) {
        if (args.size() < 3) {
            err() << "setpoint requires an argument\n";
            return 2;
        }
        bool ok = false;
        const double value = args.at(2).toDouble(&ok);
        if (!ok) {
            err() << "invalid temperature: " << args.at(2) << '\n';
            return 2;
        }
        return call("SetSetpoint", {value});
    }
    if (cmd == QLatin1String("provision")) {
        if (args.size() < 5) {
            err() << "provision requires <ssid> <wifi-password> <pairing-code>\n";
            return 2;
        }
        return call("Provision", {args.at(2), args.at(3), args.at(4)});
    }

    usage();
    return 2;
}
