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

#include "dabcliapp.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <algorithm>

#include "audiorecorder.h"
#include "config.h"
#include "dabtables.h"
#include "rtlsdrinput.h"
#include "rtltcpinput.h"
#include "webserver.h"

#if HAVE_FAAD
#include "audiodecoderfaad.h"
#endif
#if HAVE_FDKAAC
#include "audiodecoderfdkaac.h"
#endif
#if defined(Q_OS_LINUX) && HAVE_LINUX_DBUS
#include "linux.h"
#endif

namespace
{
// English labels for the DL Plus content types worth surfacing outside of ITEM_TITLE/ITEM_ARTIST
// (handled separately); mirrors DLPlusModelItem::typeToLabel() in dlplusmodel.cpp. Empty return
// means "not shown" (DUMMY, RFU/reserved and receiver-only hint types).
QString dlPlusContentTypeLabel(DLPlusContentType type)
{
    switch (type)
    {
        case DLPlusContentType::ITEM_ALBUM:
            return QStringLiteral("Album");
        case DLPlusContentType::ITEM_TRACKNUMBER:
            return QStringLiteral("Track Number");
        case DLPlusContentType::ITEM_COMPOSITION:
            return QStringLiteral("Composition");
        case DLPlusContentType::ITEM_MOVEMENT:
            return QStringLiteral("Movement");
        case DLPlusContentType::ITEM_CONDUCTOR:
            return QStringLiteral("Conductor");
        case DLPlusContentType::ITEM_COMPOSER:
            return QStringLiteral("Composer");
        case DLPlusContentType::ITEM_BAND:
            return QStringLiteral("Band");
        case DLPlusContentType::ITEM_COMMENT:
            return QStringLiteral("Comment");
        case DLPlusContentType::ITEM_GENRE:
            return QStringLiteral("Genre");
        case DLPlusContentType::INFO_NEWS:
            return QStringLiteral("News");
        case DLPlusContentType::INFO_NEWS_LOCAL:
            return QStringLiteral("News (local)");
        case DLPlusContentType::INFO_STOCKMARKET:
            return QStringLiteral("Stock Market");
        case DLPlusContentType::INFO_SPORT:
            return QStringLiteral("Sport");
        case DLPlusContentType::INFO_LOTTERY:
            return QStringLiteral("Lottery");
        case DLPlusContentType::INFO_HOROSCOPE:
            return QStringLiteral("Horoscope");
        case DLPlusContentType::INFO_DAILY_DIVERSION:
            return QStringLiteral("Daily Diversion");
        case DLPlusContentType::INFO_HEALTH:
            return QStringLiteral("Health");
        case DLPlusContentType::INFO_EVENT:
            return QStringLiteral("Event");
        case DLPlusContentType::INFO_SCENE:
            return QStringLiteral("Scene");
        case DLPlusContentType::INFO_CINEMA:
            return QStringLiteral("Cinema");
        case DLPlusContentType::INFO_TV:
            return QStringLiteral("TV");
        case DLPlusContentType::INFO_WEATHER:
            return QStringLiteral("Weather");
        case DLPlusContentType::INFO_TRAFFIC:
            return QStringLiteral("Traffic");
        case DLPlusContentType::INFO_ALARM:
            return QStringLiteral("Alarm");
        case DLPlusContentType::INFO_ADVERTISEMENT:
            return QStringLiteral("Advertisment");
        case DLPlusContentType::INFO_URL:
            return QStringLiteral("URL");
        case DLPlusContentType::INFO_OTHER:
            return QStringLiteral("Other");
        case DLPlusContentType::STATIONNAME_SHORT:
            return QStringLiteral("Station (short)");
        case DLPlusContentType::STATIONNAME_LONG:
            return QStringLiteral("Station");
        case DLPlusContentType::PROGRAMME_NOW:
            return QStringLiteral("Now");
        case DLPlusContentType::PROGRAMME_NEXT:
            return QStringLiteral("Next");
        case DLPlusContentType::PROGRAMME_PART:
            return QStringLiteral("Programme Part");
        case DLPlusContentType::PROGRAMME_HOST:
            return QStringLiteral("Host");
        case DLPlusContentType::PROGRAMME_EDITORIAL_STAFF:
            return QStringLiteral("Editorial");
        case DLPlusContentType::PROGRAMME_HOMEPAGE:
            return QStringLiteral("Homepage");
        case DLPlusContentType::PHONE_HOTLINE:
            return QStringLiteral("Phone (Hotline)");
        case DLPlusContentType::PHONE_STUDIO:
            return QStringLiteral("Phone (Studio)");
        case DLPlusContentType::PHONE_OTHER:
            return QStringLiteral("Phone (Other)");
        case DLPlusContentType::SMS_STUDIO:
            return QStringLiteral("SMS (Studio)");
        case DLPlusContentType::SMS_OTHER:
            return QStringLiteral("SMS (Other)");
        case DLPlusContentType::EMAIL_HOTLINE:
            return QStringLiteral("E-mail (Hotline)");
        case DLPlusContentType::EMAIL_STUDIO:
            return QStringLiteral("E-mail (Studio)");
        case DLPlusContentType::EMAIL_OTHER:
            return QStringLiteral("E-mail (Other)");
        case DLPlusContentType::MMS_OTHER:
            return QStringLiteral("MMS");
        case DLPlusContentType::CHAT:
            return QStringLiteral("Chat Message");
        case DLPlusContentType::CHAT_CENTER:
            return QStringLiteral("Chat");
        case DLPlusContentType::VOTE_QUESTION:
            return QStringLiteral("Vote Question");
        case DLPlusContentType::VOTE_CENTRE:
            return QStringLiteral("Vote Here");
        default:
            return QString();
    }
}
}  // namespace

Q_LOGGING_CATEGORY(cliApp, "DabCli", QtInfoMsg)

DabCliApp::DabCliApp(const CliConfig &config, QObject *parent) : QObject(parent), m_config(config) {}

DabCliApp::~DabCliApp()
{
    // Stop worker threads first, before destroying the input device.
    if (nullptr != m_radioControlThread)
    {
        if (nullptr != m_radioControl)
        {
            m_radioControl->exit();
        }
        m_radioControlThread->quit();  // this deletes m_radioControl (and stops the dabsdr thread)
        m_radioControlThread->wait();
    }
    if (nullptr != m_audioDecoderThread)
    {
        m_audioDecoderThread->quit();
        m_audioDecoderThread->wait();
    }
    if (nullptr != m_streamAudioDecoderThread)
    {
        m_streamAudioDecoderThread->quit();
        m_streamAudioDecoderThread->wait();
    }
    saveEpgCache(true);
    if (nullptr != m_inputDevice)
    {
        delete m_inputDevice;
    }
#if HAVE_FMLIST_INTERFACE
    delete m_fmlistInterface;
    m_fmlistInterface = nullptr;
#endif
    qDeleteAll(m_txTable);
    m_txTable.clear();

#if defined(Q_OS_LINUX) && HAVE_LINUX_DBUS
    linuxTeardownMediaRemoteCommands();
#endif
}

