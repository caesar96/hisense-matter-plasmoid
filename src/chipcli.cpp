// SPDX-License-Identifier: MIT
#include "chipcli.h"

#include <QDebug>

namespace {
// Chip-tool colours its logging; strip the escape sequences before matching.
const QRegularExpression &ansiRe()
{
    static const QRegularExpression re(QStringLiteral("\x1b\\[[0-9;?]*[ -/]*[@-~]"));
    return re;
}
} // namespace

ChipCli::ChipCli(QObject *parent)
    : QObject(parent)
{
    m_proc.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_proc, &QProcess::readyReadStandardOutput, this, &ChipCli::onReadyReadStdout);
    connect(&m_proc, &QProcess::readyReadStandardError, this, &ChipCli::onReadyReadStderr);
    connect(&m_proc, &QProcess::errorOccurred, this, &ChipCli::onErrorOccurred);

    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, &ChipCli::onTimeout);
}

QString ChipCli::stripAnsi(const QString &s)
{
    QString out = s;
    out.remove(ansiRe());
    return out;
}

bool ChipCli::isRunning() const
{
    return m_proc.state() == QProcess::Running;
}

void ChipCli::start()
{
    if (isRunning())
        return;

    QStringList args;
    args << QStringLiteral("interactive") << QStringLiteral("start");
    if (!m_storageDir.isEmpty()) {
        args << QStringLiteral("--storage-directory") << m_storageDir;
    }

    m_proc.start(m_chipTool, args);
    if (!m_proc.waitForStarted(5000)) {
        qWarning() << "failed to start chip-tool:" << m_proc.errorString();
    }
}

void ChipCli::stop()
{
    m_timeout.stop();
    m_busy = false;
    if (m_proc.state() != QProcess::NotRunning) {
        m_proc.write("exit\n");
        if (!m_proc.waitForFinished(2000))
            m_proc.kill();
    }
}

void ChipCli::readAttribute(const QString &cluster, const QString &attrName,
                            const QString &attrLabel, Callback cb)
{
    Job job;
    job.command = QStringLiteral("%1 read %2 %3 %4")
                      .arg(cluster, attrName)
                      .arg(m_endpoint)
                      .arg(m_nodeId);
    job.matcher = QRegularExpression(QRegularExpression::escape(attrLabel)
                                     + QStringLiteral(":\\s*(-?\\d+)"));
    job.cb = std::move(cb);
    enqueue(std::move(job));
}

void ChipCli::writeAttribute(const QString &cluster, const QString &attrName,
                             const QString &value, Callback cb)
{
    Job job;
    job.command = QStringLiteral("%1 write %2 %3 %4 %5")
                      .arg(cluster, attrName, value)
                      .arg(m_endpoint)
                      .arg(m_nodeId);
    job.matcher = QRegularExpression(
        QStringLiteral("status = 0x[0-9a-fA-F]+\\s*\\((SUCCESS|FAILURE[^)]*)\\)"));
    job.cb = std::move(cb);
    enqueue(std::move(job));
}

void ChipCli::enqueue(Job job)
{
    m_queue.enqueue(std::move(job));
    if (!m_busy)
        sendNext();
}

void ChipCli::sendNext()
{
    if (m_busy || m_queue.isEmpty())
        return;

    if (!isRunning()) {
        // Try to bring chip-tool back up before failing the queue.
        start();
        if (!isRunning()) {
            Job job = m_queue.dequeue();
            if (job.cb)
                job.cb(false, QStringLiteral("chip-tool is not running"));
            QMetaObject::invokeMethod(this, &ChipCli::sendNext, Qt::QueuedConnection);
            return;
        }
    }

    m_current = m_queue.dequeue();
    m_busy = true;
    m_proc.write(m_current.command.toUtf8() + '\n');
    m_proc.waitForBytesWritten(1000);
    m_timeout.start(m_current.timeoutMs);
}

void ChipCli::failCurrent(const QString &reason)
{
    if (!m_busy)
        return;
    m_busy = false;
    m_timeout.stop();
    Job job = m_current;
    m_current = Job();
    if (job.cb)
        job.cb(false, reason);
    QMetaObject::invokeMethod(this, &ChipCli::sendNext, Qt::QueuedConnection);
}

void ChipCli::onTimeout()
{
    failCurrent(QStringLiteral("timed out waiting for chip-tool response"));
}

void ChipCli::onErrorOccurred(QProcess::ProcessError error)
{
    Q_UNUSED(error);
    if (m_busy)
        failCurrent(QStringLiteral("chip-tool process error: ") + m_proc.errorString());
}

void ChipCli::onReadyReadStdout()
{
    processChunk(m_proc.readAllStandardOutput());
}

void ChipCli::onReadyReadStderr()
{
    processChunk(m_proc.readAllStandardError());
}

void ChipCli::processChunk(const QByteArray &chunk)
{
    m_buffer += QString::fromUtf8(chunk);
    int idx;
    while ((idx = m_buffer.indexOf(QLatin1Char('\n'))) >= 0) {
        QString line = m_buffer.left(idx);
        m_buffer.remove(0, idx + 1);
        processLine(line);
    }
}

void ChipCli::processLine(const QString &rawLine)
{
    if (!m_busy)
        return;

    const QString line = stripAnsi(rawLine);

    if (line.contains(QStringLiteral("Run command failure"))
        || line.contains(QStringLiteral("Unable to find")) ) {
        failCurrent(line.simplified());
        return;
    }

    const QRegularExpressionMatch m = m_current.matcher.match(line);
    if (m.hasMatch()) {
        m_busy = false;
        m_timeout.stop();
        Job job = m_current;
        m_current = Job();
        if (job.cb)
            job.cb(true, m.captured(1));
        QMetaObject::invokeMethod(this, &ChipCli::sendNext, Qt::QueuedConnection);
    }
}
