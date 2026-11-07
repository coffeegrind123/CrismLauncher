// SPDX-License-Identifier: GPL-3.0-only
#include "McpHttp.h"

#include <QDebug>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include <algorithm>
#include <cstring>

namespace Mcp::Http {

namespace {
constexpr QByteArrayView CRLF = "\r\n";
constexpr QByteArrayView HEAD_END = "\r\n\r\n";

bool isTokenChar(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    static constexpr char s_specials[] = "!#$%&'*+-.^_`|~";
    return c != '\0' && std::strchr(s_specials, c) != nullptr;
}

bool isToken(QByteArrayView text)
{
    if (text.isEmpty()) {
        return false;
    }
    for (char c : text) {
        if (!isTokenChar(c)) {
            return false;
        }
    }
    return true;
}

// Header values: visible ASCII, space, tab, and obs-text. Control characters (CR/LF/NUL) are refused.
bool isFieldValue(QByteArrayView text)
{
    for (char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if ((u < 0x20 && u != '\t') || u == 0x7F) {
            return false;
        }
    }
    return true;
}

bool listContainsToken(const QByteArray& list, QByteArrayView token)
{
    for (const auto& part : list.split(',')) {
        if (part.trimmed().compare(token, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}
}  // namespace

QByteArray Request::header(const QByteArray& name) const
{
    for (const auto& [key, value] : headers) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

bool Request::hasHeader(const QByteArray& name) const
{
    for (const auto& [key, value] : headers) {
        if (key == name) {
            return true;
        }
    }
    return false;
}

QByteArray Request::joinedHeader(const QByteArray& name) const
{
    QByteArrayList values;
    for (const auto& [key, value] : headers) {
        if (key == name) {
            values.append(value);
        }
    }
    return values.join(", ");
}

QByteArray Request::path() const
{
    const auto query = target.indexOf('?');
    return query < 0 ? target : target.left(query);
}

bool Request::keepAlive() const
{
    const auto connection = joinedHeader("connection");
    if (version == "HTTP/1.0") {
        return listContainsToken(connection, "keep-alive");
    }
    return !listContainsToken(connection, "close");
}

RequestParser::State RequestParser::feed(const QByteArray& data)
{
    if (m_state == State::Error) {
        return m_state;
    }
    m_buffer.append(data);
    // A client that keeps sending while its previous request is handled must not grow the buffer without bound
    if (m_buffer.size() > MAX_HEADER_BYTES + MAX_BODY_BYTES + 1024) {
        return fail(413, "Too much pipelined data");
    }
    if (m_state == State::Complete) {
        return m_state;
    }
    return parse();
}

Request RequestParser::take()
{
    Request out = std::move(m_request);
    m_request = {};
    m_phase = Phase::Head;
    m_state = State::NeedMore;
    m_bodyLength = 0;
    m_chunkRemaining = 0;
    if (!m_buffer.isEmpty()) {
        parse();
    }
    return out;
}

RequestParser::State RequestParser::fail(int status, const QByteArray& message)
{
    m_state = State::Error;
    m_errorStatus = status;
    m_errorMessage = message;
    return m_state;
}

RequestParser::State RequestParser::parse()
{
    if (m_phase == Phase::Head) {
        if (const auto state = parseHead(); state != State::Complete) {
            return state;
        }
    }

    if (m_phase == Phase::Body) {
        if (m_buffer.size() < m_bodyLength) {
            return m_state = State::NeedMore;
        }
        m_request.body = m_buffer.left(m_bodyLength);
        m_buffer.remove(0, m_bodyLength);
        return m_state = State::Complete;
    }

    return parseChunks();
}

RequestParser::State RequestParser::parseHead()
{
    // Tolerate stray CRLFs between keep-alive requests (RFC 9112 section 2.2)
    while (m_buffer.startsWith(CRLF)) {
        m_buffer.remove(0, 2);
    }

    const auto end = m_buffer.indexOf(HEAD_END);
    if (end < 0) {
        if (m_buffer.size() > MAX_HEADER_BYTES) {
            return fail(431, "Request header too large");
        }
        return m_state = State::NeedMore;
    }
    if (end > MAX_HEADER_BYTES) {
        return fail(431, "Request header too large");
    }

    const auto head = m_buffer.left(end);
    m_buffer.remove(0, end + HEAD_END.size());

    // The final line lost its CR with the terminator; every other line must end in CRLF
    auto lines = head.split('\n');
    for (qsizetype i = 0; i < lines.size(); ++i) {
        if (lines[i].endsWith('\r')) {
            lines[i].chop(1);
        } else if (i != lines.size() - 1) {
            return fail(400, "Bare LF in request header");
        }
    }

    const auto requestLine = lines.takeFirst().split(' ');
    if (requestLine.size() != 3 || !isToken(requestLine[0]) || requestLine[1].isEmpty() || !requestLine[2].startsWith("HTTP/1.")) {
        return fail(400, "Malformed request line");
    }
    m_request.method = requestLine[0];
    m_request.target = requestLine[1];
    m_request.version = requestLine[2];
    if (m_request.version != "HTTP/1.1" && m_request.version != "HTTP/1.0") {
        return fail(505, "HTTP version not supported");
    }

    for (const auto& line : std::as_const(lines)) {
        if (line.startsWith(' ') || line.startsWith('\t')) {
            return fail(400, "Obsolete header line folding");
        }
        const auto colon = line.indexOf(':');
        if (colon <= 0) {
            return fail(400, "Malformed header line");
        }
        const auto name = line.left(colon);
        const auto value = line.mid(colon + 1).trimmed();
        if (!isToken(name) || !isFieldValue(value)) {
            return fail(400, "Invalid header");
        }
        m_request.headers.emplace_back(name.toLower(), value);
    }

    const bool hasLength = m_request.hasHeader("content-length");
    const auto transferEncoding = m_request.joinedHeader("transfer-encoding");
    if (!transferEncoding.isEmpty()) {
        if (hasLength) {
            return fail(400, "Both Content-Length and Transfer-Encoding");
        }
        if (transferEncoding.trimmed().compare("chunked", Qt::CaseInsensitive) != 0) {
            return fail(501, "Unsupported transfer encoding");
        }
        m_phase = Phase::ChunkSize;
        return m_state = State::Complete;
    }

    if (hasLength) {
        const auto lengths = m_request.joinedHeader("content-length").split(',');
        const auto lengthText = lengths.first().trimmed();
        for (const auto& other : lengths) {
            if (other.trimmed() != lengthText) {
                return fail(400, "Conflicting Content-Length");
            }
        }
        const bool allDigits =
            !lengthText.isEmpty() && std::all_of(lengthText.begin(), lengthText.end(), [](char c) { return c >= '0' && c <= '9'; });
        bool ok = false;
        const auto length = lengthText.toLongLong(&ok);
        if (!allDigits || !ok) {
            return fail(400, "Invalid Content-Length");
        }
        if (length > MAX_BODY_BYTES) {
            return fail(413, "Request body too large");
        }
        m_bodyLength = static_cast<qsizetype>(length);
    } else {
        m_bodyLength = 0;
    }
    m_phase = Phase::Body;
    return m_state = State::Complete;
}

RequestParser::State RequestParser::parseChunks()
{
    while (true) {
        switch (m_phase) {
            case Phase::ChunkSize: {
                const auto lineEnd = m_buffer.indexOf(CRLF);
                if (lineEnd < 0) {
                    if (m_buffer.size() > 1024) {
                        return fail(400, "Malformed chunk size");
                    }
                    return m_state = State::NeedMore;
                }
                auto sizeText = m_buffer.left(lineEnd);
                m_buffer.remove(0, lineEnd + CRLF.size());
                if (const auto ext = sizeText.indexOf(';'); ext >= 0) {
                    sizeText.truncate(ext);
                }
                sizeText = sizeText.trimmed();
                bool ok = false;
                const auto size = sizeText.toLongLong(&ok, 16);
                if (!ok || size < 0 || sizeText.isEmpty() || sizeText.size() > 8) {
                    return fail(400, "Malformed chunk size");
                }
                if (m_request.body.size() + size > MAX_BODY_BYTES) {
                    return fail(413, "Request body too large");
                }
                if (size == 0) {
                    m_phase = Phase::ChunkTrailer;
                    break;
                }
                m_chunkRemaining = static_cast<qsizetype>(size);
                m_phase = Phase::ChunkData;
                break;
            }
            case Phase::ChunkData: {
                if (m_buffer.size() < m_chunkRemaining + CRLF.size()) {
                    return m_state = State::NeedMore;
                }
                if (m_buffer.mid(m_chunkRemaining, CRLF.size()) != "\r\n") {
                    return fail(400, "Malformed chunk");
                }
                m_request.body.append(m_buffer.left(m_chunkRemaining));
                m_buffer.remove(0, m_chunkRemaining + CRLF.size());
                m_phase = Phase::ChunkSize;
                break;
            }
            case Phase::ChunkTrailer: {
                // Trailer fields are read and dropped; the body ends at the first empty line
                const auto lineEnd = m_buffer.indexOf(CRLF);
                if (lineEnd < 0) {
                    if (m_buffer.size() > MAX_HEADER_BYTES) {
                        return fail(431, "Trailer too large");
                    }
                    return m_state = State::NeedMore;
                }
                m_buffer.remove(0, lineEnd + CRLF.size());
                if (lineEnd == 0) {
                    m_phase = Phase::Body;
                    m_bodyLength = 0;
                    return m_state = State::Complete;
                }
                break;
            }
            case Phase::Head:
            case Phase::Body:
                return m_state;
        }
    }
}

QByteArray statusText(int status)
{
    switch (status) {
        case 200:
            return "OK";
        case 202:
            return "Accepted";
        case 204:
            return "No Content";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 406:
            return "Not Acceptable";
        case 408:
            return "Request Timeout";
        case 413:
            return "Content Too Large";
        case 415:
            return "Unsupported Media Type";
        case 429:
            return "Too Many Requests";
        case 431:
            return "Request Header Fields Too Large";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 503:
            return "Service Unavailable";
        case 505:
            return "HTTP Version Not Supported";
        default:
            return "Unknown";
    }
}

namespace {
QByteArray statusLine(int status)
{
    return "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusText(status) + "\r\n";
}

QByteArray headerBlock(const Headers& headers)
{
    QByteArray out;
    for (const auto& [name, value] : headers) {
        out += name + ": " + value + "\r\n";
    }
    return out;
}
}  // namespace

Responder::Responder(Connection* connection) : m_connection(connection) {}

bool Responder::isOpen() const
{
    return m_connection && !m_finished;
}

void Responder::send(int status, const QByteArray& contentType, const QByteArray& body, const Headers& extra)
{
    if (!isOpen() || m_streaming) {
        return;
    }
    const bool closeAfter = !m_connection->m_keepAlive;
    QByteArray out = statusLine(status);
    out += "Content-Type: " + contentType + "\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\n";
    out += closeAfter ? "Connection: close\r\n" : "Connection: keep-alive\r\n";
    out += headerBlock(extra);
    out += "\r\n";
    out += body;
    m_finished = true;
    m_connection->writeRaw(out);
    m_connection->finishResponse(closeAfter);
}

void Responder::sendEmpty(int status, const Headers& extra)
{
    if (!isOpen() || m_streaming) {
        return;
    }
    const bool closeAfter = !m_connection->m_keepAlive;
    QByteArray out = statusLine(status);
    out += "Content-Length: 0\r\n";
    out += closeAfter ? "Connection: close\r\n" : "Connection: keep-alive\r\n";
    out += headerBlock(extra);
    out += "\r\n";
    m_finished = true;
    m_connection->writeRaw(out);
    m_connection->finishResponse(closeAfter);
}

void Responder::beginStream(const Headers& extra)
{
    if (!isOpen() || m_streaming) {
        return;
    }
    m_streaming = true;
    QByteArray out = statusLine(200);
    out += "Content-Type: text/event-stream\r\n";
    out += "Cache-Control: no-store\r\n";
    out += "Connection: close\r\n";
    out += "X-Accel-Buffering: no\r\n";
    out += headerBlock(extra);
    out += "\r\n";
    m_connection->writeRaw(out);
}

void Responder::streamEvent(const QByteArray& data)
{
    if (!isOpen() || !m_streaming) {
        return;
    }
    // One event; a payload line break would end the field early, so each line becomes its own data field
    QByteArray out = "event: message\n";
    for (const auto& line : data.split('\n')) {
        out += "data: " + line + "\n";
    }
    out += "\n";
    m_connection->writeRaw(out);
}

void Responder::endStream()
{
    if (!isOpen() || !m_streaming) {
        return;
    }
    m_finished = true;
    m_connection->finishResponse(true);
}

void Responder::clientGone()
{
    if (m_finished) {
        return;
    }
    m_finished = true;
    if (m_onClientGone) {
        auto callback = std::move(m_onClientGone);
        m_onClientGone = nullptr;
        callback();
    }
}

Connection::Connection(QTcpSocket* socket, Handler handler, QObject* parent)
    : QObject(parent), m_socket(socket), m_handler(std::move(handler))
{
    m_socket->setParent(this);
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(IDLE_TIMEOUT_MS);
    connect(&m_idleTimer, &QTimer::timeout, this, [this] { m_socket->disconnectFromHost(); });
    connect(m_socket, &QTcpSocket::readyRead, this, &Connection::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, [this] {
        if (auto current = m_current.lock()) {
            current->clientGone();
        }
        deleteLater();
    });
    m_idleTimer.start();
}

Connection::~Connection()
{
    if (auto current = m_current.lock()) {
        current->clientGone();
    }
}

void Connection::onReadyRead()
{
    const auto data = m_socket->readAll();
    if (m_parser.feed(data) == RequestParser::State::Error && !m_busy) {
        abortWith(m_parser.errorStatus(), m_parser.errorMessage());
        return;
    }
    dispatchNext();
}

void Connection::dispatchNext()
{
    if (m_busy) {
        return;
    }
    if (m_parser.state() == RequestParser::State::Error) {
        abortWith(m_parser.errorStatus(), m_parser.errorMessage());
        return;
    }
    if (m_parser.state() != RequestParser::State::Complete) {
        return;
    }

    m_idleTimer.stop();
    m_busy = true;
    const auto request = m_parser.take();
    m_keepAlive = request.keepAlive();
    auto responder = std::make_shared<Responder>(this);
    m_current = responder;
    m_handler(request, responder);
}

void Connection::writeRaw(const QByteArray& data)
{
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->write(data);
    }
}

void Connection::finishResponse(bool closeAfter)
{
    m_current.reset();
    if (closeAfter) {
        m_socket->disconnectFromHost();
        return;
    }
    m_busy = false;
    m_idleTimer.start();
    // Requests that arrived while this one was handled are already buffered; queue so the handler's stack unwinds
    QMetaObject::invokeMethod(this, &Connection::dispatchNext, Qt::QueuedConnection);
}

void Connection::abortWith(int status, const QByteArray& message)
{
    m_keepAlive = false;
    m_busy = true;
    QByteArray body = message + "\n";
    QByteArray out = statusLine(status);
    out += "Content-Type: text/plain; charset=utf-8\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += body;
    writeRaw(out);
    m_socket->disconnectFromHost();
}

Server::Server(Handler handler, QObject* parent)
    : QObject(parent), m_handler(std::move(handler)), m_v4(new QTcpServer(this)), m_v6(new QTcpServer(this))
{
    m_v4->setMaxPendingConnections(MAX_CONNECTIONS);
    m_v6->setMaxPendingConnections(MAX_CONNECTIONS);
    connect(m_v4, &QTcpServer::newConnection, this, [this] { acceptFrom(m_v4); });
    connect(m_v6, &QTcpServer::newConnection, this, [this] { acceptFrom(m_v6); });
}

Server::~Server()
{
    close();
}

bool Server::listen(quint16 requestedPort, QString* error)
{
    close();
    if (!m_v4->listen(QHostAddress::LocalHost, requestedPort)) {
        if (error) {
            *error = m_v4->errorString();
        }
        return false;
    }
    // Same port on ::1 so "localhost" works whichever address the client resolves first
    if (!m_v6->listen(QHostAddress::LocalHostIPv6, m_v4->serverPort())) {
        qDebug() << "MCP: IPv6 loopback not available:" << m_v6->errorString();
    }
    return true;
}

void Server::close()
{
    m_v4->close();
    m_v6->close();
}

bool Server::isListening() const
{
    return m_v4->isListening();
}

quint16 Server::port() const
{
    return m_v4->serverPort();
}

void Server::acceptFrom(QTcpServer* server)
{
    while (auto* socket = server->nextPendingConnection()) {
        if (!socket->peerAddress().isLoopback() || m_connections >= MAX_CONNECTIONS) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        ++m_connections;
        auto* connection = new Connection(socket, m_handler, this);
        connect(connection, &QObject::destroyed, this, [this] { --m_connections; });
    }
}

}  // namespace Mcp::Http