bool DabCliApp::start()
{
    // RadioControl runs in its own thread, just like in the GUI application
    m_radioControl = new RadioControl();
    m_radioControlThread = new QThread(this);
    m_radioControlThread->setObjectName("radioControlThr");
    m_radioControl->moveToThread(m_radioControlThread);
    connect(m_radioControlThread, &QThread::finished, m_radioControl, &QObject::deleteLater);
    m_radioControlThread->start();

    if (!m_radioControl->init())
    {
        qCCritical(cliApp) << "RadioControl init failed";
        return false;
    }

    m_audioRecorder = new AudioRecorder();

#if HAVE_FDKAAC
    m_audioDecoder = new AudioDecoderFDKAAC(m_audioRecorder);
#elif HAVE_FAAD
    m_audioDecoder = new AudioDecoderFAAD(m_audioRecorder);
#else
#error "Either FAAD or FDK-AAC decoder must be enabled"
#endif
    m_audioDecoderThread = new QThread(this);
    m_audioDecoderThread->setObjectName("audioDecoderThr");
    m_audioDecoder->moveToThread(m_audioDecoderThread);
    m_audioRecorder->moveToThread(m_audioDecoderThread);
    connect(m_audioDecoderThread, &QThread::finished, m_audioDecoder, &QObject::deleteLater);
    connect(m_audioDecoderThread, &QThread::finished, m_audioRecorder, &QObject::deleteLater);
    m_audioDecoderThread->start();

    // RadioControl <-> AudioDecoder wiring (mirrors Application::Application())
    connect(m_radioControl, &RadioControl::audioServiceSelection, m_audioDecoder, &AudioDecoder::start, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::audioData, m_audioDecoder, &AudioDecoder::decodeData, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::stopAudio, m_audioDecoder, &AudioDecoder::stop, Qt::QueuedConnection);
    connect(m_audioDecoder, &AudioDecoder::audioParametersInfo, this, &DabCliApp::onAudioParametersInfo, Qt::QueuedConnection);

    m_streamAudioRecorder = new AudioRecorder();
#if HAVE_FDKAAC
    m_streamAudioDecoder = new AudioDecoderFDKAAC(m_streamAudioRecorder);
#elif HAVE_FAAD
    m_streamAudioDecoder = new AudioDecoderFAAD(m_streamAudioRecorder);
#else
#error "Either FAAD or FDK-AAC decoder must be enabled"
#endif
    m_streamAudioDecoderThread = new QThread(this);
    m_streamAudioDecoderThread->setObjectName("streamAudioDecThr");
    m_streamAudioDecoder->moveToThread(m_streamAudioDecoderThread);
    m_streamAudioRecorder->moveToThread(m_streamAudioDecoderThread);
    connect(m_streamAudioDecoderThread, &QThread::finished, m_streamAudioDecoder, &QObject::deleteLater);
    connect(m_streamAudioDecoderThread, &QThread::finished, m_streamAudioRecorder, &QObject::deleteLater);
    m_streamAudioDecoderThread->start();
    connect(m_radioControl, &RadioControl::secondaryAudioServiceSelection, m_streamAudioDecoder, &AudioDecoder::start, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::secondaryAudioData, m_streamAudioDecoder, &AudioDecoder::decodeData, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::stopSecondaryAudio, m_streamAudioDecoder, &AudioDecoder::stop, Qt::QueuedConnection);
    connect(m_streamAudioDecoder, &AudioDecoder::audioParametersInfo, this, &DabCliApp::onStreamAudioParametersInfo, Qt::QueuedConnection);

#if HAVE_PORTAUDIO
    m_audioOutput = new CliAudioOutput(this);
    connect(m_audioDecoder, &AudioDecoder::startAudio, m_audioOutput, &CliAudioOutput::onStartAudio, Qt::QueuedConnection);
    connect(m_audioDecoder, &AudioDecoder::switchAudio, m_audioOutput, &CliAudioOutput::onSwitchAudio, Qt::QueuedConnection);
    connect(m_audioDecoder, &AudioDecoder::stopAudio, m_audioOutput, &CliAudioOutput::onStopAudio, Qt::QueuedConnection);
#endif

    if (m_config.webUiEnabled)
    {
        m_audioStreamer = new AudioStreamer(this);
        connect(m_streamAudioDecoder, &AudioDecoder::startAudio, m_audioStreamer, &AudioStreamer::onStartAudio, Qt::QueuedConnection);
        connect(m_streamAudioDecoder, &AudioDecoder::switchAudio, m_audioStreamer, &AudioStreamer::onSwitchAudio, Qt::QueuedConnection);
        connect(m_streamAudioDecoder, &AudioDecoder::stopAudio, m_audioStreamer, &AudioStreamer::onStopAudio, Qt::QueuedConnection);
#if HAVE_PORTAUDIO
        connect(m_audioOutput, &CliAudioOutput::audioChunk, this, [this](const QByteArray &pcm, int sr, int ch) {
            bool decoupled = false;
            {
                QMutexLocker lock(&m_mutex);
                decoupled = m_streamDecoupled;
            }
            if (!decoupled && m_audioStreamer)
            {
                emit m_audioStreamer->audioChunk(pcm, sr, ch);
            }
        });
#else
        connect(m_audioDecoder, &AudioDecoder::startAudio, this, [this](audioFifo_t *buf) {
            bool decoupled = false;
            { QMutexLocker lock(&m_mutex); m_primaryAudioFifo = buf; decoupled = m_streamDecoupled; }
            if (!decoupled && m_audioStreamer) m_audioStreamer->onStartAudio(buf);
        }, Qt::QueuedConnection);
        connect(m_audioDecoder, &AudioDecoder::switchAudio, this, [this](audioFifo_t *buf) {
            bool decoupled = false;
            { QMutexLocker lock(&m_mutex); m_primaryAudioFifo = buf; decoupled = m_streamDecoupled; }
            if (!decoupled && m_audioStreamer) m_audioStreamer->onSwitchAudio(buf);
        }, Qt::QueuedConnection);
        connect(m_audioDecoder, &AudioDecoder::stopAudio, this, [this]() {
            bool decoupled = false;
            { QMutexLocker lock(&m_mutex); m_primaryAudioFifo = nullptr; decoupled = m_streamDecoupled; }
            if (!decoupled && m_audioStreamer) m_audioStreamer->onStopAudio();
        }, Qt::QueuedConnection);
#endif
    }

    connect(m_radioControl, &RadioControl::ensembleInformation, this, &DabCliApp::onEnsembleInformation, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::ensembleReconfiguration, this, &DabCliApp::onEnsembleInformation, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::serviceListEntry, this, &DabCliApp::onServiceListEntry, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::serviceListComplete, this, &DabCliApp::onServiceListComplete, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::signalState, this, &DabCliApp::onSignalState, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::dabTime, this, &DabCliApp::onDabTime, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::tiiData, this, &DabCliApp::onTiiData, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::secondaryAudioServiceSelection, this, &DabCliApp::onSecondaryAudioServiceSelection, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::secondaryStreamStopped, this, &DabCliApp::onSecondaryStreamStopped, Qt::QueuedConnection);
    connect(this, &DabCliApp::startTiiRequested, m_radioControl, &RadioControl::startTii, Qt::QueuedConnection);
    connect(this, &DabCliApp::tuneSecondaryServiceRequested, m_radioControl, &RadioControl::tuneSecondaryService, Qt::QueuedConnection);
    connect(this, &DabCliApp::stopSecondaryServiceRequested, m_radioControl, &RadioControl::stopSecondaryService, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::tuneDone, this, &DabCliApp::onTuneDone, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::audioServiceSelection, this, &DabCliApp::onAudioServiceSelection, Qt::QueuedConnection);

    // Dynamic label (DLS) + DL Plus (title/artist/...) decoding, mirrors Application::Application()
    m_dlDecoder = new DLDecoder(this);
    connect(m_radioControl, &RadioControl::dlDataGroup_Service, m_dlDecoder, &DLDecoder::newDataGroup, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::audioServiceSelection, m_dlDecoder, &DLDecoder::reset, Qt::QueuedConnection);
    connect(m_dlDecoder, &DLDecoder::dlComplete, this, &DabCliApp::onDlComplete, Qt::QueuedConnection);
    connect(m_dlDecoder, &DLDecoder::dlPlusObject, this, &DabCliApp::onDlPlusObject, Qt::QueuedConnection);
    connect(m_dlDecoder, &DLDecoder::dlItemToggle, this, &DabCliApp::onDlItemToggle, Qt::QueuedConnection);
    connect(m_dlDecoder, &DLDecoder::dlItemRunning, this, &DabCliApp::onDlItemRunning, Qt::QueuedConnection);
    connect(m_dlDecoder, &DLDecoder::resetTerminal, this, &DabCliApp::onDlReset, Qt::QueuedConnection);
    m_streamDlDecoder = new DLDecoder(this);
    connect(m_radioControl, &RadioControl::dlDataGroup_Announcement, m_streamDlDecoder, &DLDecoder::newDataGroup, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::secondaryAudioServiceSelection, m_streamDlDecoder, &DLDecoder::reset, Qt::QueuedConnection);
    connect(m_streamDlDecoder, &DLDecoder::dlComplete, this, &DabCliApp::onStreamDlComplete, Qt::QueuedConnection);
    connect(m_streamDlDecoder, &DLDecoder::dlPlusObject, this, &DabCliApp::onStreamDlPlusObject, Qt::QueuedConnection);
    connect(m_streamDlDecoder, &DLDecoder::dlItemToggle, this, &DabCliApp::onStreamDlItemToggle, Qt::QueuedConnection);
    connect(m_streamDlDecoder, &DLDecoder::dlItemRunning, this, &DabCliApp::onStreamDlItemRunning, Qt::QueuedConnection);
    connect(m_streamDlDecoder, &DLDecoder::resetTerminal, this, &DabCliApp::onStreamDlReset, Qt::QueuedConnection);
    m_streamSlideShowApp = new SlideShowApp();
    m_streamSlideShowApp->moveToThread(m_radioControlThread);
    connect(m_radioControlThread, &QThread::finished, m_streamSlideShowApp, &QObject::deleteLater);
    connect(m_radioControl, &RadioControl::secondaryAudioServiceSelection, m_streamSlideShowApp, &SlideShowApp::start);
    connect(m_radioControl, &RadioControl::userAppData_Announcement, m_streamSlideShowApp, &SlideShowApp::onUserAppData);
    connect(m_radioControl, &RadioControl::ensembleInformation, m_streamSlideShowApp, &UserApplication::setEnsId);
    connect(m_radioControl, &RadioControl::secondaryAudioServiceSelection, m_streamSlideShowApp, &UserApplication::setAudioServiceId);
    connect(m_streamSlideShowApp, &SlideShowApp::currentSlide, this, &DabCliApp::onStreamCurrentSlide, Qt::QueuedConnection);

    // MOT Slideshow (album art / station image), mirrors Application::Application(); lives in the
    // RadioControl thread, only the resulting Slide is delivered (queued) to the main thread.
    m_slideShowApp = new SlideShowApp();
    m_slideShowApp->moveToThread(m_radioControlThread);
    connect(m_radioControlThread, &QThread::finished, m_slideShowApp, &QObject::deleteLater);
    connect(m_radioControl, &RadioControl::audioServiceSelection, m_slideShowApp, &SlideShowApp::start);
    connect(m_radioControl, &RadioControl::userAppData_Service, m_slideShowApp, &SlideShowApp::onUserAppData);
    connect(m_radioControl, &RadioControl::ensembleInformation, m_slideShowApp, &UserApplication::setEnsId);
    connect(m_radioControl, &RadioControl::audioServiceSelection, m_slideShowApp, &UserApplication::setAudioServiceId);
    connect(m_slideShowApp, &SlideShowApp::currentSlide, this, &DabCliApp::onCurrentSlide, Qt::QueuedConnection);

    // advanced/technical info: frequency offset, RS/FIB/MSC decoding stats
    connect(m_radioControl, &RadioControl::freqOffset, this, &DabCliApp::onFreqOffset, Qt::QueuedConnection);
    connect(m_radioControl, &RadioControl::decodingStats, this, &DabCliApp::onDecodingStats, Qt::QueuedConnection);

    connect(this, &DabCliApp::tuneServiceRequested, m_radioControl, &RadioControl::tuneService, Qt::QueuedConnection);

    m_epgFlushTimer = new QTimer(this);
    m_epgFlushTimer->setSingleShot(true);
    connect(m_epgFlushTimer, &QTimer::timeout, this, &DabCliApp::onEpgFlushTimer);
    loadEpgCache();

    m_spiApp = new SPIApp();
    m_spiApp->moveToThread(m_radioControlThread);
    connect(m_radioControlThread, &QThread::finished, m_spiApp, &QObject::deleteLater);
    connect(m_radioControl, &RadioControl::userAppData_Service, m_spiApp, &SPIApp::onUserAppData);
    connect(m_radioControl, &RadioControl::audioServiceSelection, m_spiApp, &SPIApp::start);
    connect(m_radioControl, &RadioControl::ensembleInformation, m_spiApp, &UserApplication::setEnsId);
    connect(m_radioControl, &RadioControl::audioServiceSelection, m_spiApp, &UserApplication::setAudioServiceId);
    connect(m_spiApp, &SPIApp::xmlDocument, this, &DabCliApp::onSpiXmlDocument, Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_spiApp, [spi = m_spiApp]() {
        spi->onSettingsChanged(true, true);
        spi->enable(true);
    }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_radioControl, [rc = m_radioControl]() {
        rc->onSpiApplicationEnabled(true);
    }, Qt::QueuedConnection);

    TxDataLoader::loadTable(m_txTable);
    m_txDbLastUpdate = TxDataLoader::lastUpdateTime();
    if (!m_txTable.isEmpty())
    {
        m_tiiDbStatus = QString("Geladen (%1 Sender)").arg(m_txTable.size());
    }
#if HAVE_FMLIST_INTERFACE
    m_fmlistInterface = new FMListInterface(QStringLiteral(PROJECT_VER), TxDataLoader::dbfile());
    connect(m_fmlistInterface, &FMListInterface::updateTiiDataFinished, this, [this](QNetworkReply::NetworkError err) {
        QMutexLocker lock(&m_mutex);
        m_tiiDbUpdating = false;
        if (QNetworkReply::NoError == err)
        {
            TxDataLoader::loadTable(m_txTable);
            m_txDbLastUpdate = TxDataLoader::lastUpdateTime();
            m_tiiDbStatus = QString("Geladen (%1 Sender)").arg(m_txTable.size());
            qCInfo(cliApp) << "TII database updated:" << m_txTable.size() << "entries";
        }
        else
        {
            m_tiiDbStatus = QString("Download-Fehler (%1)").arg(int(err));
            qCWarning(cliApp) << "TII database update failed:" << int(err);
        }
    });
#endif

    if (!openInputDevice())
    {
        return false;
    }
    emit startTiiRequested(true);

    if (m_config.webUiEnabled)
    {
        m_webServer = new WebServer(this, m_audioStreamer, this);
        if (!m_webServer->listen(m_config.bindAddress, m_config.webPort))
        {
            qCCritical(cliApp) << "Failed to start web server on port" << m_config.webPort;
            return false;
        }
        qCInfo(cliApp).noquote() << QString("Web UI listening on http://%1:%2/")
                                        .arg(m_config.bindAddress == QHostAddress::AnyIPv4 ? "0.0.0.0" : m_config.bindAddress.toString())
                                        .arg(m_config.webPort);
    }

    // initial tune, if requested on the command line
    uint32_t freq = m_config.frequencyKHz;
    if (!m_config.channel.isEmpty())
    {
        QString err;
        if (!requestTuneChannel(m_config.channel, &err))
        {
            qCCritical(cliApp).noquote() << err;
            return false;
        }
    }
    else if (freq != 0)
    {
        QString err;
        if (!requestTuneFrequency(freq, &err))
        {
            qCCritical(cliApp).noquote() << err;
            return false;
        }
    }
    else
    {
        qCInfo(cliApp) << "No channel specified, waiting for tune request";
    }

#if defined(Q_OS_LINUX) && HAVE_LINUX_DBUS
    // exposes "now playing" title/artist/state to desktop
    linuxSetupMediaRemoteCommands(this);
    linuxUpdateNowPlayingPlaybackState(m_volumePercent > 0);
#endif

    return true;
}

