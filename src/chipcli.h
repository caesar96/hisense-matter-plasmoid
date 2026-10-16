// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QRegularExpression>
#include <QString>
#include <QTimer>

#include <functional>

// ChipCli owns a single long-lived `chip-tool interactive start` process and
// funnels read/write commands to it, parsing the verbose [TOO] log lines back
// into plain values. Commands are serialized: only one is in flight at a time,
// which is enough because chip-tool handles a single command at a time anyway.
class ChipCli : public QObject
{
    Q_OBJECT
public:
    using Callback = std::function<void(bool ok, const QString &value)>;

    explicit ChipCli(QObject *parent = nullptr);

    void setChipToolPath(const QString &path) { m_chipTool = path; }
    void setStorageDirectory(const QString &dir) { m_storageDir = dir; }
    void setNodeId(quint64 id) { m_nodeId = id; }
    void setEndpoint(quint16 endpoint) { m_endpoint = endpoint; }

    void start();
    void stop();
    bool isRunning() const;

    // Reads `cluster read <attrName>` (e.g. thermostat read local-temperature)
    // and resolves the callback with the integer value rendered by chip-tool.
    void readAttribute(const QString &cluster, const QString &attrName,
                       const QString &attrLabel, Callback cb);

    // Writes `cluster write <attrName> <value>` and resolves ok=true when the
    // device answers SUCCESS.
    void writeAttribute(const QString &cluster, const QString &attrName,
                        const QString &value, Callback cb);

    // Runs a one-shot `chip-tool pairing code-wifi ...`. The interactive
    // process is taken offline first (so the two never contend for the store)
    // and restarted before the callback fires. cb receives a human-readable
    // message (chip-tool output or an error).
    void runPairing(const QString &ssid, const QString &password,
                    const QString &code, quint64 nodeId, Callback cb);

    // Removes the fabric: runs `chip-tool pairing unpair` and, only if the
    // device confirms, wipes the local store; then restarts the interactive
    // process. cb receives chip-tool's output (or an error).
    void runUnpair(quint64 nodeId, Callback cb);

    // Our own marker for "a device has been commissioned". chip-tool always
    // creates its own default controller fabric in the store, so the presence
    // of fabric keys is NOT a reliable signal; we track it explicitly.
    bool commissioned() const;
    void setCommissioned(bool on);

private slots:
    void onReadyReadStdout();
    void onReadyReadStderr();
    void onErrorOccurred(QProcess::ProcessError error);
    void onTimeout();

private:
    struct Job
    {
        QString command;
        QRegularExpression matcher;
        Callback cb;
        int timeoutMs = 12000;
    };

    QString markerPath() const;
    void enqueue(Job job);
    void sendNext();
    void failCurrent(const QString &reason);
    void processChunk(const QByteArray &chunk);
    void processLine(const QString &rawLine);
    static QString stripAnsi(const QString &s);

    QProcess m_proc;
    QProcess *m_pairProc = nullptr;
    QTimer m_timeout;
    QQueue<Job> m_queue;
    Job m_current;
    bool m_busy = false;
    QString m_buffer;

    QString m_chipTool = QStringLiteral("chip-tool");
    QString m_storageDir;
    quint64 m_nodeId = 1;
    quint16 m_endpoint = 1;
};
