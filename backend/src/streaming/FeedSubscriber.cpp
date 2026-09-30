/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "FeedSubscriber.h"

#include <QDebug>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QThread>

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace feed = mw::native::feed;

namespace {

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

QByteArray controlBytes(const QJsonObject& message)
{
    const QByteArray json = QJsonDocument(message).toJson(QJsonDocument::Compact);
    feed::Header h;
    h.kind = feed::Kind::Control;
    h.publishedUs = steadyNowUs();
    const std::vector<uint8_t> bytes = feed::message(
        h, reinterpret_cast<const uint8_t*>(json.constData()), static_cast<size_t>(json.size()));
    return QByteArray(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<qsizetype>(bytes.size()));
}

} // namespace

// ── The pipe's own thread ───────────────────────────────────────────────────

class FeedSubscriber::Client : public QObject
{
public:
    explicit Client(FeedSubscriber* owner)
        : m_Owner(owner)
    {}

    // ── start()'s wait for `info`, under m_WaitMutex ───────────────────────
    std::mutex m_WaitMutex;
    std::condition_variable m_WaitCv;
    bool m_Settled = false;
    bool m_Ok = false;
    QJsonObject m_Info;
    QString m_Error;

    void open()
    {
        m_Socket = new QLocalSocket(this);
        QObject::connect(m_Socket, &QLocalSocket::connected, this, [this]() {
            QJsonObject hello{{QStringLiteral("type"), QStringLiteral("hello")},
                              {QStringLiteral("token"), QString::fromLatin1(m_Owner->m_Token)},
                              {QStringLiteral("slot"), m_Owner->m_Slot}};
            m_Socket->write(controlBytes(hello));
        });
        QObject::connect(m_Socket, &QLocalSocket::readyRead, this, [this]() { read(); });
        QObject::connect(m_Socket, &QLocalSocket::disconnected, this,
                         [this]() { end(QStringLiteral("the feed closed the pipe")); });
        QObject::connect(m_Socket, &QLocalSocket::errorOccurred, this,
                         [this](QLocalSocket::LocalSocketError) { end(m_Socket->errorString()); });
        m_Socket->connectToServer(m_Owner->m_Name);
    }

    void write(const QByteArray& bytes)
    {
        if (m_Socket && m_Socket->state() == QLocalSocket::ConnectedState) m_Socket->write(bytes);
    }

    void close()
    {
        if (!m_Socket) return;
        QObject::disconnect(m_Socket, nullptr, this, nullptr);
        m_Socket->abort();
        delete m_Socket;
        m_Socket = nullptr;
        m_Owner->m_Connected.store(false, std::memory_order_release);
    }

private:
    void read()
    {
        m_Inbox.append(m_Socket->readAll());
        qsizetype at = 0;
        while (!m_Ended) {
            feed::Header h;
            std::string why;
            const auto* bytes = reinterpret_cast<const uint8_t*>(m_Inbox.constData()) + at;
            const size_t left = static_cast<size_t>(m_Inbox.size() - at);
            const feed::Parse p = feed::decodeHeader(bytes, left, h, &why);
            if (p == feed::Parse::NeedMore) break;
            if (p == feed::Parse::Bad) {
                end(QStringLiteral("unreadable message from the feed: ") +
                    QString::fromStdString(why));
                return;
            }
            const size_t whole = feed::kHeaderSize + h.payloadSize;
            if (left < whole) break;
            const uint8_t* payload = bytes + feed::kHeaderSize;
            at += static_cast<qsizetype>(whole);
            if (h.kind == feed::Kind::Frame) {
                // Only once the feed said what it is: a picture before `info`
                // would be one the worker cannot yet describe to its browser.
                if (!m_GotInfo) continue;
                m_Owner->m_Frames.fetch_add(1, std::memory_order_relaxed);
                if (m_Owner->m_OnFrame) m_Owner->m_OnFrame(h, payload, h.payloadSize);
                continue;
            }
            const QJsonObject msg =
                QJsonDocument::fromJson(
                    QByteArray::fromRawData(reinterpret_cast<const char*>(payload),
                                            static_cast<qsizetype>(h.payloadSize)))
                    .object();
            if (!m_GotInfo) {
                if (msg.value(QStringLiteral("type")).toString() != QLatin1String("info")) continue;
                m_GotInfo = true;
                m_Owner->m_Connected.store(true, std::memory_order_release);
                settle(true, msg, QString());
                continue;
            }
            emit m_Owner->controlReceived(msg);
        }
        // The frames handed out above pointed into the buffer: it is trimmed
        // only once they are all delivered.
        if (at > 0) m_Inbox.remove(0, at);
    }