bool DabCliApp::openInputDevice()
{
    switch (m_config.deviceType)
    {
        case CliDeviceType::RTLSDR:
        {
            RtlSdrInput *dev = new RtlSdrInput();
            m_inputDevice = dev;

            connect(m_radioControl, &RadioControl::tuneInputDevice, m_inputDevice, &InputDevice::tune, Qt::QueuedConnection);
            connect(m_inputDevice, &InputDevice::tuned, m_radioControl, &RadioControl::start, Qt::QueuedConnection);
            connect(m_inputDevice, &InputDevice::deviceReady, this, &DabCliApp::onInputDeviceReady, Qt::QueuedConnection);
            connect(m_inputDevice, &InputDevice::error, this, &DabCliApp::onInputDeviceError, Qt::QueuedConnection);
            connect(m_inputDevice, &InputDevice::rfLevel, this, &DabCliApp::onRfLevel, Qt::QueuedConnection);

            QVariant hwId = m_config.deviceArg.isEmpty() ? QVariant() : QVariant(m_config.deviceArg);
            if (!dev->openDevice(hwId, true))
            {
                qCCritical(cliApp) << "Could not open RTL-SDR device";
                return false;
            }

            if (m_config.manualGainIndex >= 0)
            {
                dev->setGainMode(RtlGainMode::Manual, m_config.manualGainIndex);
            }
            else if (m_config.hardwareAgc)
            {
                dev->setGainMode(RtlGainMode::Hardware);
            }
            else
            {
                dev->setGainMode(RtlGainMode::Software);
            }
            dev->setBW(m_config.bandwidthKHz * 1000u);
            break;
        }
        case CliDeviceType::RTLTCP:
        {
            RtlTcpInput *dev = new RtlTcpInput(m_config.rtlTcpNativeSocket);
            m_inputDevice = dev;

            connect(m_radioControl, &RadioControl::tuneInputDevice, m_inputDevice, &InputDevice::tune, Qt::QueuedConnection);
            connect(m_inputDevice, &InputDevice::tuned, m_radioControl, &RadioControl::start, Qt::QueuedConnection);
            connect(m_inputDevice, &InputDevice::deviceReady, this, &DabCliApp::onInputDeviceReady, Qt::QueuedConnection);
            connect(m_inputDevice, &InputDevice::error, this, &DabCliApp::onInputDeviceError, Qt::QueuedConnection);

            dev->setTcpIp(m_config.rtlTcpHost, m_config.rtlTcpPort, false);
            if (!dev->openDevice())
            {
                qCCritical(cliApp) << "Could not connect to rtl_tcp at" << m_config.rtlTcpHost << ":" << m_config.rtlTcpPort;
                return false;
            }
            dev->setBW(m_config.bandwidthKHz * 1000u);
            break;
        }
    }
    return true;
}

void DabCliApp::onInputDeviceReady()
{
    QMutexLocker lock(&m_mutex);
    m_deviceReady = true;
    qCInfo(cliApp) << "Input device ready";
}

void DabCliApp::onInputDeviceError(InputDevice::ErrorCode errCode)
{
    QMutexLocker lock(&m_mutex);
    m_lastDeviceError = QString("Input device error code %1").arg(int(errCode));
    qCCritical(cliApp).noquote() << m_lastDeviceError;
}

bool DabCliApp::requestTuneChannel(const QString &channelLabel, QString *errorOut)
{
    const QString wanted = channelLabel.trimmed().toUpper();
    for (auto it = DabTables::channelList.cbegin(); it != DabTables::channelList.cend(); ++it)
    {
        if (it.value().toUpper() == wanted)
        {
            return requestTuneFrequency(it.key(), errorOut);
        }
    }
    if (nullptr != errorOut)
    {
        *errorOut = QString("Unknown DAB channel '%1'").arg(channelLabel);
    }
    return false;
}

bool DabCliApp::requestTuneFrequency(uint32_t freqKHz, QString *errorOut)
{
    if (0 == freqKHz)
    {
        if (nullptr != errorOut)
        {
            *errorOut = QStringLiteral("Invalid frequency");
        }
        return false;
    }

    {
        QMutexLocker lock(&m_mutex);
        m_services.clear();
        m_currentFrequency = freqKHz;
        m_currentSid = m_config.haveInitialService ? m_config.initialSid : 0;
        m_currentScids = m_config.haveInitialService ? m_config.initialScids : 0;
        m_currentServiceLabel.clear();
        m_isPlaying = false;
        m_haveAudioParams = false;
        m_dabTime = QDateTime();
        m_tiiList.clear();
        resetStreamStateLocked();
    }

    emit tuneServiceRequested(freqKHz, m_config.haveInitialService ? m_config.initialSid : 0,
                               m_config.haveInitialService ? m_config.initialScids : 0);
    return true;
}

bool DabCliApp::requestService(uint32_t sid, uint8_t scids, QString *errorOut)
{
    uint32_t freq;
    {
        QMutexLocker lock(&m_mutex);
        freq = m_currentFrequency;
        if (0 == freq)
        {
            if (nullptr != errorOut)
            {
                *errorOut = QStringLiteral("Not tuned to any channel yet");
            }
            return false;
        }
        if (!m_services.contains(key(sid, scids)))
        {
            if (nullptr != errorOut)
            {
                *errorOut = QStringLiteral("Service not found in current ensemble");
            }
            return false;
        }
    }
    emit tuneServiceRequested(freq, sid, scids);
    return true;
}

void DabCliApp::setVolume(int percent)
{
    percent = std::clamp(percent, 0, 100);
    {
        QMutexLocker lock(&m_mutex);
        m_volumePercent = percent;
    }
#if HAVE_PORTAUDIO
    if (nullptr != m_audioOutput)
    {
        m_audioOutput->setVolume(percent / 100.0f);
    }
#endif
#if defined(Q_OS_LINUX) && HAVE_LINUX_DBUS
    linuxUpdateNowPlayingPlaybackState(percent > 0);
#endif
}

int DabCliApp::volumePercent() const
{
    QMutexLocker lock(&m_mutex);
    return m_volumePercent;
}

void DabCliApp::toggleMute()
{
    int current;
    {
        QMutexLocker lock(&m_mutex);
        current = m_volumePercent;
    }
    if (current > 0)
    {
        m_volumeBeforeMute = current;
        setVolume(0);
    }
    else
    {
        setVolume(m_volumeBeforeMute);
    }
}

void DabCliApp::resolvePendingServiceLabel()
{
    if (m_config.initialServiceLabel.isEmpty())
    {
        return;
    }
    const QString wanted = m_config.initialServiceLabel.trimmed();
    uint32_t sid = 0;
    uint8_t scids = 0;
    uint32_t freq = 0;
    bool found = false;
    {
        QMutexLocker lock(&m_mutex);
        if (m_currentSid != 0)
        {
            // an explicit SId was already requested / selected - nothing to resolve
            return;
        }
        for (auto it = m_services.cbegin(); it != m_services.cend(); ++it)
        {
            if (it->isAudio && it->label.contains(wanted, Qt::CaseInsensitive))
            {
                sid = it->sid;
                scids = it->scids;
                freq = m_currentFrequency;
                found = true;
                break;
            }
        }
    }
    if (found)
    {
        qCInfo(cliApp).noquote() << QString("Selecting service '%1' matching '%2'").arg(sid, 0, 16).arg(wanted);
        emit tuneServiceRequested(freq, sid, scids);
    }
}

void DabCliApp::onEnsembleInformation(const RadioControlEnsemble &ens)
{
    QMutexLocker lock(&m_mutex);
    m_ensemble = ens;
}

void DabCliApp::onServiceListEntry(const RadioControlEnsemble &ens, const RadioControlServiceComponent &s)
{
    QMutexLocker lock(&m_mutex);
    m_ensemble = ens;

    CliServiceInfo info;
    info.sid = s.SId.value();
    info.scids = s.SCIdS;
    info.label = s.label;
    info.subChId = s.SubChId;
    info.startCU = s.SubChAddr;
    info.subChSize = s.SubChSize;
    info.isAudio = s.isAudioService();
    info.bitRate = !s.isDataPacketService() ? s.streamAudioData.bitRate : 0;
    if (s.protection.isEEP())
    {
        if (s.protection.level < DabProtectionLevel::EEP_1B)
            info.protection = QString("EEP %1-A").arg(int(s.protection.level) - int(DabProtectionLevel::EEP_1A) + 1);
        else
            info.protection = QString("EEP %1-B").arg(int(s.protection.level) - int(DabProtectionLevel::EEP_1B) + 1);
    }
    else if (s.protection.level != DabProtectionLevel::PROTECTION_LEVEL_UNDEFINED)
    {
        info.protection = QString("UEP #%1").arg(s.protection.uepIndex);
    }
    if (s.isDataPacketService() || s.isDataStreamService())
        info.codec = QStringLiteral("Data");
    else if (s.streamAudioData.scType == DabAudioDataSCty::DABPLUS_AUDIO)
        info.codec = QStringLiteral("AAC+");
    else if (s.streamAudioData.scType == DabAudioDataSCty::DAB_AUDIO)
        info.codec = QStringLiteral("MP2");
    if (info.isAudio && !info.label.isEmpty())
    {
        m_epgServiceLabels.insert(key(info.sid, info.scids), info.label);
    }
    m_services.insert(key(info.sid, info.scids), info);
}

