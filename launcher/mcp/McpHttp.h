// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <memory>
#include <utility>

class QTcpServer;
class QTcpSocket;

// Minimal HTTP/1.1 server for the MCP endpoint. Loopback only, one request at a time per connection, keep-alive
// for plain responses, and server-sent-event streams that end by closing the connection. It covers what MCP's
// Streamable HTTP transport needs and nothing else, so no extra Qt module is required.
namespace Mcp::Http {

constexpr qsizetype MAX_HEADER_BYTES = 16 * 1024;
constexpr qsizetype MAX_BODY_BYTES = 1024 * 1024;
constexpr int MAX_CONNECTIONS = 32;
constexpr int IDLE_TIMEOUT_MS = 60 * 1000;

using Headers = QList<std::pair<QByteArray, QByteArray>>;

struct Request {
    QByteArray method;
    QByteArray target;
    QByteArray version;
    Headers headers;  // names lower-cased, values trimmed, in arrival order
    QByteArray body;

    // First value of the header, or an empty array; `name` must be lower case
    QByteArray header(const QByteArray& name) const;
    bool hasHeader(const QByteArray& name) const;
    // Header values may repeat (Accept: a, Accept: b); this joins them like a comma list
    QByteArray joinedHeader(const QByteArray& name) const;
    QByteArray path() const;
    bool keepAlive() const;
};

class RequestParser {
   public:
    enum class State { NeedMore, Complete, Error };

    // Appends bytes and parses as far as possible. Bytes past a complete request stay buffered for the next one.
    State feed(const QByteArray& data);
    State state() const { return m_state; }

    // Hands out the completed request and starts parsing whatever is already buffered
    Request take();

    int errorStatus() const { return m_errorStatus; }
    QByteArray errorMessage() const { return m_errorMessage; }

   private:
    enum class Phase { Head, Body, ChunkSize, ChunkData, ChunkTrailer };

    State parse();
    State parseHead();
    State parseChunks();
    State fail(int status, const QByteArray& message);

    QByteArray m_buffer;
    Phase m_phase = Phase::Head;
    State m_state = State::NeedMore;
    Request m_request;
    qsizetype m_bodyLength = 0;
    qsizetype m_chunkRemaining = 0;
    int m_errorStatus = 0;
    QByteArray m_errorMessage;
};

QByteArray statusText(int status);

class Connection;

// The reply side of one request. Safe to keep after the client went away: writes then do nothing.
class Responder {
   public:
    explicit Responder(Connection* connection);

    void send(int status, const QByteArray& contentType, const QByteArray& body, const Headers& extra = {});
    void sendEmpty(int status, const Headers& extra = {});

    // Server-sent events. The connection closes when the stream ends, so streams never block keep-alive reuse.
    void beginStream(const Headers& extra = {});
    void streamEvent(const QByteArray& data);
    void endStream();

    bool isOpen() const;
    bool isStreaming() const { return m_streaming; }
    bool isFinished() const { return m_finished; }

    // Called once if the client disconnects before the response finished. Used as the cancellation signal.
    void onClientGone(std::function<void()> callback) { m_onClientGone = std::move(callback); }

   private:
    friend class Connection;
    void clientGone();

    QPointer<Connection> m_connection;
    bool m_streaming = false;
    bool m_finished = false;
    std::function<void()> m_onClientGone;
};
using ResponderPtr = std::shared_ptr<Responder>;

using Handler = std::function<void(const Request&, const ResponderPtr&)>;

class Connection : public QObject {
    Q_OBJECT

   public:
    Connection(QTcpSocket* socket, Handler handler, QObject* parent = nullptr);
    ~Connection() override;

   private:
    friend class Responder;

    void onReadyRead();
    void dispatchNext();
    void writeRaw(const QByteArray& data);
    void finishResponse(bool closeAfter);
    void abortWith(int status, const QByteArray& message);

    QTcpSocket* m_socket;
    Handler m_handler;
    RequestParser m_parser;
    QTimer m_idleTimer;
    std::weak_ptr<Responder> m_current;
    bool m_busy = false;
    bool m_keepAlive = true;
};

class Server : public QObject {
    Q_OBJECT

   public:
    explicit Server(Handler handler, QObject* parent = nullptr);
    ~Server() override;

    // Binds 127.0.0.1 (required) and ::1 (best effort) on `port`; 0 picks a free port
    bool listen(quint16 requestedPort, QString* error = nullptr);
    void close();
    bool isListening() const;
    quint16 port() const;

   private:
    void acceptFrom(QTcpServer* server);

    Handler m_handler;
    QTcpServer* m_v4;
    QTcpServer* m_v6;
    int m_connections = 0;
};

}  // namespace Mcp::Http