    void end(const QString& why)
    {
        if (m_Ended) return;
        m_Ended = true;
        m_Owner->m_Connected.store(false, std::memory_order_release);
        if (!m_GotInfo) settle(false, QJsonObject(), why);
        qInfo() << "[FeedSubscriber] subscription ended:" << why;
        emit m_Owner->disconnected(why);
    }

    void settle(bool ok, const QJsonObject& info, const QString& error)
    {
        {
            std::lock_guard<std::mutex> lock(m_WaitMutex);
            if (m_Settled) return;
            m_Settled = true;
            m_Ok = ok;
            m_Info = info;
            m_Error = error;
        }
        m_WaitCv.notify_all();
    }

    FeedSubscriber* m_Owner;
    QLocalSocket* m_Socket = nullptr;
    QByteArray m_Inbox;
    bool m_GotInfo = false;
    bool m_Ended = false;
};

// ── FeedSubscriber ──────────────────────────────────────────────────────────

FeedSubscriber::FeedSubscriber(QString name, QByteArray token, int slot, QObject* parent)
    : QObject(parent)
    , m_Name(std::move(name))
    , m_Token(std::move(token))
    , m_Slot(slot)
{}

FeedSubscriber::~FeedSubscriber()
{
    stop();
}

void FeedSubscriber::setFrameCallback(FrameCallback callback)
{
    m_OnFrame = std::move(callback);
}

bool FeedSubscriber::start(int timeoutMs, QJsonObject* info, QString* error)
{
    if (m_Thread) return m_Connected.load(std::memory_order_acquire);
    m_Thread = new QThread();
    m_Thread->setObjectName(QStringLiteral("feed-subscriber"));
    m_Client = new Client(this);
    m_Client->moveToThread(m_Thread);
    // The pictures are handed on from this thread: it is on the frame's path.
    m_Thread->start(QThread::HighestPriority);
    QMetaObject::invokeMethod(m_Client, [this]() { m_Client->open(); }, Qt::QueuedConnection);

    bool ok = false;
    QString why;
    {
        std::unique_lock<std::mutex> lock(m_Client->m_WaitMutex);
        const bool settled = m_Client->m_WaitCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                                         [this]() { return m_Client->m_Settled; });
        if (!settled) {
            why = QStringLiteral("the feed said nothing in %1 ms").arg(timeoutMs);
        } else {
            ok = m_Client->m_Ok;
            why = m_Client->m_Error;
            if (ok && info) *info = m_Client->m_Info;
        }
    }
    if (!ok) {
        qWarning() << "[FeedSubscriber] cannot join the feed" << m_Name << ":" << why;
        if (error) *error = why;
        stop();
        return false;
    }
    qInfo() << "[FeedSubscriber] joined the feed" << m_Name << "(slot" << m_Slot << ")";
    return true;
}

void FeedSubscriber::stop()
{
    if (!m_Thread) return;
    if (m_Client)
        QMetaObject::invokeMethod(
            m_Client, [this]() { m_Client->close(); }, Qt::BlockingQueuedConnection);
    m_Thread->quit();
    m_Thread->wait();
    delete m_Client;
    m_Client = nullptr;
    delete m_Thread;
    m_Thread = nullptr;
    m_Connected.store(false, std::memory_order_release);
}

void FeedSubscriber::sendControl(const QJsonObject& message)
{
    if (!m_Client) return;
    const QByteArray bytes = controlBytes(message);
    Client* client = m_Client;
    QMetaObject::invokeMethod(
        client, [client, bytes]() { client->write(bytes); }, Qt::QueuedConnection);
}