void DabCliApp::onServiceListComplete(const RadioControlEnsemble &ens)
{
    int numServices;
    {
        QMutexLocker lock(&m_mutex);
        m_ensemble = ens;
        numServices = m_services.size();
    }
    qCInfo(cliApp) << "Service list complete:" << numServices << "service component(s)";

    resolvePendingServiceLabel();
    {
        QMutexLocker lock(&m_mutex);
        requestRadioDnsForEnsembleLocked();
    }
}

void DabCliApp::onSignalState(uint8_t sync, float snr)
{
    QMutexLocker lock(&m_mutex);
    m_syncLevel = sync;
    m_snr = snr;
}

void DabCliApp::onTuneDone(uint32_t freq)
{
    if (nullptr != m_spiApp)
    {
        QMetaObject::invokeMethod(m_spiApp, [spi = m_spiApp, freq]() { if (0 != freq) spi->enable(true); else spi->stop(); }, Qt::QueuedConnection);
    }
    if (m_shutdownRequested && (0 == freq))
    {
        emit readyToQuit();
    }
}

void DabCliApp::requestShutdown()
{
    if (m_shutdownRequested)
    {
        return;
    }
    m_shutdownRequested = true;
    saveEpgCache(true);

    uint32_t freq;
    {
        QMutexLocker lock(&m_mutex);
        freq = m_currentFrequency;
    }

    if (0 == freq)
    {  // never tuned (or already idle) - nothing to wait for
        emit readyToQuit();
        return;
    }

    qCInfo(cliApp) << "Shutting down - stopping DAB processing...";
    emit tuneServiceRequested(0, 0, 0);
}

void DabCliApp::onAudioServiceSelection(const RadioControlServiceComponent &s)
{
    QMutexLocker lock(&m_mutex);
    m_currentSid = s.SId.value();
    m_currentScids = s.SCIdS;
    m_currentServiceLabel = s.label;
    m_isPlaying = true;
    m_haveAudioParams = false;
    m_dlsText.clear();
    m_dlPlusTitle.clear();
    m_dlPlusArtist.clear();
    m_dlPlusTags.clear();
    m_dlItemRunning = true;
    m_slideData.clear();
    m_slideContentType.clear();
    ++m_slideVersion;
    qCInfo(cliApp).noquote() << "Now playing:" << s.label;
#if defined(Q_OS_LINUX) && HAVE_LINUX_DBUS
    linuxUpdateNowPlayingInfo(s.label);
    linuxUpdateNowPlayingSubtitle(QString());
#endif
}

void DabCliApp::onDlComplete(const QString &dl)
{
    QMutexLocker lock(&m_mutex);
    m_dlsText = dl;
#if defined(Q_OS_LINUX) && HAVE_LINUX_DBUS
    linuxUpdateNowPlayingSubtitle(buildCurrentSubtitleLocked());
#endif
}

void DabCliApp::onDlPlusObject(const DLPlusObject &object)
{
    QMutexLocker lock(&m_mutex);
    if (DLPlusContentType::ITEM_TITLE == object.getType())
    {
        m_dlPlusTitle = object.isDelete() ? QString() : object.getTag();
#if defined(Q_OS_LINUX) && HAVE_LINUX_DBUS
        linuxUpdateNowPlayingSubtitle(buildCurrentSubtitleLocked());
#endif
        return;
    }
    if (DLPlusContentType::ITEM_ARTIST == object.getType())
    {
        m_dlPlusArtist = object.isDelete() ? QString() : object.getTag();
#if defined(Q_OS_LINUX) && HAVE_LINUX_DBUS
        linuxUpdateNowPlayingSubtitle(buildCurrentSubtitleLocked());
#endif
        return;
    }
    if (object.isDelete())
    {
        m_dlPlusTags.remove(int(object.getType()));
        return;
    }
    const QString label = dlPlusContentTypeLabel(object.getType());
    if (!label.isEmpty())
    {
        m_dlPlusTags[int(object.getType())] = object.getTag();
    }
}

void DabCliApp::onDlReset()
{
    QMutexLocker lock(&m_mutex);
    m_dlsText.clear();
    m_dlPlusTitle.clear();
    m_dlPlusArtist.clear();
    m_dlPlusTags.clear();
    m_dlItemRunning = true;
#if defined(Q_OS_LINUX) && HAVE_LINUX_DBUS
    linuxUpdateNowPlayingSubtitle(QString());
#endif
}

void DabCliApp::onCurrentSlide(const Slide &slide)
{
    QMutexLocker lock(&m_mutex);
    m_slideData = slide.getRawData();
    const QString &format = slide.getFormat();
    m_slideContentType = format.contains("PNG", Qt::CaseInsensitive) ? QStringLiteral("image/png") : QStringLiteral("image/jpeg");
    ++m_slideVersion;
}

void DabCliApp::onFreqOffset(float offsetHz)
{
    QMutexLocker lock(&m_mutex);
    m_freqOffsetHz = offsetHz;
}

void DabCliApp::onDecodingStats(const RadioControlDecodingStats &stats)
{
    QMutexLocker lock(&m_mutex);
    m_decodingStats = stats;
    m_haveDecodingStats = true;
}

void DabCliApp::onRfLevel(float level, float gain)
{
    QMutexLocker lock(&m_mutex);
    m_rfLevel = level;
    m_gain = gain;
    m_haveRfLevel = true;
}

static int cliParseIsoDurationSec(const QString &dur)
{
    if (!dur.startsWith(QLatin1String("PT"))) return 0;
    int total = 0;
    int val = 0;
    for (int i = 2; i < dur.size(); ++i)
    {
        QChar c = dur.at(i);
        if (c.isDigit())
        {
            val = val * 10 + c.digitValue();
        }
        else if (c == QLatin1Char('H'))
        {
            total += val * 3600;
            val = 0;
        }
        else if (c == QLatin1Char('M'))
        {
            total += val * 60;
            val = 0;
        }
        else if (c == QLatin1Char('S'))
        {
            total += val;
            val = 0;
        }
    }
    return total;
}

static QTimeZone cliDabTz(const QDateTime &dabTime)
{
    return dabTime.isValid() ? QTimeZone::fromSecondsAheadOfUtc(dabTime.offsetFromUtc()) : QTimeZone::systemTimeZone();
}

static bool cliParseDabBearerUri(const QString &uri, uint32_t *sidOut, uint8_t *scidsOut)
{
    if (!uri.startsWith(QLatin1String("dab:"), Qt::CaseInsensitive)) return false;
    QStringList parts = uri.mid(4).split(QLatin1Char('.'));
    if (parts.size() < 4 || parts.at(0).size() != 3) return false;
    bool okSid = false;
    bool okScids = false;
    uint32_t sid = 0;
    if (parts.at(2).size() == 8)
        sid = parts.at(2).toUInt(&okSid, 16);
    else
        sid = (parts.at(0).mid(1) + parts.at(2)).toUInt(&okSid, 16);
    uint8_t scids = uint8_t(parts.at(3).toUInt(&okScids, 10));
    if (!okSid || !okScids || 0 == sid) return false;
    if (sidOut) *sidOut = sid;
    if (scidsOut) *scidsOut = scids;
    return true;
}

void DabCliApp::onAudioParametersInfo(const AudioParameters &params)
{
    QMutexLocker lock(&m_mutex);
    m_audioParams = params;
    m_haveAudioParams = true;
}

void DabCliApp::onDabTime(const QDateTime &d)
{
    QMutexLocker lock(&m_mutex);
    bool wasInvalid = !m_dabTime.isValid();
    QDate prevDate = wasInvalid ? QDate() : m_dabTime.date();
    m_dabTime = d;
    QDate newDate = d.isValid() ? d.date() : QDate();
    if (d.isValid() && (wasInvalid || prevDate != newDate))
    {
        pruneOldEpgLocked();
        requestRadioDnsForEnsembleLocked();
    }
}

QString DabCliApp::epgCachePath() const
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (dir.isEmpty()) dir = QStringLiteral("/tmp/AbracaDABra");
    return dir + QStringLiteral("/cli_epg_cache.json");
}

void DabCliApp::loadEpgCache()
{
    QFile f(epgCachePath());
    if (!f.open(QIODevice::ReadOnly))
    {
        m_epgHasDiskCache = false;
        return;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (QJsonParseError::NoError != err.error || !doc.isObject())
    {
        m_epgHasDiskCache = false;
        return;
    }
    QMutexLocker lock(&m_mutex);
    QJsonObject root = doc.object();
    QDate today = QDate::currentDate();
    qint64 minEndUtc = today.startOfDay().toSecsSinceEpoch();
    qint64 maxStartUtc = today.addDays(2).startOfDay().toSecsSinceEpoch();
    int validCount = 0;

    QJsonObject labelsObj = root.value(QStringLiteral("labels")).toObject();
    for (auto it = labelsObj.constBegin(); it != labelsObj.constEnd(); ++it)
    {
        bool ok = false;
        quint64 k = it.key().toULongLong(&ok);
        if (ok && !it.value().toString().isEmpty())
        {
            m_epgServiceLabels.insert(k, it.value().toString());
        }
    }

    QJsonObject svcsObj = root.value(QStringLiteral("services")).toObject();
    for (auto it = svcsObj.constBegin(); it != svcsObj.constEnd(); ++it)
    {
        bool ok = false;
        quint64 k = it.key().toULongLong(&ok);
        if (!ok) continue;
        QList<CliEpgProgram> list;
        QJsonArray arr = it.value().toArray();
        for (const QJsonValue &v : arr)
        {
            QJsonObject po = v.toObject();
            CliEpgProgram pr;
            pr.startUtc = qint64(po.value(QStringLiteral("s")).toDouble(0));
            pr.durationSec = po.value(QStringLiteral("d")).toInt(0);
            pr.title = po.value(QStringLiteral("t")).toString();
            pr.desc = po.value(QStringLiteral("x")).toString();
            if (pr.startUtc <= 0 || pr.durationSec <= 0 || pr.title.isEmpty()) continue;
            if ((pr.startUtc + pr.durationSec) <= minEndUtc || pr.startUtc >= maxStartUtc) continue;
            list.append(pr);
            ++validCount;
        }
        if (!list.isEmpty())
        {
            std::sort(list.begin(), list.end(), [](const CliEpgProgram &a, const CliEpgProgram &b) {
                return a.startUtc < b.startUtc;
            });
            m_epgPrograms.insert(k, list);
        }
    }
    m_epgHasDiskCache = (validCount > 0);
    if (m_epgHasDiskCache)
    {
        m_epgLastDiskWrite = QFileInfo(epgCachePath()).lastModified().toUTC();
        qCInfo(cliApp) << "EPG cache loaded:" << validCount << "programme(s)";
    }
}

void DabCliApp::saveEpgCache(bool force)
{
    QJsonObject root;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_epgCacheDirty) return;
        Q_UNUSED(force);
        pruneOldEpgLocked();
        QJsonObject labelsObj;
        QJsonObject svcsObj;
        for (auto it = m_epgPrograms.cbegin(); it != m_epgPrograms.cend(); ++it)
        {
            if (it.value().isEmpty()) continue;
            QString kStr = QString::number(it.key());
            QString lbl = m_epgServiceLabels.value(it.key());
            if (!lbl.isEmpty()) labelsObj.insert(kStr, lbl);
            QJsonArray arr;
            for (const CliEpgProgram &pr : it.value())
            {
                QJsonObject po;
                po.insert(QStringLiteral("s"), double(pr.startUtc));
                po.insert(QStringLiteral("d"), pr.durationSec);
                po.insert(QStringLiteral("t"), pr.title);
                if (!pr.desc.isEmpty()) po.insert(QStringLiteral("x"), pr.desc);
                arr.append(po);
            }
            svcsObj.insert(kStr, arr);
        }
        root.insert(QStringLiteral("labels"), labelsObj);
        root.insert(QStringLiteral("services"), svcsObj);
        m_epgCacheDirty = false;
        m_epgHasDiskCache = !svcsObj.isEmpty();
        m_epgLastDiskWrite = QDateTime::currentDateTimeUtc();
    }

    QString path = epgCachePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile sf(path);
    if (sf.open(QIODevice::WriteOnly))
    {
        sf.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        if (sf.commit())
        {
            qCInfo(cliApp) << "EPG cache saved to" << path;
        }
    }
}

