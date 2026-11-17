/*
 * This file is part of the AbracaDABra project
 *
 * MIT License
 *
 * Copyright (c) 2019-2026 Petr Kopecký <xkejpi (at) gmail (dot) com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "webserver.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLoggingCategory>

#include "audiostreamer.h"
#include "config.h"
#include "dabcliapp.h"

Q_LOGGING_CATEGORY(webServer, "WebServer", QtInfoMsg)

WebServer::WebServer(DabCliApp *app, AudioStreamer *streamer, QObject *parent)
    : QObject(parent), m_app(app), m_streamer(streamer)
{
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &WebServer::onNewConnection);
    connect(m_streamer, &AudioStreamer::audioChunk, this, &WebServer::onAudioChunk, Qt::QueuedConnection);
    m_streamIdleTimer = new QTimer(this);
    m_streamIdleTimer->setSingleShot(true);
    connect(m_streamIdleTimer, &QTimer::timeout, this, &WebServer::onStreamIdleTimeout);
}

bool WebServer::listen(const QHostAddress &address, quint16 port)
{
    return m_server->listen(address, port);
}

void WebServer::onNewConnection()
{
    while (m_server->hasPendingConnections())
    {
        QTcpSocket *socket = m_server->nextPendingConnection();
        m_clients.insert(socket, ClientState());
        connect(socket, &QTcpSocket::readyRead, this, &WebServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &WebServer::onSocketDisconnected);
    }
}

void WebServer::onReadyRead()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());
    if (nullptr == socket)
    {
        return;
    }
    auto it = m_clients.find(socket);
    if (it == m_clients.end())
    {
        return;
    }
    it->buffer.append(socket->readAll());
    tryProcessRequest(socket);
}

void WebServer::onSocketDisconnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());
    if (nullptr == socket)
    {
        return;
    }
    m_streamClients.remove(socket);
    if (m_streamClients.isEmpty() && m_app->isStreamDecoupled())
    {
        m_streamIdleTimer->start(6000);
    }
    m_clients.remove(socket);
    socket->deleteLater();
}

void WebServer::tryProcessRequest(QTcpSocket *socket)
{
    auto it = m_clients.find(socket);
    if (it == m_clients.end())
    {
        return;
    }
    ClientState &state = it.value();

    int headerEnd = state.buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0)
    {
        if (state.buffer.size() > 64 * 1024)
        {
            sendError(socket, 431, "Request Header Fields Too Large", "Request too large");
        }
        return;
    }

    QByteArray headerPart = state.buffer.left(headerEnd);
    QList<QByteArray> lines = headerPart.split('\n');
    if (lines.isEmpty())
    {
        sendError(socket, 400, "Bad Request", "Malformed request");
        return;
    }

    QList<QByteArray> requestParts = lines.at(0).trimmed().split(' ');
    if (requestParts.size() < 2)
    {
        sendError(socket, 400, "Bad Request", "Malformed request line");
        return;
    }
    QString method = QString::fromLatin1(requestParts.at(0)).toUpper();
    QString path = QString::fromLatin1(requestParts.at(1));

    qint64 contentLength = 0;
    QHash<QByteArray, QByteArray> headers;
    for (int i = 1; i < lines.size(); ++i)
    {
        QByteArray line = lines.at(i).trimmed();
        int colon = line.indexOf(':');
        if (colon <= 0)
        {
            continue;
        }
        QByteArray name = line.left(colon).trimmed().toLower();
        QByteArray value = line.mid(colon + 1).trimmed();
        headers.insert(name, value);
        if ("content-length" == name)
        {
            contentLength = value.toLongLong();
        }
    }

    int bodyStart = headerEnd + 4;
    if (state.buffer.size() - bodyStart < contentLength)
    {
        // wait for the rest of the body to arrive
        return;
    }

    QByteArray body = state.buffer.mid(bodyStart, int(contentLength));
    state.buffer.clear();

    handleRequest(socket, method, path, body, headers);
}

void WebServer::handleRequest(QTcpSocket *socket, const QString &method, const QString &path, const QByteArray &body,
                               const QHash<QByteArray, QByteArray> &headers)
{
    QUrl url(path, QUrl::TolerantMode);
    QString p = url.path();

    if ("GET" == method && ("/" == p || p.isEmpty()))
    {
        sendResponse(socket, 200, "OK", "text/html; charset=utf-8", indexHtml().toUtf8());
        return;
    }

    if ("GET" == method && "/api/status" == p)
    {
        sendJson(socket, QJsonDocument(m_app->statusJson()));
        return;
    }

    if ("GET" == method && "/api/channels" == p)
    {
        QJsonObject o;
        o["channels"] = DabCliApp::channelsJson();
        sendJson(socket, QJsonDocument(o));
        return;
    }

    if ("GET" == method && "/api/stationlist" == p)
    {
        QJsonObject res;
        QJsonArray stations;
        bool scanned = false;
        QFile f(QStringLiteral("/opt/dab-radio/radio_state.json"));
        if (f.open(QIODevice::ReadOnly))
        {
            QJsonParseError perr;
            QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
            if (QJsonParseError::NoError == perr.error && doc.isObject())
            {
                QJsonObject state = doc.object();
                stations = state.value(QStringLiteral("stations")).toArray();
                scanned = state.value(QStringLiteral("stations_scanned")).toBool(false);
            }
        }
        res[QStringLiteral("stations")] = stations;
        res[QStringLiteral("stations_scanned")] = scanned;
        sendJson(socket, QJsonDocument(res));
        return;
    }

    if ("POST" == method && "/api/stationlist" == p)
    {
        QJsonParseError perr;
        QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
        if (QJsonParseError::NoError != perr.error || !doc.isObject())
        {
            sendError(socket, 400, "Bad Request", "Invalid JSON body");
            return;
        }
        QJsonObject reqObj = doc.object();
        QJsonArray stations = reqObj.value(QStringLiteral("stations")).toArray();
        bool scanned = reqObj.value(QStringLiteral("stations_scanned")).toBool(true);

        QJsonObject state;
        QFile rf(QStringLiteral("/opt/dab-radio/radio_state.json"));
        if (rf.open(QIODevice::ReadOnly))
        {
            QJsonParseError rerr;
            QJsonDocument rdoc = QJsonDocument::fromJson(rf.readAll(), &rerr);
            if (QJsonParseError::NoError == rerr.error && rdoc.isObject())
            {
                state = rdoc.object();
            }
            rf.close();
        }

        state[QStringLiteral("stations")] = stations;
        state[QStringLiteral("stations_scanned")] = scanned;

        QDir().mkpath(QStringLiteral("/opt/dab-radio"));
        QFile wf(QStringLiteral("/opt/dab-radio/radio_state.json"));
        if (!wf.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            sendError(socket, 500, "Internal Server Error", "Cannot write radio_state.json");
            return;
        }
        wf.write(QJsonDocument(state).toJson(QJsonDocument::Indented));
        wf.close();

        QJsonObject res;
        res[QStringLiteral("ok")] = true;
        res[QStringLiteral("count")] = stations.size();
        sendJson(socket, QJsonDocument(res));
        return;
    }

    if ("POST" == method && "/api/tii/update" == p)
    {
        QString err;
        if (!m_app->requestTiiDbUpdate(&err))
        {
            sendError(socket, 409, "Conflict", err);
            return;
        }
        QJsonObject res;
        res[QStringLiteral("ok")] = true;
        sendJson(socket, QJsonDocument(res));
        return;
    }

    if ("POST" == method && "/api/stream" == p)
    {
        QJsonParseError perr;
        QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
        if (QJsonParseError::NoError != perr.error || !doc.isObject())
        {
            sendError(socket, 400, "Bad Request", "Invalid JSON body");
            return;
        }
        QJsonObject o = doc.object();
        uint32_t sid = 0;
        if (o.value("sid").isString())
        {
            sid = o.value("sid").toString().toUInt(nullptr, 0);
        }
        else
        {
            sid = uint32_t(o.value("sid").toDouble());
        }
        uint8_t scids = uint8_t(o.value("scids").toInt(0));
        QString channel = o.value("channel").toString();
        bool decoupled = false;
        QString err;
        if (!m_app->requestStreamService(channel, sid, scids, &decoupled, &err))
        {
            sendError(socket, 409, "Conflict", err);
            return;
        }
        if (decoupled && m_streamClients.isEmpty())
        {
            m_streamIdleTimer->start(6000);
        }
        else if (!decoupled)
        {
            m_streamIdleTimer->stop();
        }
        QJsonObject res;
        res["ok"] = true;
        res["decoupled"] = decoupled;
        sendJson(socket, QJsonDocument(res));
        return;
    }

    if ("POST" == method && "/api/stream/stop" == p)
    {
        m_streamIdleTimer->stop();
        m_app->requestStreamStop();
        QJsonObject res;
        res["ok"] = true;
        sendJson(socket, QJsonDocument(res));
        return;
    }

    if ("GET" == method && "/api/stream/slideshow" == p)
    {
        QByteArray data;
        QString contentType;
        if (!m_app->currentStreamSlide(&data, &contentType))
        {
            sendError(socket, 404, "Not Found", "No slide available yet");
            return;
        }
        sendResponse(socket, 200, "OK", contentType.toLatin1(), data);
        return;
    }

    if ("GET" == method && "/api/epg" == p)
    {
        sendJson(socket, QJsonDocument(m_app->epgJson()));
        return;
    }

    if ("POST" == method && "/api/tune" == p)
    {
        QJsonParseError perr;
        QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
        if (QJsonParseError::NoError != perr.error || !doc.isObject())
        {
            sendError(socket, 400, "Bad Request", "Invalid JSON body");
            return;
        }
        QJsonObject o = doc.object();
        QString err;
        bool ok;
        if (o.contains("channel"))
        {
            ok = m_app->requestTuneChannel(o.value("channel").toString(), &err);
        }
        else if (o.contains("frequencyKHz"))
        {
            ok = m_app->requestTuneFrequency(uint32_t(o.value("frequencyKHz").toDouble()), &err);
        }
        else
        {
            sendError(socket, 400, "Bad Request", "Expected 'channel' or 'frequencyKHz'");
            return;
        }
        if (!ok)
        {
            sendError(socket, 409, "Conflict", err);
            return;
        }
        QJsonObject res;
        res["ok"] = true;
        sendJson(socket, QJsonDocument(res));
        return;
    }

    if ("POST" == method && "/api/service" == p)
    {
        QJsonParseError perr;
        QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
        if (QJsonParseError::NoError != perr.error || !doc.isObject())
        {
            sendError(socket, 400, "Bad Request", "Invalid JSON body");
            return;
        }
        QJsonObject o = doc.object();
        uint32_t sid = 0;
        if (o.value("sid").isString())
        {
            sid = o.value("sid").toString().toUInt(nullptr, 0);  // accepts "0x..." or decimal
        }
        else
        {
            sid = uint32_t(o.value("sid").toDouble());
        }
        uint8_t scids = uint8_t(o.value("scids").toInt(0));

        QString err;
        if (!m_app->requestService(sid, scids, &err))
        {
            sendError(socket, 409, "Conflict", err);
            return;
        }
        QJsonObject res;
        res["ok"] = true;
        sendJson(socket, QJsonDocument(res));
        return;
    }

    if ("GET" == method && "/stream/audio" == p)
    {
        const bool icyRequested = ("1" == headers.value("icy-metadata"));
        startStreamClient(socket, icyRequested);
        return;
    }

    if ("GET" == method && "/api/slideshow" == p)
    {
        QByteArray data;
        QString contentType;
        if (!m_app->currentSlide(&data, &contentType))
        {
            sendError(socket, 404, "Not Found", "No slide available yet");
            return;
        }
        sendResponse(socket, 200, "OK", contentType.toLatin1(), data);
        return;
    }

    sendError(socket, 404, "Not Found", "Unknown endpoint: " + path);
}

void WebServer::sendResponse(QTcpSocket *socket, int status, const QString &statusText, const QByteArray &contentType, const QByteArray &body,
                              bool closeAfter)
{
    QByteArray header;
    header += "HTTP/1.1 " + QByteArray::number(status) + " " + statusText.toLatin1() + "\r\n";
    header += "Content-Type: " + contentType + "\r\n";
    header += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    header += "Access-Control-Allow-Origin: *\r\n";
    header += closeAfter ? "Connection: close\r\n" : "Connection: keep-alive\r\n";
    header += "\r\n";

    socket->write(header);
    socket->write(body);
    if (closeAfter)
    {
        socket->disconnectFromHost();
    }
}

void WebServer::sendJson(QTcpSocket *socket, const QJsonDocument &doc, int status)
{
    sendResponse(socket, status, status == 200 ? "OK" : "Error", "application/json", doc.toJson(QJsonDocument::Compact));
}

void WebServer::sendError(QTcpSocket *socket, int status, const QString &statusText, const QString &message)
{
    QJsonObject o;
    o["error"] = message;
    sendJson(socket, QJsonDocument(o), status);
}

void WebServer::startStreamClient(QTcpSocket *socket, bool icyMetadataRequested)
{
    int sampleRate = 48000;
    int numChannels = 2;
    m_app->currentAudioFormat(&sampleRate, &numChannels);

    QByteArray header;
    header += "HTTP/1.1 200 OK\r\n";
#if HAVE_MP3LAME
    // Real MP3 elementary stream, so it can be played back with a standard <audio> element
    // (and by any Icecast/SHOUTcast-aware client) instead of being manually decoded in JS.
    header += "Content-Type: audio/mpeg\r\n";
#else
    // Raw interleaved 16-bit LE PCM, not wrapped in a WAV container: browsers don't reliably
    // play an <audio> WAV stream of unknown/unbounded length, so the client instead reads this
    // via fetch()+ReadableStream and plays it through the Web Audio API.
    header += "Content-Type: application/octet-stream\r\n";
    header += "X-Audio-Sample-Rate: " + QByteArray::number(sampleRate) + "\r\n";
    header += "X-Audio-Channels: " + QByteArray::number(numChannels) + "\r\n";
#endif
    header += "Cache-Control: no-cache, no-store\r\n";
    header += "Access-Control-Allow-Origin: *\r\n";
    header += "icy-name: AbracaDABra-cli\r\n";

    ClientState &state = m_clients[socket];
    state.isStreamClient = true;
    // ICY (SHOUTcast/Icecast-style) inline metadata, so external players (VLC, foobar2000, ...)
    // can show the current DLS/DL Plus title/artist, similar to an Icecast source stream.
    if (icyMetadataRequested)
    {
        state.icyMetadata = true;
        state.icyMetaInt = 16000;
        state.bytesSinceMeta = 0;
        header += "icy-metaint: " + QByteArray::number(state.icyMetaInt) + "\r\n";
    }
    header += "Connection: close\r\n";
    header += "\r\n";
    socket->write(header);

    m_streamIdleTimer->stop();
    m_streamClients.insert(socket);
    qCInfo(webServer) << "Audio stream client connected, format" << sampleRate << "Hz" << numChannels << "ch, icy metadata"
                       << icyMetadataRequested;
}

QByteArray WebServer::buildIcyMetaBlock(ClientState &state)
{
    const QString title = m_app->currentStreamTitle();
    if (title == state.lastIcyTitle)
    {
        return QByteArray(1, char(0));  // unchanged since last interval -> zero-length block
    }
    state.lastIcyTitle = title;

    QString escaped = title;
    escaped.replace('\'', QStringLiteral("\\'"));
    QByteArray text = "StreamTitle='" + escaped.toUtf8() + "';";

    int numBlocks = std::min<int>((text.size() + 15) / 16, 255);
    QByteArray padded = text.leftJustified(numBlocks * 16, '\0', true);

    QByteArray out;
    out.append(char(numBlocks));
    out.append(padded);
    return out;
}

void WebServer::writeStreamData(QTcpSocket *socket, ClientState &state, const QByteArray &pcm)
{
    if (!state.icyMetadata)
    {
        socket->write(pcm);
        return;
    }

    int offset = 0;
    while (offset < pcm.size())
    {
        int chunkLen = std::min<int>(state.icyMetaInt - state.bytesSinceMeta, pcm.size() - offset);
        socket->write(pcm.constData() + offset, chunkLen);
        offset += chunkLen;
        state.bytesSinceMeta += chunkLen;
        if (state.bytesSinceMeta >= state.icyMetaInt)
        {
            socket->write(buildIcyMetaBlock(state));
            state.bytesSinceMeta = 0;
        }
    }
}

void WebServer::onAudioChunk(const QByteArray &pcm, int sampleRate, int numChannels)
{
    if (m_streamClients.isEmpty())
    {
        return;
    }

#if HAVE_MP3LAME
    if (!m_mp3Encoder.init(sampleRate, numChannels))
    {
        qCCritical(webServer) << "Failed to initialize MP3 encoder for" << sampleRate << "Hz" << numChannels << "ch";
        return;
    }
    const QByteArray data = m_mp3Encoder.encode(pcm);
    if (data.isEmpty())
    {
        return;  // LAME is still buffering internally; nothing to send yet
    }
#else
    const QByteArray &data = pcm;
#endif

    QList<QTcpSocket *> toRemove;
    for (QTcpSocket *socket : std::as_const(m_streamClients))
    {
        auto it = m_clients.find(socket);
        if (QAbstractSocket::ConnectedState != socket->state() || it == m_clients.end())
        {
            toRemove.append(socket);
            continue;
        }
        writeStreamData(socket, it.value(), data);
    }
    for (QTcpSocket *socket : std::as_const(toRemove))
    {
        m_streamClients.remove(socket);
    }
}

void WebServer::onStreamIdleTimeout()
{
    if (m_streamClients.isEmpty() && m_app->isStreamDecoupled())
    {
        m_app->requestStreamStop();
    }
}

QString WebServer::indexHtml()
{
    QString html = QStringLiteral(R"HTML(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AbracaDABra &mdash; dnt IPdio mini Web-UI</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { font-family: system-ui, sans-serif; background:#111; color:#eee; margin:0; padding:1.25rem; }
  h1 { font-size:1.25rem; margin:0 0 1rem 0; display:flex; align-items:center; justify-content:space-between; flex-wrap:wrap; gap:.5rem; }
  .badge { font-size:.75rem; font-weight:600; color:#86efac; background:#14532d66; border:1px solid #166534; padding:.2rem .55rem; border-radius:999px; }
  .layout { display:flex; flex-direction:column; gap:1rem; max-width:1440px; margin:0 auto; }
  .infoGroup { order:1; display:flex; flex-direction:column; gap:1rem; }
  .sideList { order:2; display:flex; flex-direction:column; }
  .ensembleCard { order:3; }
  @media (min-width: 960px) {
    .layout {
      display:grid;
      grid-template-columns: minmax(320px, 390px) minmax(0, 1fr);
      gap:1rem;
      align-items:start;
    }
    .sideList {
      order:0;
      grid-column:1;
      grid-row:1 / span 2;
      position:sticky;
      top:1rem;
      max-height:calc(100vh - 4.5rem);
    }
    .infoGroup { order:0; grid-column:2; grid-row:1; }
    .ensembleCard { order:0; grid-column:2; grid-row:2; }
  }
  .card { background:#1b1b1b; border:1px solid #333; border-radius:8px; padding:1rem; }
  .cardHead { display:flex; align-items:center; justify-content:space-between; gap:.5rem; margin-bottom:.65rem; }
  .cardHead h2 { font-size:1rem; margin:0; }
  .grid { display:grid; grid-template-columns: repeat(auto-fit, minmax(150px,1fr)); gap:.75rem; }
  .statusGrid { grid-template-columns: repeat(auto-fit, minmax(110px,1fr)); }
  .stat.wide { grid-column: 1 / -1; }
  .statSplit { grid-column: 1 / -1; display:flex; flex-wrap:wrap; gap:.75rem; align-items:flex-start; }
  .statSender { flex: 0 1 180px; min-width:140px; }
  .statTime { flex: 0 1 160px; min-width:130px; }
  .statTitle { flex: 1 1 220px; min-width:180px; }
  .stat label { display:block; font-size:.72rem; color:#999; text-transform:uppercase; letter-spacing:.05em; }
  .stat span { font-size:1.05rem; word-break:break-word; }
  table { width:100%; border-collapse: collapse; }
  th, td { text-align:left; padding:.45rem .5rem; border-bottom:1px solid #333; font-size:.9rem; }
  button, select, input[type="search"] { background:#2a2a2a; color:#eee; border:1px solid #444; border-radius:6px; padding:.4rem .7rem; font-size:.88rem; }
  button, select { cursor:pointer; }
  button:hover { background:#3a3a3a; }
  button:disabled { opacity:.55; cursor:default; }
  button.tuned { background:#1d4ed8; border-color:#2563eb; color:#fff; }
  button.playing { background:#2e7d32; border-color:#2e7d32; color:#fff; }
  button.btnSm { padding:.28rem .55rem; font-size:.8rem; }
  .btnGroup { display:inline-flex; gap:.35rem; align-items:center; flex-shrink:0; }
  #player { width:100%; margin-top:.5rem; }
  .row { display:flex; gap:.5rem; align-items:center; flex-wrap:wrap; }
  .statusRow { display:flex; gap:1rem; align-items:flex-start; flex-wrap:wrap; }
  #slide { width:160px; height:160px; object-fit:contain; border-radius:6px; background:#222; border:1px solid #333; display:none; }
  #slide.visible { display:block; }
  details.card summary { cursor:pointer; font-weight:600; }
  details.card[open] summary { margin-bottom:.75rem; }
  .muted { color:#9ca3af; font-size:.82rem; }
  #stationFilter { width:100%; margin-bottom:.6rem; }
  .stationScroll { overflow-y:auto; flex:1; min-height:140px; max-height:60vh; padding-right:.2rem; }
  @media (min-width: 960px) {
    .stationScroll { max-height:none; }
  }
  .stRow { display:flex; align-items:center; justify-content:space-between; gap:.5rem; padding:.45rem .5rem; border-bottom:1px solid #2a2a2a; border-radius:6px; }
  .stRow.active { background:#1e293b88; border-color:#334155; }
  .stMeta { display:flex; align-items:center; gap:.45rem; min-width:0; flex:1; }
  .stName { font-size:.88rem; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .chBadge { font-family:monospace; font-size:.72rem; color:#93c5fd; background:#1e3a8a55; border:1px solid #1e40af; border-radius:4px; padding:.1rem .35rem; flex-shrink:0; }
  .advSubBar { display:flex; align-items:center; justify-content:space-between; flex-wrap:wrap; gap:.5rem; margin-top:.85rem; padding-top:.75rem; border-top:1px solid #2a2a2a; }
  .cuWrap { margin-top:.85rem; padding-top:.75rem; border-top:1px solid #2a2a2a; }
  .cuHead { display:flex; align-items:center; justify-content:space-between; flex-wrap:wrap; gap:.4rem; margin-bottom:.45rem; font-size:.85rem; }
  .cuBar { position:relative; height:24px; background:#262626; border:1px solid #383838; border-radius:6px; overflow:hidden; }
  .cuSeg { position:absolute; top:0; bottom:0; border-right:1px solid #111; transition:filter .15s; cursor:pointer; }
  .cuSeg:hover { filter:brightness(1.2); }
  .cuSeg.active { box-shadow:inset 0 0 0 2px #fff; z-index:2; }
  .cuLegend { display:flex; flex-wrap:wrap; gap:.35rem; margin-top:.55rem; }
  .cuChip { display:inline-flex; align-items:center; gap:.35rem; font-size:.75rem; background:#242424; border:1px solid #333; border-radius:5px; padding:.18rem .45rem; }
  .cuChip.active { border-color:#60a5fa; background:#1e293b; color:#fff; }
  .cuDot { width:9px; height:9px; border-radius:2px; flex-shrink:0; }
  .layout.epgOpen .epgHide { display:none !important; }
  #epgCard { display:none; flex-direction:column; gap:.75rem; min-width:0; }
  .layout.epgOpen #epgCard { display:flex; }
  .epgHead { display:flex; align-items:center; justify-content:space-between; flex-wrap:wrap; gap:.5rem; }
  .epgDayBadge { font-size:.82rem; font-weight:600; color:#93c5fd; background:#1e3a8a55; border:1px solid #1e40af; padding:.22rem .6rem; border-radius:999px; }
  #epgScroll { position:relative; overflow:auto; border:1px solid #2e2e2e; border-radius:6px; background:#151515; max-height:68vh; cursor:grab; user-select:none; }
  #epgScroll.dragging { cursor:grabbing; }
  .epgGrid { position:relative; width:max-content; min-width:100%; }
  .epgHeaderRow { position:sticky; top:0; z-index:6; display:flex; background:#1e1e1e; border-bottom:1px solid #333; }
  .epgCorner { position:sticky; left:0; z-index:8; width:155px; min-width:155px; padding:.45rem .55rem; background:#1e1e1e; border-right:1px solid #333; font-size:.78rem; font-weight:600; color:#9ca3af; }
  .epgRuler { position:relative; width:8640px; height:32px; flex-shrink:0; }
  .epgTick { position:absolute; top:0; bottom:0; border-left:1px solid #2f2f2f; padding:6px 0 0 5px; font-size:.74rem; color:#9ca3af; white-space:nowrap; }
  .epgTick.midnight { border-left:2px solid #60a5fa; color:#93c5fd; font-weight:600; }
  .epgNowDot { position:absolute; top:0; bottom:0; width:2px; background:#ef4444; z-index:2; pointer-events:none; }
  .epgRow { display:flex; border-bottom:1px solid #242424; }
  .epgRow.epgCurRow { background:#17255444; }
  .epgSvc { position:sticky; left:0; z-index:4; width:155px; min-width:155px; padding:.5rem .55rem; background:#1a1a1a; border-right:1px solid #333; font-size:.84rem; font-weight:500; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; display:flex; align-items:center; }
  .epgRow.epgCurRow .epgSvc { background:#172554; color:#bfdbfe; font-weight:700; border-right-color:#2563eb; }
  .epgTrack { position:relative; width:8640px; height:48px; flex-shrink:0; }
  .epgProg { position:absolute; top:5px; bottom:5px; background:#262626; border:1px solid #3a3a3a; border-radius:5px; padding:3px 6px; overflow:hidden; cursor:pointer; display:flex; flex-direction:column; justify-content:center; }
  .epgProg:hover { background:#323232; border-color:#525252; }
  .epgProg.isNow { border-color:#2563eb; background:#1e3a8a55; }
  .epgRow.epgCurRow .epgProg.isNow { border-color:#22c55e; background:#14532d88; }
  .epgProg.selected { box-shadow:0 0 0 2px #93c5fd; }
  .epgProgTitle { font-size:.8rem; font-weight:600; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  .epgProgTime { font-size:.69rem; color:#9ca3af; white-space:nowrap; }
  .epgNowLine { position:absolute; top:32px; bottom:0; width:2px; background:#ef4444; z-index:5; pointer-events:none; }
  #epgDetail { display:none; background:#161616; border:1px solid #333; border-radius:6px; padding:.7rem .85rem; font-size:.88rem; }
  #epgDetail.visible { display:block; }
</style>
</head>
<body>
<h1>
  <span>AbracaDABra &mdash; dnt IPdio mini DAB/DAB+</span>
  <span class="badge">S/PDIF 100% + Web-UI</span>
</h1>

<div class="layout" id="mainLayout">
  <aside class="card sideList">
    <div class="cardHead">
      <h2>Senderliste <span id="stationCount" class="muted">(0)</span></h2>
      <button id="scanBtn" class="btnSm" onclick="toggleScan()">Suchlauf</button>
    </div>
    <div id="scanInfo" class="muted" style="margin-bottom:.45rem; display:none;"></div>
    <input type="search" id="stationFilter" placeholder="Sender filtern..." oninput="renderStationList()">
    <div id="stationList" class="stationScroll">
      <div class="muted">Lade Senderliste...</div>
    </div>
  </aside>

  <div class="infoGroup">
    <div class="card">
      <div class="row">
        <select id="channelSelect"></select>
        <button onclick="tune()">Tune</button>
        <span id="tuneFeedback" class="muted"></span>
        <button id="epgToggleBtn" style="margin-left:auto; display:none;" onclick="toggleEpgView()">EPG</button>
      </div>
    </div>

    <div class="card statusRow epgHide">
      <img id="slide" alt="Station image">
      <div class="grid statusGrid" style="flex:1">
        <div class="stat"><label>Ensemble</label><span id="ensemble">-</span></div>
        <div class="stat"><label>Kanal</label><span id="channel">-</span></div>
        <div class="stat"><label>Frequenz</label><span id="freq">-</span></div>
        <div class="stat"><label>Sync</label><span id="sync">-</span></div>
        <div class="stat"><label>SNR</label><span id="snr">-</span></div>
        <div class="stat wide statSplit">
          <div class="stat statSender"><label>Sender</label><span id="nowplaying">-</span></div>
          <div class="stat statTime"><label>DAB-Zeit</label><span id="dabTime">-</span></div>
          <div class="stat statTitle"><label>Titel (DL+)</label><span id="title">-</span></div>
        </div>
        <div class="stat wide"><label>Radiotext (DLS)</label><span id="dlsText">-</span></div>
        <div class="stat wide"><label>DL Plus info</label><span id="dlPlusTags">-</span></div>
      </div>
    </div>

    <div id="streamCard" class="card statusRow epgHide" style="display:none; border-color:#2e7d32;">
      <img id="streamSlide" alt="Stream cover" style="width:96px; height:96px; object-fit:contain; border-radius:6px; background:#222; border:1px solid #333; display:none;">
      <div class="grid" style="flex:1">
        <div class="stat wide" style="display:flex; align-items:center; justify-content:space-between; gap:.5rem; flex-wrap:wrap;">
          <span class="badge">Webstream entkoppelt (S/PDIF bleibt unabhängig)</span>
          <button id="syncStreamBtn" class="btnSm" onclick="syncToStream()">Auf S/PDIF übernehmen</button>
        </div>
        <div class="stat"><label>Webstream Sender</label><span id="streamLabel">-</span></div>
        <div class="stat"><label>Webstream Titel (DL+)</label><span id="streamTitle">-</span></div>
        <div class="stat wide"><label>Webstream Radiotext (DLS)</label><span id="streamDls">-</span></div>
      </div>
    </div>

    <details class="card epgHide">
      <summary>Technische Informationen</summary>
      <div class="grid">
        <div class="stat"><label>Ensemble ID</label><span id="advEid">-</span></div>
        <div class="stat"><label>Service ID (SID)</label><span id="advSid">-</span></div>
        <div class="stat"><label>TII Code</label><span id="advTii">-</span></div>
        <div class="stat"><label>Standort (TII DB)</label><span id="advTx">-</span></div>
        <div class="stat"><label>Freq. offset</label><span id="freqOffset">-</span></div>
        <div class="stat"><label>Sub-ch / Schutz</label><span id="advBitRate">-</span></div>
        <div class="stat"><label>FIB errors</label><span id="fibErrors">-</span></div>
        <div class="stat"><label>MSC CRC errors</label><span id="mscCrcErrors">-</span></div>
        <div class="stat"><label>RS bit errors</label><span id="rsBitErrors">-</span></div>
        <div class="stat"><label>RS uncorrectable</label><span id="rsUncorrectable">-</span></div>
      </div>
      <div class="advSubBar">
        <div>
          <div id="tiiMulti" class="muted">TII-Empfang: -</div>
          <div id="tiiDbInfo" class="muted">TII-Datenbank: -</div>
        </div>
        <button id="tiiUpdateBtn" class="btnSm" onclick="updateTiiDb()">TII-DB laden</button>
      </div>
      <div class="cuWrap">
        <div class="cuHead">
          <strong>Ensemble-Zusammenstellung</strong>
          <span id="cuSummary" class="muted">0 / 864 CU</span>
        </div>
        <div id="cuBar" class="cuBar"></div>
        <div id="cuLegend" class="cuLegend"></div>
      </div>
    </details>
    <div id="epgCard" class="card">
      <div class="epgHead">
        <div class="row">
          <strong style="font-size:1rem;">Programmführer (EPG)</strong>
          <span id="epgDayBadge" class="epgDayBadge">Heute</span>
        </div>
        <div class="btnGroup">
          <button class="btnSm" onclick="scrollEpgTo('today')">Heute</button>
          <button class="btnSm" onclick="scrollEpgTo('now')">Jetzt</button>
          <button class="btnSm" onclick="scrollEpgTo('tomorrow')">Morgen</button>
          <button class="btnSm" onclick="toggleEpgView(false)">Schließen</button>
        </div>
      </div>
      <div id="epgScroll"></div>
      <div id="epgDetail"></div>
    </div>
  </div>

  <div class="card ensembleCard epgHide">
    <div class="cardHead">
      <h2>Aktuelles Ensemble</h2>
      <span id="ensembleBadge" class="muted"></span>
    </div>
    <table>
      <thead><tr><th>Service</th><th>Sub-ch</th><th>Bitrate</th><th>Aktion</th></tr></thead>
      <tbody id="services"></tbody>
    </table>
    <div class="row" style="margin-top:.75rem;">
      <span id="playerStatus">Not playing</span>
      <button onclick="stopPlayback()">Stop</button>
      <label for="volume">Volume</label>
      <input type="range" id="volume" min="0" max="100" value="100" oninput="setVolume(this.value)">
    </div>
    <audio id="player" style="display:none"></audio>
  </div>
</div>

<script>
let lastSlideVersion = 0;
let lastStreamSlideVersion = 0;
let lastStatus = null;
let scannedStations = [];
let scannedLoaded = false;
let scanning = false;
let scanAbort = false;
let tuneSeq = 0;
let browserPlayingKey = '';
let browserStreaming = false;
let lastListRefresh = 0;
const CU_PALETTE = ['#4285F4','#EA4335','#FBBC05','#34A853','#9C27B0','#FF9800','#00BCD4','#795548'];

function normSid(v){
  if (v === null || v === undefined || v === '') return '';
  const n = Number(v);
  if (!Number.isNaN(n) && n > 0) return '0x' + n.toString(16).toLowerCase();
  return String(v).trim().toLowerCase();
}

function svcKey(sid, scids){
  return normSid(sid) + ':' + Number(scids || 0);
}

function isServiceActive(sid, scids, ch, statusObj){
  const s = statusObj || lastStatus;
  if (!s || !s.current || !s.current.playing) return false;
  const curSid = normSid(s.current.sid);
  const tgtSid = normSid(sid);
  if (!curSid || curSid !== tgtSid) return false;
  if (Number(s.current.scids || 0) !== Number(scids || 0)) return false;
  if (ch && s.channel && String(s.channel).toUpperCase() !== String(ch).toUpperCase()) return false;
  return true;
}

function isStreamServiceActive(sid, scids, ch, statusObj){
  const s = statusObj || lastStatus;
  if (s && s.stream && s.stream.decoupled && s.stream.sid) {
    if (normSid(s.stream.sid) !== normSid(sid)) return false;
    if (Number(s.stream.scids || 0) !== Number(scids || 0)) return false;
    if (ch && s.channel && String(s.channel).toUpperCase() !== String(ch).toUpperCase()) return false;
    return true;
  }
  const hasCur = Boolean(s && s.current && s.current.playing && s.current.sid);
  if (hasCur) return isServiceActive(sid, scids, ch, s);
  return browserPlayingKey === svcKey(sid, scids);
}

async function ensureStatus(){
  if (lastStatus) return lastStatus;
  try {
    const r = await fetch('/api/status');
    lastStatus = await r.json();
  } catch (e) {}
  return lastStatus;
}

async function loadChannels(){
  const r = await fetch('/api/channels');
  const j = await r.json();
  const sel = document.getElementById('channelSelect');
  sel.innerHTML = '';
  (j.channels || []).forEach(c => {
    const opt = document.createElement('option');
    opt.value = c.channel;
    opt.textContent = c.channel + ' (' + (c.frequencyKHz/1000).toFixed(3) + ' MHz)';
    sel.appendChild(opt);
  });
}

async function tune(){
  const ch = document.getElementById('channelSelect').value;
  await fetch('/api/tune', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({channel: ch})});
  setTimeout(refresh, 400);
}

async function updateTiiDb(){
  const btn = document.getElementById('tiiUpdateBtn');
  const info = document.getElementById('tiiDbInfo');
  btn.disabled = true;
  info.textContent = 'TII-Datenbank: Download gestartet...';
  try {
    const r = await fetch('/api/tii/update', {method:'POST', headers:{'Content-Type':'application/json'}, body:'{}'});
    if (!r.ok) {
      const errTxt = await r.text();
      info.textContent = 'TII-Datenbank Fehler: ' + errTxt;
      btn.disabled = false;
    } else {
      setTimeout(refresh, 800);
    }
  } catch (e) {
    info.textContent = 'TII-Datenbank Download fehlgeschlagen.';
    btn.disabled = false;
  }
}

async function tuneOnly(sid, scids, ch){
  const mySeq = ++tuneSeq;
  const fb = document.getElementById('tuneFeedback');
  const tgtSid = normSid(sid);
  const tgtScids = Number(scids || 0);
  const s = await ensureStatus();
  const curCh = (s && s.channel) ? String(s.channel).toUpperCase() : '';
  const wantCh = ch ? String(ch).toUpperCase() : '';

  if (wantCh && wantCh !== curCh) {
    if (fb) fb.textContent = 'Wechsle auf Kanal ' + ch + '...';
    await fetch('/api/tune', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({channel: ch})
    });
    for (let i = 0; i < 18; i++) {
      if (mySeq !== tuneSeq) return false;
      await new Promise(res => setTimeout(res, 300));
      try {
        const sr = await fetch('/api/status');
        const sj = await sr.json();
        lastStatus = sj;
        const sameCh = sj.channel && String(sj.channel).toUpperCase() === wantCh;
        const hasSvc = Array.isArray(sj.services) && sj.services.some(x => normSid(x.sid) === tgtSid);
        if (sameCh && hasSvc) break;
      } catch (e) {}
    }
  }

  if (mySeq !== tuneSeq) return false;
  if (fb) fb.textContent = 'Stelle Sender ein...';
  let ok = false;
  for (let attempt = 0; attempt < 4; attempt++) {
    if (mySeq !== tuneSeq) return false;
    try {
      const resp = await fetch('/api/service', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({sid: sid, scids: tgtScids})
      });
      if (resp.ok) {
        ok = true;
        break;
      }
    } catch (e) {}
    await new Promise(res => setTimeout(res, 500));
  }
  if (fb) fb.textContent = '';
  await refresh();
  return ok;
}

async function tuneService(sid, scids, ch){
  await tuneOnly(sid, scids, ch);
}

async function loadStationList(){
  try {
    const r = await fetch('/api/stationlist');
    const j = await r.json();
    if (Array.isArray(j.stations)) {
      scannedStations = j.stations;
      scannedLoaded = true;
      renderStationList();
    }
  } catch (e) { console.error(e); }
}

async function syncToStream(){
  if (!lastStatus || !lastStatus.stream || !lastStatus.stream.decoupled || !lastStatus.stream.sid) return;
  await tuneOnly(lastStatus.stream.sid, lastStatus.stream.scids, lastStatus.channel || '');
}

function makeActionButtons(sid, scids, ch){
  const wrap = document.createElement('div');
  wrap.className = 'btnGroup';
  const active = isServiceActive(sid, scids, ch);
  const bPlaying = browserStreaming && isStreamServiceActive(sid, scids, ch);

  const btnTune = document.createElement('button');
  btnTune.className = 'btnSm' + (active ? ' tuned' : '');
  btnTune.textContent = 'Tune';
  btnTune.title = 'Lokal (S/PDIF) einstellen (im gleichen Ensemble bleibt Webstream unabhängig)';
  btnTune.onclick = () => tuneService(sid, scids, ch);

  const btnPlay = document.createElement('button');
  btnPlay.className = 'btnSm' + (bPlaying ? ' playing' : '');
  btnPlay.textContent = bPlaying ? 'Playing' : 'Play';
  btnPlay.title = 'Im Browser abspielen (im gleichen Ensemble unabhängig von S/PDIF)';
  btnPlay.onclick = () => playService(sid, scids, ch);

  wrap.appendChild(btnTune);
  wrap.appendChild(btnPlay);
  return wrap;
}

function renderStationList(){
  const box = document.getElementById('stationList');
  const cnt = document.getElementById('stationCount');
  const q = (document.getElementById('stationFilter').value || '').trim().toLowerCase();
  cnt.textContent = '(' + scannedStations.length + ')';
  box.innerHTML = '';

  const filtered = scannedStations.filter(st => {
    if (!q) return true;
    const name = String(st.name || st.label || '').toLowerCase();
    const ch = String(st.channel || '').toLowerCase();
    return name.includes(q) || ch.includes(q);
  });

  if (!filtered.length) {
    const msg = document.createElement('div');
    msg.className = 'muted';
    msg.textContent = scannedStations.length
      ? 'Keine Treffer fuer diesen Filter.'
      : 'Noch kein Suchlauf gespeichert. Klicke auf "Suchlauf" oder nutze am Frontpanel Reply -> Sendersuchlauf.';
    box.appendChild(msg);
    return;
  }

  filtered.forEach(st => {
    const sid = st.sid || st.id;
    const scids = Number(st.scids || 0);
    const ch = st.channel || '';
    const active = isServiceActive(sid, scids, ch);

    const row = document.createElement('div');
    row.className = 'stRow' + (active ? ' active' : '');

    const meta = document.createElement('div');
    meta.className = 'stMeta';

    if (ch) {
      const badge = document.createElement('span');
      badge.className = 'chBadge';
      badge.textContent = ch;
      meta.appendChild(badge);
    }

    const nameEl = document.createElement('span');
    nameEl.className = 'stName';
    nameEl.textContent = st.name || st.label || sid;
    nameEl.title = (st.name || st.label || sid) + (ch ? ' (' + ch + ')' : '');
    meta.appendChild(nameEl);

    row.appendChild(meta);
    row.appendChild(makeActionButtons(sid, scids, ch));
    box.appendChild(row);
  });
}

function renderServicesTable(s){
  const tbody = document.getElementById('services');
  tbody.innerHTML = '';
  const ch = (s && s.channel) ? s.channel : '';
  const eb = document.getElementById('ensembleBadge');
  if (eb) eb.textContent = ch ? ('Kanal ' + ch) : '';

  (s && s.services ? s.services : []).forEach(svc => {
    const tr = document.createElement('tr');

    const tdLabel = document.createElement('td');
    tdLabel.textContent = svc.label;
    tr.appendChild(tdLabel);

    const tdSub = document.createElement('td');
    tdSub.textContent = svc.subChId;
    tr.appendChild(tdSub);

    const tdBr = document.createElement('td');
    tdBr.textContent = svc.bitRate + ' kbps';
    tr.appendChild(tdBr);

    const tdAct = document.createElement('td');
    tdAct.appendChild(makeActionButtons(svc.sid, svc.scids, ch));
    tr.appendChild(tdAct);

    tbody.appendChild(tr);
  });
}

function renderCuLayout(s){
  const bar = document.getElementById('cuBar');
  const leg = document.getElementById('cuLegend');
  const sum = document.getElementById('cuSummary');
  if (!bar || !leg || !sum) return;
  const subs = (s && Array.isArray(s.subchannels)) ? s.subchannels : [];
  bar.innerHTML = '';
  leg.innerHTML = '';
  let usedCu = 0;

  subs.forEach((sc, idx) => {
    const numCU = Number(sc.numCU || 0);
    const startCU = Number(sc.startCU || 0);
    usedCu += numCU;
    const color = CU_PALETTE[idx % CU_PALETTE.length];
    const lbl = (Array.isArray(sc.labels) && sc.labels.length) ? sc.labels.join(', ') : ('SubCh ' + sc.subChId);
    const tip = 'SubCh ' + sc.subChId + ': ' + lbl + ' | ' + numCU + ' CU [' + startCU + '..' + (startCU + Math.max(0, numCU - 1)) + '] | ' + (sc.bitRate ? (sc.bitRate + ' kbps ') : '') + (sc.codec || '') + (sc.protection ? (' (' + sc.protection + ')') : '');

    const seg = document.createElement('div');
    seg.className = 'cuSeg' + (sc.active ? ' active' : '');
    seg.style.left = ((startCU / 864) * 100).toFixed(2) + '%';
    seg.style.width = ((numCU / 864) * 100).toFixed(2) + '%';
    seg.style.background = color;
    seg.title = tip;
    bar.appendChild(seg);

    const chip = document.createElement('div');
    chip.className = 'cuChip' + (sc.active ? ' active' : '');
    chip.title = tip;
    const dot = document.createElement('span');
    dot.className = 'cuDot';
    dot.style.background = color;
    const txt = document.createElement('span');
    txt.textContent = lbl + ' (' + numCU + ' CU)';
    chip.appendChild(dot);
    chip.appendChild(txt);
    leg.appendChild(chip);
  });

  const freeCu = Math.max(0, 864 - usedCu);
  const pct = ((usedCu / 864) * 100).toFixed(1);
  sum.textContent = usedCu + ' / 864 CU (' + pct + '%) - ' + freeCu + ' CU frei';
}

function renderAllLists(){
  renderStationList();
  if (lastStatus) renderServicesTable(lastStatus);
}

async function toggleScan(){
  if (scanning) {
    scanAbort = true;
    document.getElementById('scanBtn').disabled = true;
    return;
  }
  scanning = true;
  scanAbort = false;
  const btn = document.getElementById('scanBtn');
  const info = document.getElementById('scanInfo');
  btn.textContent = 'Abbrechen';
  info.style.display = 'block';
  info.textContent = 'Starte Band-III Suchlauf...';

  const prev = lastStatus;
  const prevCh = prev && prev.channel ? prev.channel : '';
  const prevSid = prev && prev.current && prev.current.playing ? prev.current.sid : '';
  const prevScids = prev && prev.current ? Number(prev.current.scids || 0) : 0;

  try {
    const chResp = await fetch('/api/channels');
    const chJson = await chResp.json();
    const channels = (chJson.channels || []).map(c => c.channel);
    const found = {};

    for (let i = 0; i < channels.length; i++) {
      if (scanAbort) break;
      const ch = channels[i];
      info.textContent = 'Suche Kanal ' + ch + ' (' + (i + 1) + '/' + channels.length + ') - ' + Object.keys(found).length + ' Sender';
      await fetch('/api/tune', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({channel: ch})
      });
      await new Promise(res => setTimeout(res, 1300));
      if (scanAbort) break;
      let stResp = await fetch('/api/status');
      let st = await stResp.json();
      if (Number(st.syncLevel || 0) > 0) {
        await new Promise(res => setTimeout(res, 1100));
        if (scanAbort) break;
        stResp = await fetch('/api/status');
        st = await stResp.json();
        (st.services || []).forEach(svc => {
          const sid = svc.sid;
          const scids = Number(svc.scids || 0);
          const label = String(svc.label || '').trim();
          if (sid && label) {
            const key = ch + ':' + normSid(sid) + ':' + scids;
            found[key] = {id: sid, sid: sid, scids: scids, name: label, channel: ch};
          }
        });
      }
    }

    const list = Object.values(found).sort((a, b) => a.name.toLowerCase().localeCompare(b.name.toLowerCase()));
    if (list.length > 0) {
      scannedStations = list;
      await fetch('/api/stationlist', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({stations: list, stations_scanned: true})
      });
      renderStationList();
      info.textContent = 'Suchlauf fertig: ' + list.length + ' Sender gespeichert.';
      if (prevSid && prevCh) {
        await tuneOnly(prevSid, prevScids, prevCh);
      } else {
        await tuneOnly(list[0].sid, list[0].scids, list[0].channel);
      }
    } else {
      info.textContent = scanAbort ? 'Suchlauf abgebrochen.' : 'Keine Sender gefunden.';
    }
  } catch (e) {
    console.error(e);
    info.textContent = 'Fehler beim Suchlauf.';
  } finally {
    scanning = false;
    scanAbort = false;
    btn.disabled = false;
    btn.textContent = 'Suchlauf';
  }
}
)HTML");

#if HAVE_MP3LAME
    html += QStringLiteral(R"HTML(
const player = document.getElementById('player');
let lastSid = null;
let lastScids = 0;
let lastCh = '';

if ('mediaSession' in navigator) {
  navigator.mediaSession.setActionHandler('play', () => { if (null !== lastSid) playService(lastSid, lastScids, lastCh); });
  navigator.mediaSession.setActionHandler('pause', () => stopPlayback(true));
  navigator.mediaSession.setActionHandler('stop', () => stopPlayback(true));
}

function setVolume(percent){
  player.volume = percent / 100;
}

function stopPlayback(notifyBackend = true){
  player.pause();
  player.removeAttribute('src');
  player.load();
  browserStreaming = false;
  browserPlayingKey = '';
  document.getElementById('playerStatus').textContent = 'Not playing';
  if ('mediaSession' in navigator) {
    navigator.mediaSession.playbackState = 'paused';
    navigator.mediaSession.setPositionState();
  }
  if (notifyBackend) {
    fetch('/api/stream/stop', {method:'POST'}).then(() => setTimeout(refresh, 150)).catch(() => {});
  }
  renderAllLists();
}

async function playService(sid, scids, ch){
  lastSid = sid;
  lastScids = Number(scids || 0);
  lastCh = ch || '';
  stopPlayback(false);

  const curStatus = await ensureStatus();
  const curCh = (curStatus && curStatus.channel) ? String(curStatus.channel).toUpperCase() : '';
  const wantCh = ch ? String(ch).toUpperCase() : '';
  const needMuxTune = !curStatus || !curStatus.current || !curStatus.current.playing || (wantCh && curCh && wantCh !== curCh);

  if (needMuxTune) {
    await tuneOnly(sid, scids, ch);
  } else {
    try {
      const sr = await fetch('/api/stream', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({sid: sid, scids: Number(scids || 0), channel: ch || ''})
      });
      if (!sr.ok) {
        await tuneOnly(sid, scids, ch);
      } else {
        const sj = await sr.json().catch(() => ({}));
        if (sj.decoupled) {
          await new Promise(res => setTimeout(res, 200));
        }
        await refresh();
      }
    } catch (e) {
      await tuneOnly(sid, scids, ch);
    }
  }

  browserStreaming = true;
  browserPlayingKey = svcKey(sid, scids);
  document.getElementById('playerStatus').textContent = 'Playing...';
  if ('mediaSession' in navigator) {
    navigator.mediaSession.playbackState = 'playing';
    try {
      navigator.mediaSession.setPositionState({duration: Infinity, playbackRate: 1, position: 0});
    } catch (e) {}
  }
  renderAllLists();
  player.src = '/stream/audio?_=' + Date.now();
  player.volume = document.getElementById('volume').value / 100;
  try {
    await player.play();
  } catch (e) {
    console.error(e);
    stopPlayback(true);
  }
}
player.onerror = () => stopPlayback(true);
)HTML");
#else
    html += QStringLiteral(R"HTML(
let audioCtx = null;
let gainNode = null;
let nextStartTime = 0;
let streamXhr = null;
let leftoverBytes = new Uint8Array(0);

if ('mediaSession' in navigator) {
  navigator.mediaSession.setActionHandler('pause', () => stopPlayback(true));
  navigator.mediaSession.setActionHandler('stop', () => stopPlayback(true));
}

function setVolume(percent){
  if (gainNode) {
    gainNode.gain.value = percent / 100;
  }
}

function stopPlayback(notifyBackend = true){
  if (streamXhr) {
    const x = streamXhr;
    streamXhr = null;
    x.abort();
  }
  leftoverBytes = new Uint8Array(0);
  browserStreaming = false;
  browserPlayingKey = '';
  document.getElementById('playerStatus').textContent = 'Not playing';
  if ('mediaSession' in navigator) {
    navigator.mediaSession.playbackState = 'paused';
  }
  if (notifyBackend) {
    fetch('/api/stream/stop', {method:'POST'}).then(() => setTimeout(refresh, 150)).catch(() => {});
  }
  renderAllLists();
}

function feedPcm(chunk, numChannels, sampleRate){
  let combined = chunk;
  if (leftoverBytes.length) {
    combined = new Uint8Array(leftoverBytes.length + chunk.length);
    combined.set(leftoverBytes, 0);
    combined.set(chunk, leftoverBytes.length);
  }
  const frameBytes = 2 * numChannels;
  const usableLen = combined.length - (combined.length % frameBytes);
  leftoverBytes = combined.slice(usableLen);
  if (0 === usableLen) {
    return;
  }

  const view = new DataView(combined.buffer, combined.byteOffset, usableLen);
  const numFrames = usableLen / frameBytes;
  const buffer = audioCtx.createBuffer(numChannels, numFrames, sampleRate);
  for (let ch = 0; ch < numChannels; ch++) {
    const channelData = buffer.getChannelData(ch);
    for (let i = 0; i < numFrames; i++) {
      channelData[i] = view.getInt16(i * frameBytes + ch * 2, true) / 32768;
    }
  }

  const src = audioCtx.createBufferSource();
  src.buffer = buffer;
  src.connect(gainNode);
  const now = audioCtx.currentTime;
  if (nextStartTime < now) {
    nextStartTime = now + 0.2;
  }
  src.start(nextStartTime);
  nextStartTime += buffer.duration;
}

async function playService(sid, scids, ch){
  if (!audioCtx) {
    audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    gainNode = audioCtx.createGain();
    gainNode.connect(audioCtx.destination);
    setVolume(document.getElementById('volume').value);
  }
  if ('suspended' === audioCtx.state) {
    audioCtx.resume();
  }
  const unlockBuffer = audioCtx.createBuffer(1, 1, 22050);
  const unlockSrc = audioCtx.createBufferSource();
  unlockSrc.buffer = unlockBuffer;
  unlockSrc.connect(audioCtx.destination);
  unlockSrc.start(0);

  stopPlayback(false);

  const curStatus = await ensureStatus();
  const curCh = (curStatus && curStatus.channel) ? String(curStatus.channel).toUpperCase() : '';
  const wantCh = ch ? String(ch).toUpperCase() : '';
  const needMuxTune = !curStatus || !curStatus.current || !curStatus.current.playing || (wantCh && curCh && wantCh !== curCh);

  if (needMuxTune) {
    await tuneOnly(sid, scids, ch);
  } else {
    try {
      const sr = await fetch('/api/stream', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({sid: sid, scids: Number(scids || 0), channel: ch || ''})
      });
      if (!sr.ok) {
        await tuneOnly(sid, scids, ch);
      } else {
        const sj = await sr.json().catch(() => ({}));
        if (sj.decoupled) {
          await new Promise(res => setTimeout(res, 200));
        }
        await refresh();
      }
    } catch (e) {
      await tuneOnly(sid, scids, ch);
    }
  }

  browserStreaming = true;
  browserPlayingKey = svcKey(sid, scids);
  nextStartTime = 0;
  document.getElementById('playerStatus').textContent = 'Playing...';
  if ('mediaSession' in navigator) {
    navigator.mediaSession.playbackState = 'playing';
  }
  renderAllLists();

  const xhr = new XMLHttpRequest();
  xhr.open('GET', '/stream/audio?_=' + Date.now(), true);
  xhr.overrideMimeType('text/plain; charset=x-user-defined');
  streamXhr = xhr;

  let sampleRate = 48000;
  let numChannels = 2;
  let headersParsed = false;
  let lastLength = 0;

  xhr.onprogress = function(){
    if (streamXhr !== xhr) {
      return;
    }
    if (!headersParsed) {
      sampleRate = parseInt(xhr.getResponseHeader('X-Audio-Sample-Rate') || '48000', 10);
      numChannels = parseInt(xhr.getResponseHeader('X-Audio-Channels') || '2', 10);
      headersParsed = true;
    }
    const text = xhr.responseText;
    const newText = text.substring(lastLength);
    lastLength = text.length;
    if (newText.length) {
      const bytes = new Uint8Array(newText.length);
      for (let i = 0; i < newText.length; i++) {
        bytes[i] = newText.charCodeAt(i) & 0xff;
      }
      feedPcm(bytes, numChannels, sampleRate);
    }
  };
  xhr.onloadend = function(){
    if (streamXhr === xhr) {
      streamXhr = null;
      browserStreaming = false;
      browserPlayingKey = '';
      document.getElementById('playerStatus').textContent = 'Not playing';
      renderAllLists();
    }
  };
  xhr.send();
}
)HTML");
#endif

    html += QStringLiteral(R"HTML(
function fmtSync(level){
  return ({0:'No sync', 1:'Null sync', 2:'Full sync'})[level] ?? level;
}
async function refresh(){
  try {
    const r = await fetch('/api/status');
    const s = await r.json();
    lastStatus = s;

    document.getElementById('ensemble').textContent = s.ensemble && s.ensemble.valid ? (s.ensemble.label || '(unnamed)') : '-';
    document.getElementById('channel').textContent = s.channel || '-';
    document.getElementById('freq').textContent = s.frequencyKHz ? (s.frequencyKHz/1000).toFixed(3) + ' MHz' : '-';
    document.getElementById('sync').textContent = fmtSync(s.syncLevel);
    document.getElementById('snr').textContent = (s.snr ?? 0).toFixed(1) + ' dB';
    document.getElementById('nowplaying').textContent = (s.current && s.current.playing) ? s.current.label : '(none)';
    document.getElementById('dabTime').textContent = s.dabTime || '-';

    const chSel = document.getElementById('channelSelect');
    if (s.channel && chSel && chSel.value !== s.channel && document.activeElement !== chSel) {
      chSel.value = s.channel;
    }

    const cur = s.current || {};
    let titleText = '-';
    if (cur.dlPlusTitle) {
      titleText = cur.dlPlusArtist ? (cur.dlPlusArtist + ' - ' + cur.dlPlusTitle) : cur.dlPlusTitle;
    }
    document.getElementById('title').textContent = titleText;
    document.getElementById('dlsText').textContent = (cur.dls && cur.dls.trim()) ? cur.dls : '-';
    const tags = cur.dlPlusTags || [];
    document.getElementById('dlPlusTags').textContent = tags.length ? tags.map(t => t.label + ': ' + t.text).join(' | ') : '-';

    const adv = s.advanced || {};
    document.getElementById('advEid').textContent = adv.eid ? (adv.eid + (adv.ecc ? (' (ECC ' + adv.ecc + ')') : '')) : '-';
    document.getElementById('advSid').textContent = adv.sid ? (adv.sid + ' (SCIdS ' + (adv.scids ?? 0) + ')') : '-';
    const tiiList = Array.isArray(adv.tiiList) ? adv.tiiList : [];
    const topTii = tiiList.length ? tiiList[0] : null;
    document.getElementById('advTii').textContent = adv.tii ? (adv.tii + (topTii && topTii.levelDb != null ? (' (' + Number(topTii.levelDb).toFixed(1) + ' dB)') : '')) : '-';
    document.getElementById('advTx').textContent = adv.transmitter ? (adv.transmitter + (adv.tiiPowerKw > 0 ? (' (' + Number(adv.tiiPowerKw).toFixed(1) + ' kW)') : '')) : (adv.tii ? 'TII-DB laden / unbekannt' : '-');
    document.getElementById('freqOffset').textContent = (adv.freqOffsetHz != null) ? (adv.freqOffsetHz/1000).toFixed(2) + ' kHz' : '-';
    let brText = adv.bitRate ? (adv.bitRate + ' kbps') : '-';
    if (adv.protection) brText += ' · ' + adv.protection;
    if (adv.subChSize) brText += ' · ' + adv.subChSize + ' CU';
    document.getElementById('advBitRate').textContent = brText;
    const stats = adv.decodingStats || {};
    document.getElementById('fibErrors').textContent = (stats.fibErrorCntr != null) ? (stats.fibErrorCntr + ' / ' + stats.fibCntr) : '-';
    document.getElementById('mscCrcErrors').textContent = (stats.mscCrcErrorCntr != null) ? stats.mscCrcErrorCntr : '-';
    document.getElementById('rsBitErrors').textContent = (stats.rsBitErrorCntr != null) ? stats.rsBitErrorCntr : '-';
    document.getElementById('rsUncorrectable').textContent = (stats.rsUncorrectableCntr != null) ? stats.rsUncorrectableCntr : '-';

    const tiiMulti = document.getElementById('tiiMulti');
    if (tiiMulti) {
      tiiMulti.textContent = tiiList.length
        ? ('Empfangene TII (' + tiiList.length + '): ' + tiiList.map(x => x.code + (x.location ? (' ' + x.location) : '') + ' (' + Number(x.levelDb || 0).toFixed(1) + ' dB)').join(' | '))
        : 'TII-Empfang: Noch kein TII-Signal dekodiert';
    }
    const db = s.tiiDb || {};
    const dbEl = document.getElementById('tiiDbInfo');
    const dbBtn = document.getElementById('tiiUpdateBtn');
    if (dbEl) {
      if (!db.supported) {
        dbEl.textContent = 'TII-Datenbank: FMLIST-Bibliothek nicht einkompiliert';
      } else if (db.updating) {
        dbEl.textContent = 'TII-Datenbank: Wird heruntergeladen...';
      } else if (db.loaded) {
        dbEl.textContent = 'TII-Datenbank: ' + db.count + ' Sender' + (db.lastUpdate ? (' (Stand: ' + db.lastUpdate + ')') : '');
      } else {
        dbEl.textContent = 'TII-Datenbank: Nicht geladen' + (db.status ? (' (' + db.status + ')') : '');
      }
    }
    if (dbBtn) {
      dbBtn.disabled = Boolean(!db.supported || db.updating);
      dbBtn.textContent = db.updating ? 'Lade...' : (db.loaded ? 'TII-DB aktualisieren' : 'TII-DB laden');
    }

    renderCuLayout(s);

    const slideImg = document.getElementById('slide');
    const slideVersion = (s.current && s.current.slideVersion) || 0;
    if (slideVersion && slideVersion !== lastSlideVersion) {
      lastSlideVersion = slideVersion;
      slideImg.src = '/api/slideshow?v=' + slideVersion;
      slideImg.classList.add('visible');
    } else if (!slideVersion) {
      lastSlideVersion = 0;
      slideImg.classList.remove('visible');
      slideImg.removeAttribute('src');
    }

    const streamObj = s.stream || {};
    const streamCard = document.getElementById('streamCard');
    const streamSlideImg = document.getElementById('streamSlide');
    if (streamCard) {
      if (streamObj.decoupled) {
        streamCard.style.display = 'flex';
        document.getElementById('streamLabel').textContent = streamObj.label || '-';
        let sTitle = '-';
        if (streamObj.dlPlusTitle) {
          sTitle = streamObj.dlPlusArtist ? (streamObj.dlPlusArtist + ' - ' + streamObj.dlPlusTitle) : streamObj.dlPlusTitle;
        }
        document.getElementById('streamTitle').textContent = sTitle;
        document.getElementById('streamDls').textContent = (streamObj.dls && streamObj.dls.trim()) ? streamObj.dls : '-';
        const sSlideVer = streamObj.slideVersion || 0;
        if (streamSlideImg) {
          if (sSlideVer && sSlideVer !== lastStreamSlideVersion) {
            lastStreamSlideVersion = sSlideVer;
            streamSlideImg.src = '/api/stream/slideshow?v=' + sSlideVer;
            streamSlideImg.style.display = 'block';
          } else if (!sSlideVer) {
            lastStreamSlideVersion = 0;
            streamSlideImg.style.display = 'none';
            streamSlideImg.removeAttribute('src');
          }
        }
      } else {
        streamCard.style.display = 'none';
        lastStreamSlideVersion = 0;
        if (streamSlideImg) {
          streamSlideImg.style.display = 'none';
          streamSlideImg.removeAttribute('src');
        }
      }
    }

    if ('mediaSession' in navigator) {
      const metaSrc = (streamObj.decoupled && streamObj.sid) ? streamObj : s.current;
      if (metaSrc) {
        const artist = metaSrc.dlPlusArtist || '';
        const title = metaSrc.dlPlusTitle || metaSrc.dls || metaSrc.label || '';
        const sVer = metaSrc.slideVersion || 0;
        const slidePath = (metaSrc === streamObj) ? '/api/stream/slideshow?v=' : '/api/slideshow?v=';
        const artwork = sVer ? [{src: slidePath + sVer}] : [];
        navigator.mediaSession.metadata = new MediaMetadata({
          title: title,
          artist: artist,
          album: (s.ensemble && s.ensemble.label) || '',
          artwork: artwork
        });
      }
    }

    if (browserStreaming) {
      if (streamObj.decoupled && streamObj.sid) {
        browserPlayingKey = svcKey(streamObj.sid, streamObj.scids);
        if (typeof lastSid !== 'undefined') {
          lastSid = streamObj.sid;
          lastScids = Number(streamObj.scids || 0);
          lastCh = s.channel || '';
        }
      } else if (s && s.current && s.current.playing && s.current.sid) {
        browserPlayingKey = svcKey(s.current.sid, s.current.scids);
        if (typeof lastSid !== 'undefined') {
          lastSid = s.current.sid;
          lastScids = Number(s.current.scids || 0);
          lastCh = s.channel || '';
        }
      } else {
        browserPlayingKey = '';
      }
    }
    const pSt = document.getElementById('playerStatus');
    if (pSt) {
      if (browserStreaming) {
        pSt.textContent = streamObj.decoupled
          ? ('Playing (entkoppelt): ' + (streamObj.label || '...'))
          : ('Playing: ' + ((s && s.current && s.current.label) ? s.current.label : '...'));
      } else {
        pSt.textContent = 'Not playing';
      }
    }

    const epgBtn = document.getElementById('epgToggleBtn');
    if (epgBtn) {
      if (s && s.hasEpg) {
        epgBtn.style.display = '';
      } else {
        epgBtn.style.display = 'none';
        if (epgOpen) toggleEpgView(false);
      }
    }
    if (epgOpen && Date.now() - epgLastFetch > 30000) {
      loadEpg(false);
    }

    renderServicesTable(s);
    if (!scanning && Date.now() - lastListRefresh > 6000) {
      lastListRefresh = Date.now();
      loadStationList();
    } else if (scannedLoaded) {
      renderStationList();
    }
  } catch (e) { console.error(e); }
}
const EPG_PX_PER_MIN = 3;
let epgOpen = false;
let epgData = null;
let epgLastFetch = 0;
let epgDragged = false;
let epgSelectedKey = '';

function clearEpgSelection() {
  epgSelectedKey = '';
  const sc = document.getElementById('epgScroll');
  if (sc) sc.querySelectorAll('.epgProg.selected').forEach(el => el.classList.remove('selected'));
  const box = document.getElementById('epgDetail');
  if (box) {
    box.classList.remove('visible');
    box.innerHTML = '';
  }
}

function toggleEpgView(forceState) {
  const layout = document.getElementById('mainLayout');
  const btn = document.getElementById('epgToggleBtn');
  epgOpen = (typeof forceState === 'boolean') ? forceState : !epgOpen;
  if (epgOpen) {
    if (layout) layout.classList.add('epgOpen');
    if (btn) { btn.textContent = 'Zurück'; btn.classList.add('tuned'); }
    loadEpg(true);
  } else {
    if (layout) layout.classList.remove('epgOpen');
    if (btn) { btn.textContent = 'EPG'; btn.classList.remove('tuned'); }
    clearEpgSelection();
  }
}

async function loadEpg(autoScroll) {
  try {
    epgLastFetch = Date.now();
    const r = await fetch('/api/epg');
    const j = await r.json();
    epgData = j;
    renderEpg(j, autoScroll);
  } catch (e) { console.error(e); }
}

function updateEpgDayBadge() {
  const sc = document.getElementById('epgScroll');
  const badge = document.getElementById('epgDayBadge');
  if (!sc || !badge || !epgData) return;
  const midnightX = 1440 * EPG_PX_PER_MIN;
  const viewTrackW = Math.max(0, sc.clientWidth - 155);
  const threshold = midnightX - viewTrackW * 0.35;
  badge.textContent = (sc.scrollLeft >= threshold)
    ? (epgData.tomorrowLabel || 'Morgen')
    : (epgData.todayLabel || 'Heute');
}

function scrollEpgTo(target) {
  clearEpgSelection();
  const sc = document.getElementById('epgScroll');
  if (!sc) return;
  let x = 0;
  if (target === 'tomorrow') {
    x = 1440 * EPG_PX_PER_MIN;
  } else if (target === 'now' && epgData && typeof epgData.nowMinute === 'number') {
    x = Math.max(0, epgData.nowMinute * EPG_PX_PER_MIN - Math.max(100, (sc.clientWidth - 155) * 0.25));
  }
  sc.scrollTo({ left: x, behavior: 'smooth' });
  setTimeout(updateEpgDayBadge, 180);
}

function showEpgDetail(svcLabel, prog) {
  const box = document.getElementById('epgDetail');
  if (!box) return;
  box.innerHTML = '';
  const head = document.createElement('div');
  head.style.cssText = 'display:flex; justify-content:space-between; align-items:center; gap:.5rem; flex-wrap:wrap; margin-bottom:.35rem;';
  const titleEl = document.createElement('strong');
  titleEl.textContent = (svcLabel ? svcLabel + ' · ' : '') + (prog.title || '');
  const timeEl = document.createElement('span');
  timeEl.className = 'muted';
  timeEl.textContent = (prog.dayLabel ? prog.dayLabel + ', ' : '') + prog.startStr + ' – ' + prog.endStr;
  head.appendChild(titleEl);
  head.appendChild(timeEl);
  box.appendChild(head);
  const descEl = document.createElement('div');
  descEl.className = 'muted';
  descEl.style.color = '#d1d5db';
  descEl.textContent = prog.desc || 'Keine Beschreibung vorhanden.';
  box.appendChild(descEl);
  box.classList.add('visible');
}

function renderEpg(data, autoScroll) {
  const sc = document.getElementById('epgScroll');
  if (!sc) return;
  const prevScrollLeft = sc.scrollLeft;
  const prevScrollTop = sc.scrollTop;
  sc.innerHTML = '';
  if (!data || !data.available || !Array.isArray(data.services) || !data.services.length) {
    sc.innerHTML = '<div class="muted" style="padding:1rem;">Aktuell keine EPG-Daten für heute oder morgen vorhanden.</div>';
    return;
  }

  const grid = document.createElement('div');
  grid.className = 'epgGrid';

  const hdr = document.createElement('div');
  hdr.className = 'epgHeaderRow';
  const corner = document.createElement('div');
  corner.className = 'epgCorner';
  corner.textContent = 'Sender';
  hdr.appendChild(corner);

  const ruler = document.createElement('div');
  ruler.className = 'epgRuler';
  for (let h = 0; h < 48; h++) {
    const tick = document.createElement('div');
    tick.className = 'epgTick' + (h === 24 ? ' midnight' : '');
    tick.style.left = (h * 60 * EPG_PX_PER_MIN) + 'px';
    const hh = String(h % 24).padStart(2, '0') + ':00';
    tick.textContent = (h === 24) ? ('00:00 · ' + (data.tomorrowShort || 'Morgen')) : hh;
    ruler.appendChild(tick);
  }
  if (typeof data.nowMinute === 'number') {
    const dot = document.createElement('div');
    dot.className = 'epgNowDot';
    dot.style.left = (data.nowMinute * EPG_PX_PER_MIN) + 'px';
    ruler.appendChild(dot);
  }
  hdr.appendChild(ruler);
  grid.appendChild(hdr);

  let curRowEl = null;
  data.services.forEach(svc => {
    const row = document.createElement('div');
    row.className = 'epgRow' + (svc.isCurrent ? ' epgCurRow' : '');
    if (svc.isCurrent) curRowEl = row;

    const svcCell = document.createElement('div');
    svcCell.className = 'epgSvc';
    svcCell.textContent = svc.label || svc.sid;
    svcCell.title = svc.label || svc.sid;
    row.appendChild(svcCell);

    const track = document.createElement('div');
    track.className = 'epgTrack';
    (svc.programs || []).forEach(pr => {
      const leftPx = pr.startMin * EPG_PX_PER_MIN;
      const widthPx = Math.max(22, (pr.endMin - pr.startMin) * EPG_PX_PER_MIN - 2);
      const pKey = svc.sid + ':' + pr.startMin;
      const item = document.createElement('div');
      item.className = 'epgProg' + (pr.isNow ? ' isNow' : '') + (pKey === epgSelectedKey ? ' selected' : '');
      item.style.left = leftPx + 'px';
      item.style.width = widthPx + 'px';
      item.title = pr.startStr + '–' + pr.endStr + ' · ' + pr.title;

      const tEl = document.createElement('div');
      tEl.className = 'epgProgTitle';
      tEl.textContent = pr.title;
      const tmEl = document.createElement('div');
      tmEl.className = 'epgProgTime';
      tmEl.textContent = pr.startStr + '–' + pr.endStr;
      item.appendChild(tEl);
      item.appendChild(tmEl);

      item.onclick = () => {
        if (epgDragged) return;
        if (epgSelectedKey === pKey) {
          clearEpgSelection();
          return;
        }
        epgSelectedKey = pKey;
        sc.querySelectorAll('.epgProg.selected').forEach(el => el.classList.remove('selected'));
        item.classList.add('selected');
        showEpgDetail(svc.label || svc.sid, pr);
      };
      track.appendChild(item);
    });
    row.appendChild(track);
    grid.appendChild(row);
  });

  if (typeof data.nowMinute === 'number') {
    const nowLine = document.createElement('div');
    nowLine.className = 'epgNowLine';
    nowLine.style.left = (155 + data.nowMinute * EPG_PX_PER_MIN) + 'px';
    grid.appendChild(nowLine);
  }

  sc.appendChild(grid);

  if (autoScroll) {
    const targetX = (typeof data.nowMinute === 'number')
      ? Math.max(0, data.nowMinute * EPG_PX_PER_MIN - Math.max(100, (sc.clientWidth - 155) * 0.25))
      : 0;
    sc.scrollLeft = targetX;
    if (curRowEl) {
      sc.scrollTop = Math.max(0, curRowEl.offsetTop - sc.clientHeight * 0.35);
    }
  } else {
    sc.scrollLeft = prevScrollLeft;
    sc.scrollTop = prevScrollTop;
  }
  updateEpgDayBadge();
}

(function initEpgScrollHandlers(){
  const sc = document.getElementById('epgScroll');
  if (!sc) return;
  let down = false, startX = 0, startLeft = 0;
  sc.addEventListener('scroll', updateEpgDayBadge, { passive: true });
  sc.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    down = true;
    epgDragged = false;
    startX = e.clientX;
    startLeft = sc.scrollLeft;
    sc.classList.add('dragging');
  });
  window.addEventListener('mousemove', (e) => {
    if (!down) return;
    const dx = e.clientX - startX;
    if (Math.abs(dx) > 5) epgDragged = true;
    if (epgDragged) sc.scrollLeft = startLeft - dx;
  });
  window.addEventListener('mouseup', () => {
    if (!down) return;
    down = false;
    sc.classList.remove('dragging');
    setTimeout(() => { epgDragged = false; }, 0);
  });
  sc.addEventListener('wheel', (e) => {
    if (sc.scrollHeight <= sc.clientHeight && Math.abs(e.deltaY) > Math.abs(e.deltaX)) {
      e.preventDefault();
      sc.scrollLeft += e.deltaY;
    }
  }, { passive: false });
})();

loadChannels().then(refresh);
loadStationList();
setInterval(refresh, 1500);
</script>
</body>
</html>
)HTML");

    return html;
}