void DabCliApp::scheduleEpgFlushLocked()
{
    if ((nullptr == m_epgFlushTimer) || !m_epgCacheDirty) return;
    if (m_epgFlushTimer->isActive()) return;
    int delayMs = 60 * 60 * 1000;
    if (!m_epgHasDiskCache)
    {
        delayMs = 10 * 60 * 1000;
    }
    else if (m_epgLastDiskWrite.isValid())
    {
        qint64 elapsed = m_epgLastDiskWrite.msecsTo(QDateTime::currentDateTimeUtc());
        delayMs = int(qMax<qint64>(1000, qint64(60 * 60 * 1000) - elapsed));
    }
    m_epgFlushTimer->start(delayMs);
}

void DabCliApp::onEpgFlushTimer()
{
    saveEpgCache(false);
}

void DabCliApp::pruneOldEpgLocked()
{
    QTimeZone tz = cliDabTz(m_dabTime);
    QDate today = m_dabTime.isValid() ? m_dabTime.date() : QDate::currentDate();
    qint64 minEndUtc = QDateTime(today, QTime(0, 0), tz).toSecsSinceEpoch();
    qint64 maxStartUtc = QDateTime(today.addDays(2), QTime(0, 0), tz).toSecsSinceEpoch();
    for (auto it = m_epgPrograms.begin(); it != m_epgPrograms.end(); )
    {
        QList<CliEpgProgram> &list = it.value();
        for (int i = list.size() - 1; i >= 0; --i)
        {
            const CliEpgProgram &pr = list.at(i);
            if ((pr.startUtc + pr.durationSec) <= minEndUtc || pr.startUtc >= maxStartUtc)
            {
                list.removeAt(i);
            }
        }
        if (list.isEmpty())
            it = m_epgPrograms.erase(it);
        else
            ++it;
    }
}

void DabCliApp::onSpiXmlDocument(const QString &xmldocument, const QString &scopeId, uint16_t decoderId)
{
    Q_UNUSED(decoderId);
    QMutexLocker lock(&m_mutex);
    if (parseEpgXmlLocked(xmldocument, scopeId))
    {
        m_epgCacheDirty = true;
        scheduleEpgFlushLocked();
    }
}

bool DabCliApp::parseEpgXmlLocked(const QString &xmldocument, const QString &scopeId)
{
    QDomDocument doc;
    if (!doc.setContent(xmldocument)) return false;
    QDomElement root = doc.documentElement();
    QDate today = m_dabTime.isValid() ? m_dabTime.date() : QDate::currentDate();
    QDate tomorrow = today.addDays(1);
    bool changed = false;

    if (QLatin1String("serviceInformation") == root.tagName())
    {
        QDomElement svcsEl = root.firstChildElement(QStringLiteral("services"));
        QDomElement svcEl = svcsEl.firstChildElement(QStringLiteral("service"));
        while (!svcEl.isNull())
        {
            QString label;
            QDomElement c = svcEl.firstChildElement();
            while (!c.isNull())
            {
                if (QLatin1String("longName") == c.tagName() && !c.text().isEmpty())
                    label = c.text();
                else if (QLatin1String("mediumName") == c.tagName() && label.isEmpty() && !c.text().isEmpty())
                    label = c.text();
                else if (QLatin1String("shortName") == c.tagName() && label.isEmpty() && !c.text().isEmpty())
                    label = c.text();
                c = c.nextSiblingElement();
            }
            if (!label.isEmpty())
            {
                QDomElement bearerEl = svcEl.firstChildElement(QStringLiteral("bearer"));
                while (!bearerEl.isNull())
                {
                    uint32_t sid = 0;
                    uint8_t scids = 0;
                    if (cliParseDabBearerUri(bearerEl.attribute(QStringLiteral("id")), &sid, &scids))
                    {
                        quint64 k = key(sid, scids);
                        if (!m_epgServiceLabels.contains(k))
                        {
                            m_epgServiceLabels.insert(k, label);
                        }
                    }
                    bearerEl = bearerEl.nextSiblingElement(QStringLiteral("bearer"));
                }
            }
            svcEl = svcEl.nextSiblingElement(QStringLiteral("service"));
        }
        return false;
    }

    if (QLatin1String("epg") != root.tagName()) return false;

    QDomElement schedEl = root.firstChildElement(QStringLiteral("schedule"));
    while (!schedEl.isNull())
    {
        uint32_t sid = 0;
        uint8_t scids = 0;
        bool haveSid = false;
        QDomElement scopeEl = schedEl.firstChildElement(QStringLiteral("scope"));
        if (!scopeEl.isNull())
        {
            QDomElement ssEl = scopeEl.firstChildElement(QStringLiteral("serviceScope"));
            while (!ssEl.isNull())
            {
                if (cliParseDabBearerUri(ssEl.attribute(QStringLiteral("id")), &sid, &scids))
                {
                    haveSid = true;
                    break;
                }
                ssEl = ssEl.nextSiblingElement(QStringLiteral("serviceScope"));
            }
        }
        if (!haveSid && !scopeId.isEmpty())
        {
            haveSid = cliParseDabBearerUri(scopeId, &sid, &scids);
        }
        if (haveSid)
        {
            QDomElement progEl = schedEl.firstChildElement(QStringLiteral("programme"));
            while (!progEl.isNull())
            {
                if (parseEpgProgrammeLocked(progEl, sid, scids, today, tomorrow))
                {
                    changed = true;
                }
                progEl = progEl.nextSiblingElement(QStringLiteral("programme"));
            }
        }
        schedEl = schedEl.nextSiblingElement(QStringLiteral("schedule"));
    }
    return changed;
}

bool DabCliApp::parseEpgProgrammeLocked(const QDomElement &element, uint32_t sid, uint8_t scids, const QDate &today, const QDate &tomorrow)
{
    QString longName, medName, shortName;
    QString longDesc, shortDesc;
    QDomElement child = element.firstChildElement();
    while (!child.isNull())
    {
        const QString tag = child.tagName();
        if (QLatin1String("longName") == tag) longName = child.text().trimmed();
        else if (QLatin1String("mediumName") == tag) medName = child.text().trimmed();
        else if (QLatin1String("shortName") == tag) shortName = child.text().trimmed();
        else if (QLatin1String("mediaDescription") == tag)
        {
            QDomElement dc = child.firstChildElement();
            while (!dc.isNull())
            {
                if (QLatin1String("longDescription") == dc.tagName() && longDesc.isEmpty())
                    longDesc = dc.text().trimmed();
                else if (QLatin1String("shortDescription") == dc.tagName() && shortDesc.isEmpty())
                    shortDesc = dc.text().trimmed();
                dc = dc.nextSiblingElement();
            }
        }
        child = child.nextSiblingElement();
    }

    QString title = !longName.isEmpty() ? longName : (!medName.isEmpty() ? medName : shortName);
    if (title.isEmpty()) return false;
    QString desc = !longDesc.isEmpty() ? longDesc : shortDesc;

    QTimeZone tz = cliDabTz(m_dabTime);
    qint64 minEndUtc = QDateTime(today, QTime(0, 0), tz).toSecsSinceEpoch();
    qint64 maxStartUtc = QDateTime(tomorrow.addDays(1), QTime(0, 0), tz).toSecsSinceEpoch();
    quint64 k = key(sid, scids);
    bool changed = false;

    QDomElement locEl = element.firstChildElement(QStringLiteral("location"));
    while (!locEl.isNull())
    {
        QDomElement timeEl = locEl.firstChildElement(QStringLiteral("time"));
        while (!timeEl.isNull())
        {
            QDateTime st = QDateTime::fromString(timeEl.attribute(QStringLiteral("time")), Qt::ISODate);
            int dur = cliParseIsoDurationSec(timeEl.attribute(QStringLiteral("duration")));
            if (st.isValid() && dur > 0)
            {
                qint64 stUtc = st.toUTC().toSecsSinceEpoch();
                if ((stUtc + dur) > minEndUtc && stUtc < maxStartUtc)
                {
                    QList<CliEpgProgram> &list = m_epgPrograms[k];
                    bool found = false;
                    for (CliEpgProgram &ex : list)
                    {
                        if (ex.startUtc == stUtc)
                        {
                            found = true;
                            if (ex.durationSec != dur || ex.title != title || ex.desc != desc)
                            {
                                ex.durationSec = dur;
                                ex.title = title;
                                ex.desc = desc;
                                changed = true;
                            }
                            break;
                        }
                    }
                    if (!found)
                    {
                        CliEpgProgram np;
                        np.startUtc = stUtc;
                        np.durationSec = dur;
                        np.title = title;
                        np.desc = desc;
                        list.append(np);
                        std::sort(list.begin(), list.end(), [](const CliEpgProgram &a, const CliEpgProgram &b) {
                            return a.startUtc < b.startUtc;
                        });
                        changed = true;
                    }
                }
            }
            timeEl = timeEl.nextSiblingElement(QStringLiteral("time"));
        }
        locEl = locEl.nextSiblingElement(QStringLiteral("location"));
    }
    return changed;
}

void DabCliApp::requestRadioDnsForEnsembleLocked()
{
    if ((nullptr == m_spiApp) || !m_dabTime.isValid() || !m_ensemble.isValid() || m_services.isEmpty()) return;
    QTimeZone tz = cliDabTz(m_dabTime);
    QDate today = m_dabTime.date();
    QList<uint32_t> ueids{m_ensemble.ueid};

    for (auto it = m_services.cbegin(); it != m_services.cend(); ++it)
    {
        if (!it->isAudio) continue;
        ServiceListId servId(it->sid, it->scids);
        quint64 k = key(it->sid, it->scids);
        for (int dayOff = 0; dayOff <= 1; ++dayOff)
        {
            QDate d = today.addDays(dayOff);
            QString qKey = QString("%1:%2:%3:%4")
                               .arg(m_ensemble.ueid, 0, 16)
                               .arg(it->sid, 0, 16)
                               .arg(it->scids)
                               .arg(d.toString(QStringLiteral("yyyyMMdd")));
            if (m_epgRadioDnsQueried.contains(qKey)) continue;
            m_epgRadioDnsQueried.insert(qKey);

            bool alreadyHaveDay = false;
            qint64 ds = QDateTime(d, QTime(0, 0), tz).toSecsSinceEpoch();
            qint64 de = QDateTime(d.addDays(1), QTime(0, 0), tz).toSecsSinceEpoch();
            const QList<CliEpgProgram> list = m_epgPrograms.value(k);
            for (const CliEpgProgram &pr : list)
            {
                if (pr.startUtc >= ds && pr.startUtc < de)
                {
                    alreadyHaveDay = true;
                    break;
                }
            }
            if (alreadyHaveDay) continue;

            QMetaObject::invokeMethod(m_spiApp, [spi = m_spiApp, servId, ueids, d]() {
                spi->getPI(servId, ueids, d);
            }, Qt::QueuedConnection);
        }
    }
}

bool DabCliApp::hasValidEpgLocked(QDate *todayOut, int *nowMinOut) const
{
    if (!m_dabTime.isValid() || m_epgPrograms.isEmpty()) return false;
    QTimeZone tz = cliDabTz(m_dabTime);
    QDate today = m_dabTime.date();
    QDateTime dayStart(today, QTime(0, 0), tz);
    qint64 minEndUtc = dayStart.toSecsSinceEpoch();
    qint64 maxStartUtc = QDateTime(today.addDays(2), QTime(0, 0), tz).toSecsSinceEpoch();
    if (todayOut) *todayOut = today;
    if (nowMinOut) *nowMinOut = qBound(0, int(dayStart.secsTo(m_dabTime) / 60), 1439);

    for (auto it = m_epgPrograms.cbegin(); it != m_epgPrograms.cend(); ++it)
    {
        for (const CliEpgProgram &pr : it.value())
        {
            if ((pr.startUtc + pr.durationSec) > minEndUtc && pr.startUtc < maxStartUtc)
            {
                return true;
            }
        }
    }
    return false;
}

QJsonObject DabCliApp::epgJson() const
{
    QMutexLocker lock(&m_mutex);
    QJsonObject root;
    QDate today;
    int nowMin = 0;
    if (!hasValidEpgLocked(&today, &nowMin))
    {
        root[QStringLiteral("available")] = false;
        root[QStringLiteral("services")] = QJsonArray();
        return root;
    }

    QTimeZone tz = cliDabTz(m_dabTime);
    QDate tomorrow = today.addDays(1);
    QDateTime dayStart(today, QTime(0, 0), tz);
    qint64 dayStartUtc = dayStart.toSecsSinceEpoch();
    qint64 windowEndUtc = QDateTime(today.addDays(2), QTime(0, 0), tz).toSecsSinceEpoch();

    root[QStringLiteral("available")] = true;
    root[QStringLiteral("dabTimeIso")] = m_dabTime.toString(Qt::ISODate);
    root[QStringLiteral("today")] = today.toString(Qt::ISODate);
    root[QStringLiteral("todayLabel")] = QString("Heute · %1").arg(today.toString(QStringLiteral("dd.MM.")));
    root[QStringLiteral("tomorrow")] = tomorrow.toString(Qt::ISODate);
    root[QStringLiteral("tomorrowLabel")] = QString("Morgen · %1").arg(tomorrow.toString(QStringLiteral("dd.MM.")));
    root[QStringLiteral("tomorrowShort")] = tomorrow.toString(QStringLiteral("dd.MM."));
    root[QStringLiteral("nowMinute")] = nowMin;
    root[QStringLiteral("totalMinutes")] = 2880;

    struct SvcEntry {
        quint64 k;
        uint32_t sid;
        uint8_t scids;
        QString label;
        bool isCurrent;
        QJsonArray progs;
    };
    QList<SvcEntry> entries;

    for (auto it = m_epgPrograms.cbegin(); it != m_epgPrograms.cend(); ++it)
    {
        QJsonArray progsArr;
        for (const CliEpgProgram &pr : it.value())
        {
            qint64 pEndUtc = pr.startUtc + pr.durationSec;
            if (pEndUtc <= dayStartUtc || pr.startUtc >= windowEndUtc) continue;
            int startMin = qBound(0, int((pr.startUtc - dayStartUtc) / 60), 2880);
            int endMin = qBound(0, int((pEndUtc - dayStartUtc) / 60), 2880);
            if (endMin <= startMin) continue;

            QDateTime stLocal = QDateTime::fromSecsSinceEpoch(pr.startUtc, tz);
            QDateTime enLocal = QDateTime::fromSecsSinceEpoch(pEndUtc, tz);
            QJsonObject po;
            po[QStringLiteral("startMin")] = startMin;
            po[QStringLiteral("endMin")] = endMin;
            po[QStringLiteral("startStr")] = stLocal.toString(QStringLiteral("HH:mm"));
            po[QStringLiteral("endStr")] = enLocal.toString(QStringLiteral("HH:mm"));
            po[QStringLiteral("dayLabel")] = (stLocal.date() == tomorrow)
                                                 ? QStringLiteral("Morgen")
                                                 : (stLocal.date() == today ? QStringLiteral("Heute") : stLocal.date().toString(QStringLiteral("dd.MM.")));
            po[QStringLiteral("title")] = pr.title;
            po[QStringLiteral("desc")] = pr.desc;
            po[QStringLiteral("isNow")] = (nowMin >= startMin && nowMin < endMin);
            progsArr.append(po);
        }
        if (progsArr.isEmpty()) continue;

        quint64 k = it.key();
        uint32_t sid = uint32_t(k >> 8);
        uint8_t scids = uint8_t(k & 0xFFu);
        QString label;
        if (m_services.contains(k) && !m_services.value(k).label.isEmpty())
            label = m_services.value(k).label;
        else if (m_epgServiceLabels.contains(k))
            label = m_epgServiceLabels.value(k);
        else
            label = QString("0x%1").arg(sid, 0, 16).toUpper();

        SvcEntry e;
        e.k = k;
        e.sid = sid;
        e.scids = scids;
        e.label = label;
        e.isCurrent = (sid == m_currentSid && scids == m_currentScids && m_isPlaying);
        e.progs = progsArr;
        entries.append(e);
    }

    std::sort(entries.begin(), entries.end(), [](const SvcEntry &a, const SvcEntry &b) {
        if (a.isCurrent != b.isCurrent) return a.isCurrent;
        return a.label.compare(b.label, Qt::CaseInsensitive) < 0;
    });

    QJsonArray svcsArr;
    for (const SvcEntry &e : entries)
    {
        QJsonObject so;
        so[QStringLiteral("sid")] = QString("0x%1").arg(e.sid, 0, 16);
        so[QStringLiteral("scids")] = e.scids;
        so[QStringLiteral("label")] = e.label;
        so[QStringLiteral("isCurrent")] = e.isCurrent;
        so[QStringLiteral("programs")] = e.progs;
        svcsArr.append(so);
    }
    root[QStringLiteral("services")] = svcsArr;
    return root;
}

void DabCliApp::onDlItemToggle()
{
    QMutexLocker lock(&m_mutex);
    m_dlPlusTitle.clear();
    m_dlPlusArtist.clear();
    for (auto it = m_dlPlusTags.begin(); it != m_dlPlusTags.end(); )
    {
        if (it.key() < int(DLPlusContentType::INFO_NEWS))
        {
            it = m_dlPlusTags.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void DabCliApp::onDlItemRunning(bool isRunning)
{
    QMutexLocker lock(&m_mutex);
    m_dlItemRunning = isRunning;
}

void DabCliApp::onTiiData(const RadioControlTIIData &data)
{
    QMutexLocker lock(&m_mutex);
    m_tiiList = data.idList;
    std::sort(m_tiiList.begin(), m_tiiList.end(), [](const dabsdrTii_t &a, const dabsdrTii_t &b) {
        return a.level > b.level;
    });
}

bool DabCliApp::requestTiiDbUpdate(QString *errorOut)
{
#if HAVE_FMLIST_INTERFACE
    QMutexLocker lock(&m_mutex);
    if (nullptr == m_fmlistInterface)
    {
        if (nullptr != errorOut) *errorOut = QStringLiteral("FMLIST interface not initialized");
        return false;
    }
    if (m_tiiDbUpdating)
    {
        if (nullptr != errorOut) *errorOut = QStringLiteral("Update already in progress");
        return false;
    }
    m_tiiDbUpdating = true;
    m_tiiDbStatus = QStringLiteral("Wird heruntergeladen...");
    lock.unlock();
    QFileInfo(TxDataLoader::dbfile()).dir().mkpath(QStringLiteral("."));
    m_fmlistInterface->updateTiiData();
    return true;
#else
    if (nullptr != errorOut) *errorOut = QStringLiteral("FMLIST support not compiled in");
    return false;
#endif
}

void DabCliApp::resetStreamStateLocked()
{
    m_streamDecoupled = false;
    m_streamSid = 0;
    m_streamScids = 0;
    m_streamServiceLabel.clear();
    m_streamPlaying = false;
    m_streamHaveAudioParams = false;
    m_streamDlsText.clear();
    m_streamDlPlusTitle.clear();
    m_streamDlPlusArtist.clear();
    m_streamDlPlusTags.clear();
    m_streamDlItemRunning = true;
    m_streamSlideData.clear();
    m_streamSlideContentType.clear();
}

QString DabCliApp::buildStreamSubtitleLocked() const
{
    if (m_streamDlItemRunning && !m_streamDlPlusTitle.isEmpty())
    {
        return m_streamDlPlusArtist.isEmpty() ? m_streamDlPlusTitle : QString("%1 - %2").arg(m_streamDlPlusArtist, m_streamDlPlusTitle);
    }
    return m_streamDlsText;
}

bool DabCliApp::isStreamDecoupled() const
{
    QMutexLocker lock(&m_mutex);
    return m_streamDecoupled;
}

void DabCliApp::requestStreamStop()
{
    audioFifo_t *priFifo = nullptr;
    {
        QMutexLocker lock(&m_mutex);
        resetStreamStateLocked();
#if !HAVE_PORTAUDIO
        priFifo = m_primaryAudioFifo;
#endif
    }
    if (m_audioStreamer)
    {
        m_audioStreamer->onStopAudio();
#if !HAVE_PORTAUDIO
        if (priFifo) m_audioStreamer->onStartAudio(priFifo);
#endif
    }
    Q_UNUSED(priFifo);
    emit stopSecondaryServiceRequested();
}

bool DabCliApp::requestStreamService(const QString &channelLabel, uint32_t sid, uint8_t scids, bool *decoupledOut, QString *errorOut)
{
    uint32_t wantFreq = 0;
    if (!channelLabel.trimmed().isEmpty())
    {
        const QString wanted = channelLabel.trimmed().toUpper();
        for (auto it = DabTables::channelList.cbegin(); it != DabTables::channelList.cend(); ++it)
        {
            if (it.value().toUpper() == wanted)
            {
                wantFreq = it.key();
                break;
            }
        }
        if (0 == wantFreq)
        {
            if (nullptr != errorOut) *errorOut = QString("Unknown DAB channel '%1'").arg(channelLabel);
            return false;
        }
    }

    uint32_t curFreq = 0;
    bool syncRetune = false;
    bool sameAsPrimary = false;

    {
        QMutexLocker lock(&m_mutex);
        curFreq = m_currentFrequency;
        if ((0 != wantFreq) && (wantFreq != curFreq))
        {
            syncRetune = true;
        }
        else
        {
            if (0 == curFreq)
            {
                if (nullptr != errorOut) *errorOut = QStringLiteral("Not tuned to any channel yet");
                return false;
            }
            if (!m_services.contains(key(sid, scids)))
            {
                if (nullptr != errorOut) *errorOut = QStringLiteral("Service not found in current ensemble");
                return false;
            }
            const CliServiceInfo &targetSvc = m_services.value(key(sid, scids));
            if ((sid == m_currentSid) && (scids == m_currentScids))
            {
                sameAsPrimary = true;
            }
            else if (m_services.contains(key(m_currentSid, m_currentScids)))
            {
                const CliServiceInfo &curSvc = m_services.value(key(m_currentSid, m_currentScids));
                if (targetSvc.subChId == curSvc.subChId)
                {
                    sameAsPrimary = true;
                }
            }
            if (sameAsPrimary)
            {
                resetStreamStateLocked();
            }
            else
            {
                m_streamDecoupled = true;
                m_streamSid = sid;
                m_streamScids = scids;
                m_streamServiceLabel = targetSvc.label;
                m_streamPlaying = false;
                m_streamHaveAudioParams = false;
                m_streamDlsText.clear();
                m_streamDlPlusTitle.clear();
                m_streamDlPlusArtist.clear();
                m_streamDlPlusTags.clear();
                m_streamDlItemRunning = true;
                m_streamSlideData.clear();
                m_streamSlideContentType.clear();
                ++m_streamSlideVersion;
            }
        }
    }

    if (syncRetune)
    {
        requestStreamStop();
        {
            QMutexLocker lock(&m_mutex);
            m_services.clear();
            m_currentFrequency = wantFreq;
            m_currentSid = sid;
            m_currentScids = scids;
            m_currentServiceLabel.clear();
            m_isPlaying = false;
            m_haveAudioParams = false;
            m_dabTime = QDateTime();
            m_tiiList.clear();
        }
        emit tuneServiceRequested(wantFreq, sid, scids);
        if (nullptr != decoupledOut) *decoupledOut = false;
        return true;
    }
    if (sameAsPrimary)
    {
        audioFifo_t *priFifo = nullptr;
#if !HAVE_PORTAUDIO
        {
            QMutexLocker lock(&m_mutex);
            priFifo = m_primaryAudioFifo;
        }
#endif
        if (m_audioStreamer)
        {
            m_audioStreamer->onStopAudio();
#if !HAVE_PORTAUDIO
            if (priFifo) m_audioStreamer->onStartAudio(priFifo);
#endif
        }
        Q_UNUSED(priFifo);
        emit stopSecondaryServiceRequested();
        if (nullptr != decoupledOut) *decoupledOut = false;
        return true;
    }
    emit tuneSecondaryServiceRequested(curFreq, sid, scids);
    if (nullptr != decoupledOut) *decoupledOut = true;
    return true;
}

bool DabCliApp::currentStreamSlide(QByteArray *data, QString *contentType) const
{
    QMutexLocker lock(&m_mutex);
    if (!m_streamDecoupled)
    {
        if (m_slideData.isEmpty()) return false;
        *data = m_slideData;
        *contentType = m_slideContentType;
        return true;
    }
    if (m_streamSlideData.isEmpty())
    {
        return false;
    }
    *data = m_streamSlideData;
    *contentType = m_streamSlideContentType;
    return true;
}

void DabCliApp::onSecondaryAudioServiceSelection(const RadioControlServiceComponent &s)
{
    QMutexLocker lock(&m_mutex);
    if (!m_streamDecoupled || s.SId.value() != m_streamSid || s.SCIdS != m_streamScids)
    {
        return;
    }
    m_streamServiceLabel = s.label;
    m_streamPlaying = true;
    m_streamHaveAudioParams = false;
    m_streamDlsText.clear();
    m_streamDlPlusTitle.clear();
    m_streamDlPlusArtist.clear();
    m_streamDlPlusTags.clear();
    m_streamDlItemRunning = true;
    m_streamSlideData.clear();
    m_streamSlideContentType.clear();
    ++m_streamSlideVersion;
}

void DabCliApp::onSecondaryStreamStopped()
{
    audioFifo_t *priFifo = nullptr;
    {
        QMutexLocker lock(&m_mutex);
        resetStreamStateLocked();
#if !HAVE_PORTAUDIO
        priFifo = m_primaryAudioFifo;
#endif
    }
    if (m_audioStreamer)
    {
        m_audioStreamer->onStopAudio();
#if !HAVE_PORTAUDIO
        if (priFifo) m_audioStreamer->onStartAudio(priFifo);
#endif
    }
    Q_UNUSED(priFifo);
}

void DabCliApp::onStreamAudioParametersInfo(const AudioParameters &params)
{
    QMutexLocker lock(&m_mutex);
    if (!m_streamDecoupled) return;
    m_streamAudioParams = params;
    m_streamHaveAudioParams = true;
}

void DabCliApp::onStreamDlComplete(const QString &dl)
{
    QMutexLocker lock(&m_mutex);
    if (!m_streamDecoupled) return;
    m_streamDlsText = dl;
}

void DabCliApp::onStreamDlPlusObject(const DLPlusObject &object)
{
    QMutexLocker lock(&m_mutex);
    if (!m_streamDecoupled) return;
    if (DLPlusContentType::ITEM_TITLE == object.getType())
    {
        m_streamDlPlusTitle = object.isDelete() ? QString() : object.getTag();
        return;
    }
    if (DLPlusContentType::ITEM_ARTIST == object.getType())
    {
        m_streamDlPlusArtist = object.isDelete() ? QString() : object.getTag();
        return;
    }
    if (object.isDelete())
    {
        m_streamDlPlusTags.remove(int(object.getType()));
        return;
    }
    const QString label = dlPlusContentTypeLabel(object.getType());
    if (!label.isEmpty())
    {
        m_streamDlPlusTags[int(object.getType())] = object.getTag();
    }
}

void DabCliApp::onStreamDlItemToggle()
{
    QMutexLocker lock(&m_mutex);
    if (!m_streamDecoupled) return;
    m_streamDlPlusTitle.clear();
    m_streamDlPlusArtist.clear();
    for (auto it = m_streamDlPlusTags.begin(); it != m_streamDlPlusTags.end(); )
    {
        if (it.key() < int(DLPlusContentType::INFO_NEWS))
        {
            it = m_streamDlPlusTags.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void DabCliApp::onStreamDlItemRunning(bool isRunning)
{
    QMutexLocker lock(&m_mutex);
    if (!m_streamDecoupled) return;
    m_streamDlItemRunning = isRunning;
}

void DabCliApp::onStreamDlReset()
{
    QMutexLocker lock(&m_mutex);
    if (!m_streamDecoupled) return;
    m_streamDlsText.clear();
    m_streamDlPlusTitle.clear();
    m_streamDlPlusArtist.clear();
    m_streamDlPlusTags.clear();
    m_streamDlItemRunning = true;
}

void DabCliApp::onStreamCurrentSlide(const Slide &slide)
{
    QMutexLocker lock(&m_mutex);
    if (!m_streamDecoupled) return;
    m_streamSlideData = slide.getRawData();
    const QString &format = slide.getFormat();
    m_streamSlideContentType = format.contains("PNG", Qt::CaseInsensitive) ? QStringLiteral("image/png") : QStringLiteral("image/jpeg");
    ++m_streamSlideVersion;
}

void DabCliApp::currentAudioFormat(int *sampleRate, int *numChannels) const
{
    QMutexLocker lock(&m_mutex);
    if (m_streamDecoupled)
    {
        if (m_streamHaveAudioParams)
        {
            *sampleRate = m_streamAudioParams.sampleRateKHz * 1000;
            *numChannels = m_streamAudioParams.stereo ? 2 : 1;
        }
        else
        {
            *sampleRate = 48000;
            *numChannels = 2;
        }
        return;
    }
    if (m_haveAudioParams)
    {
        *sampleRate = m_audioParams.sampleRateKHz * 1000;
        *numChannels = m_audioParams.stereo ? 2 : 1;
    }
    else
    {
        *sampleRate = 48000;
        *numChannels = 2;
    }
}

QString DabCliApp::currentStreamTitle() const
{
    QMutexLocker lock(&m_mutex);
    if (m_streamDecoupled)
    {
        return buildStreamSubtitleLocked();
    }
    return buildCurrentSubtitleLocked();
}

QString DabCliApp::buildCurrentSubtitleLocked() const
{
    if (!m_dlPlusTitle.isEmpty())
    {
        return m_dlPlusArtist.isEmpty() ? m_dlPlusTitle : QString("%1 - %2").arg(m_dlPlusArtist, m_dlPlusTitle);
    }
    return m_dlsText;
}

bool DabCliApp::currentSlide(QByteArray *data, QString *contentType) const
{
    QMutexLocker lock(&m_mutex);
    if (m_slideData.isEmpty())
    {
        return false;
    }
    *data = m_slideData;
    *contentType = m_slideContentType;
    return true;
}

QJsonObject DabCliApp::statusJson() const
{
    QMutexLocker lock(&m_mutex);

    QJsonObject root;
    root["deviceReady"] = m_deviceReady;
    root["deviceError"] = m_lastDeviceError;
    root["frequencyKHz"] = double(m_currentFrequency);
    root["channel"] = m_currentFrequency ? DabTables::channelList.value(m_currentFrequency, QString()) : QString();
    root["syncLevel"] = m_syncLevel;
    root["snr"] = double(m_snr);
        root["volumePercent"] = m_volumePercent;
    root["hasEpg"] = hasValidEpgLocked();
    if (m_dabTime.isValid())
    {
        root["dabTime"] = m_dabTime.toString(QStringLiteral("dd.MM.yyyy HH:mm"));
        root["dabTimeIso"] = m_dabTime.toString(Qt::ISODate);
    }
    QJsonObject streamObj;
    streamObj["decoupled"] = m_streamDecoupled;
    if (m_streamDecoupled)
    {
        streamObj["sid"] = QString("0x%1").arg(m_streamSid, 0, 16);
        streamObj["scids"] = m_streamScids;
        streamObj["label"] = m_streamServiceLabel;
        streamObj["playing"] = m_streamPlaying;
        streamObj["dls"] = m_streamDlsText;
        streamObj["dlPlusTitle"] = m_streamDlItemRunning ? m_streamDlPlusTitle : QString();
        streamObj["dlPlusArtist"] = m_streamDlItemRunning ? m_streamDlPlusArtist : QString();
        QJsonArray sTagsArr;
        for (auto it = m_streamDlPlusTags.constBegin(); it != m_streamDlPlusTags.constEnd(); ++it)
        {
            QJsonObject tagObj;
            tagObj["label"] = dlPlusContentTypeLabel(static_cast<DLPlusContentType>(it.key()));
            tagObj["text"] = it.value();
            sTagsArr.append(tagObj);
        }
        streamObj["dlPlusTags"] = sTagsArr;
        streamObj["slideVersion"] = m_streamSlideData.isEmpty() ? 0 : m_streamSlideVersion;
        if (m_streamHaveAudioParams)
        {
            QJsonObject saObj;
            saObj["sampleRate"] = m_streamAudioParams.sampleRateKHz * 1000;
            saObj["stereo"] = m_streamAudioParams.stereo;
            saObj["parametricStereo"] = m_streamAudioParams.parametricStereo;
            saObj["sbr"] = m_streamAudioParams.sbr;
            saObj["coding"] = int(m_streamAudioParams.coding);
            streamObj["audio"] = saObj;
        }
    }
    root["stream"] = streamObj;

    QJsonObject ensembleObj;
    ensembleObj["label"] = m_ensemble.label;
    ensembleObj["labelShort"] = m_ensemble.labelShort;
    ensembleObj["eid"] = m_ensemble.eid();
    ensembleObj["ecc"] = m_ensemble.ecc();
    ensembleObj["valid"] = m_ensemble.isValid();
    root["ensemble"] = ensembleObj;

    QJsonObject currentObj;
    currentObj["sid"] = QString("0x%1").arg(m_currentSid, 0, 16);
    currentObj["scids"] = m_currentScids;
    currentObj["label"] = m_currentServiceLabel;
    currentObj["playing"] = m_isPlaying;
    currentObj["dls"] = m_dlsText;
    currentObj["dlPlusTitle"] = m_dlItemRunning ? m_dlPlusTitle : QString();
    currentObj["dlPlusArtist"] = m_dlItemRunning ? m_dlPlusArtist : QString();
    QJsonArray dlPlusTagsArr;
    for (auto it = m_dlPlusTags.constBegin(); it != m_dlPlusTags.constEnd(); ++it)
    {
        QJsonObject tagObj;
        tagObj["label"] = dlPlusContentTypeLabel(static_cast<DLPlusContentType>(it.key()));
        tagObj["text"] = it.value();
        dlPlusTagsArr.append(tagObj);
    }
    currentObj["dlPlusTags"] = dlPlusTagsArr;
    currentObj["slideVersion"] = m_slideData.isEmpty() ? 0 : m_slideVersion;
    if (m_haveAudioParams)
    {
        QJsonObject audioObj;
        audioObj["sampleRate"] = m_audioParams.sampleRateKHz * 1000;
        audioObj["stereo"] = m_audioParams.stereo;
        audioObj["parametricStereo"] = m_audioParams.parametricStereo;
        audioObj["sbr"] = m_audioParams.sbr;
        audioObj["coding"] = int(m_audioParams.coding);
        currentObj["audio"] = audioObj;
    }
    root["current"] = currentObj;

    QJsonObject advancedObj;
    advancedObj["freqOffsetHz"] = double(m_freqOffsetHz);
    if (m_ensemble.isValid())
    {
        advancedObj["eid"] = QString("0x%1").arg(m_ensemble.eid(), 4, 16, QChar('0')).toUpper();
        advancedObj["ecc"] = QString("0x%1").arg(m_ensemble.ecc(), 2, 16, QChar('0')).toUpper();
    }
    if (m_currentSid != 0)
    {
        advancedObj["sid"] = QString("0x%1").arg(m_currentSid, 4, 16, QChar('0')).toUpper();
        advancedObj["scids"] = m_currentScids;
    }
    if (m_haveRfLevel)
    {
        advancedObj["rfLevel"] = double(m_rfLevel);
        advancedObj["gain"] = double(m_gain);
    }
    if (!m_tiiList.isEmpty())
    {
        QList<TxDataItem *> candidates;
        if (m_ensemble.isValid() && !m_txTable.isEmpty())
        {
            candidates = m_txTable.values(ServiceListId(m_ensemble.frequency, m_ensemble.ueid));
            if (candidates.isEmpty())
            {
                for (auto it = m_txTable.cbegin(); it != m_txTable.cend(); ++it)
                {
                    if (it.key().ueid() == m_ensemble.ueid && qAbs(int(it.key().freq()) - int(m_ensemble.frequency)) <= 2)
                    {
                        candidates.append(it.value());
                    }
                }
            }
        }
        QJsonArray tiiArr;
        for (int i = 0; i < m_tiiList.size(); ++i)
        {
            const dabsdrTii_t &t = m_tiiList.at(i);
            QString code = QString("%1-%2").arg(t.main, 2, 10, QChar('0')).arg(t.sub, 2, 10, QChar('0'));
            TxDataItem *matched = nullptr;
            for (TxDataItem *item : candidates)
            {
                if (item && item->mainId() == t.main && item->subId() == t.sub)
                {
                    matched = item;
                    break;
                }
            }
            QJsonObject to;
            to["main"] = t.main;
            to["sub"] = t.sub;
            to["code"] = code;
            to["levelDb"] = double(t.level);
            if (matched)
            {
                to["location"] = matched->location();
                to["powerKw"] = double(matched->power());
                to["antHeight"] = matched->antHeight();
            }
            tiiArr.append(to);
            if (i == 0)
            {
                advancedObj["tii"] = code;
                advancedObj["tiiMain"] = t.main;
                advancedObj["tiiSub"] = t.sub;
                if (matched)
                {
                    advancedObj["transmitter"] = matched->location();
                    advancedObj["tiiPowerKw"] = double(matched->power());
                }
            }
        }
        advancedObj["tiiList"] = tiiArr;
    }
    if (m_haveDecodingStats)
    {
        QJsonObject statsObj;
        statsObj["fibCntr"] = m_decodingStats.fibCntr;
        statsObj["fibErrorCntr"] = m_decodingStats.fibErrorCntr;
        statsObj["mscCrcOkCntr"] = m_decodingStats.mscCrcOkCntr;
        statsObj["mscCrcErrorCntr"] = m_decodingStats.mscCrcErrorCntr;
        statsObj["audioServiceBytes"] = m_decodingStats.audioServiceBytes;
        statsObj["padBytes"] = m_decodingStats.padBytes;
        statsObj["rsBytes"] = m_decodingStats.rsBytes;
        statsObj["rsBitErrorCntr"] = m_decodingStats.rsBitErrorCntr;
        statsObj["rsUncorrectableCntr"] = m_decodingStats.rsUncorrectableCntr;
        advancedObj["decodingStats"] = statsObj;
    }
    int activeSubCh = -1;
    if (m_services.contains(key(m_currentSid, m_currentScids)))
    {
        const CliServiceInfo &svc = m_services.value(key(m_currentSid, m_currentScids));
        activeSubCh = svc.subChId;
        advancedObj["subChId"] = svc.subChId;
        advancedObj["subChSize"] = svc.subChSize;
        advancedObj["startCU"] = svc.startCU;
        advancedObj["bitRate"] = svc.bitRate;
        advancedObj["protection"] = svc.protection;
        advancedObj["codec"] = svc.codec;
    }
    root["advanced"] = advancedObj;

    QJsonObject tiiDbObj;
#if HAVE_FMLIST_INTERFACE
    tiiDbObj["supported"] = true;
#else
    tiiDbObj["supported"] = false;
#endif
    tiiDbObj["loaded"] = !m_txTable.isEmpty();
    tiiDbObj["count"] = int(m_txTable.size());
    tiiDbObj["updating"] = m_tiiDbUpdating;
    tiiDbObj["status"] = m_tiiDbStatus;
    tiiDbObj["lastUpdate"] = m_txDbLastUpdate.isValid() ? m_txDbLastUpdate.toString(QStringLiteral("dd.MM.yyyy HH:mm")) : QString();
    root["tiiDb"] = tiiDbObj;

    QJsonArray servicesArr;
    QMap<uint8_t, QJsonObject> subchMap;
    QMap<uint8_t, QStringList> subchLabels;
    for (auto it = m_services.cbegin(); it != m_services.cend(); ++it)
    {
        if (it->subChSize > 0)
        {
            if (!subchMap.contains(it->subChId))
            {
                QJsonObject scObj;
                scObj["subChId"] = it->subChId;
                scObj["startCU"] = it->startCU;
                scObj["numCU"] = it->subChSize;
                scObj["bitRate"] = it->bitRate;
                scObj["protection"] = it->protection;
                scObj["codec"] = it->codec;
                scObj["isAudio"] = it->isAudio;
                scObj["active"] = (int(it->subChId) == activeSubCh);
                subchMap.insert(it->subChId, scObj);
            }
            if (!it->label.isEmpty() && !subchLabels[it->subChId].contains(it->label))
            {
                subchLabels[it->subChId].append(it->label);
            }
        }
        if (!it->isAudio)
        {
            continue;
        }
        QJsonObject o;
        o["sid"] = QString("0x%1").arg(it->sid, 0, 16);
        o["scids"] = it->scids;
        o["label"] = it->label;
        o["subChId"] = it->subChId;
        o["startCU"] = it->startCU;
        o["subChSize"] = it->subChSize;
        o["bitRate"] = it->bitRate;
        o["protection"] = it->protection;
        o["codec"] = it->codec;
        servicesArr.append(o);
    }
    QList<QJsonObject> subchList = subchMap.values();
    std::sort(subchList.begin(), subchList.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value("startCU").toInt() < b.value("startCU").toInt();
    });
    QJsonArray subchArr;
    for (QJsonObject &scObj : subchList)
    {
        uint8_t sid8 = uint8_t(scObj.value("subChId").toInt());
        scObj["labels"] = QJsonArray::fromStringList(subchLabels.value(sid8));
        subchArr.append(scObj);
    }
    root["services"] = servicesArr;
    root["subchannels"] = subchArr;

    return root;
}

QJsonArray DabCliApp::channelsJson()
{
    QJsonArray arr;
    for (auto it = DabTables::channelList.cbegin(); it != DabTables::channelList.cend(); ++it)
    {
        QJsonObject o;
        o["channel"] = it.value();
        o["frequencyKHz"] = double(it.key());
        arr.append(o);
    }
    return arr;
}
