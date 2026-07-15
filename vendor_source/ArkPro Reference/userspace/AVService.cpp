#include "AVService.h"
#include "ark_api.h"
#include "AudioService.h"
#include "Utility.h"
#include <QDebug>
#include <QCoreApplication>
#include <QTimer>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>

class AVServicePrivate
{
public:
    explicit AVServicePrivate(AVService* parent);
    ~AVServicePrivate();
    void initialize();
    void connectAllSlots();
    bool requestAudioSource(const AudioSource source);
    void releaseAudioSource(const AudioSource source);
    int dvr_fd;
    int fb_dev_fd;
    AVType m_AVType;
    AVStatus m_AVStatus;
    QTimer* m_Timer;
private:
    AVService* m_Parent;
};

AVService::AVService(QObject *parent)
    : QObject(parent)
    , m_Private(new AVServicePrivate(this))
{
}

AVService::~AVService()
{
}

void AVService::releaseAudioSource(const AudioSource source)
{
    qDebug() << "AVService::releaseAudioSource" << source;
    switch (source) {
    case AS_AV1: {
        stopAV(AVT_1);
        break;
    }
    case AS_AV2: {
        stopAV(AVT_2);
        break;
    }
    default: {
        break;
    }
    }
}

void AVService::startAV(const int type, const int width, const int height)
{
    dvr_source avType(DVR_SOURCE_AUX);
    AudioSource sourceType(AS_Idle);
    if (AVT_1 == type) {
        avType = DVR_SOURCE_AUX;
        sourceType = AS_AV1;
    } else if (AVT_2 == type) {
        avType = DVR_SOURCE_CAMERA;
        sourceType = AS_AV2;
    } else {
        return ;
    }
    if (m_Private->requestAudioSource(sourceType)) {
        m_Private->m_AVType = type;
        if (AVS_Disconnect == m_Private->m_AVStatus) {
            qDebug() << "m_Private->dvr_fd1" << m_Private->dvr_fd;
            if (-1 == m_Private->dvr_fd) {
                m_Private->m_AVStatus = AVS_Connecting;
#ifndef gcc
                m_Private->dvr_fd = arkapi_open_dvr();
                qDebug() << "arkapi_open_dvr" << m_Private->dvr_fd;
                if (m_Private->dvr_fd < 0) {
                    printf("open dvr device failure.\n");
                    exit(EXIT_FAILURE);
                }
                char *endptr;
                int tvout = strtoul("tvout", &endptr, 10);
                if (arkapi_dvr_config(m_Private->dvr_fd, avType, width, height, tvout) < 0) {
                    printf("config dvr failure.\n");
                    exit(EXIT_FAILURE);
                }
                m_Private->fb_dev_fd = arkapi_open_video_fb();
                qDebug() << "arkapi_open_video_fb" << m_Private->fb_dev_fd;
                if (m_Private->fb_dev_fd < 0) {
                    printf("open video fb device failure.\n");
                    exit(EXIT_FAILURE);
                }
                int ret = arkapi_dvr_start(m_Private->dvr_fd);
                qDebug() << "arkapi_dvr_start" << ret;
                if (ret < 0) {
                    printf("open start dvr failure.\n");
                    exit(EXIT_FAILURE);
                }
#else
                m_Private->dvr_fd = 10086;
#endif
            }
        }
        if (-1 != m_Private->dvr_fd) {
            m_Private->m_Timer->start();
        }
        qDebug() << "m_Private->m_AVStatus" << m_Private->m_AVStatus;
        emit onAVStatus(type, m_Private->m_AVStatus);
    }
}

void AVService::stopAV(const AVType type)
{
    qDebug() << "AVService::stopAV" << type;
    AudioSource sourceType(AS_Idle);
    if (AVT_1 == type) {
        sourceType = AS_AV1;
    } else {
        sourceType = AS_AV2;
    }
    m_Private->releaseAudioSource(sourceType);
    m_Private->m_AVStatus = AVS_Disconnect;
    m_Private->m_Timer->stop();
#ifndef gcc
    if (-1 != m_Private->dvr_fd) {
        arkapi_dvr_stop(m_Private->dvr_fd);
        arkapi_close_dvr(m_Private->dvr_fd);
        m_Private->dvr_fd = -1;
    }
    if (-1 != m_Private->fb_dev_fd) {
        arkapi_close_video_fb(m_Private->fb_dev_fd);
        m_Private->fb_dev_fd = -1;
    }
#endif
#ifdef gcc
    m_Private->dvr_fd = -1;
    qDebug() << "m_Private->dvr_fd2" << m_Private->dvr_fd ;
#endif
    emit onAVStatus(type, m_Private->m_AVStatus);
}

void AVService::onTimeout()
{
    if (-1 != m_Private->dvr_fd) {
#ifndef gcc
        int ret = arkapi_dvr_detect_signal(m_Private->dvr_fd);
        if (0 != ret) {
            if (AVS_Connected != m_Private->m_AVStatus) {
                m_Private->m_AVStatus = AVS_Connected;
                emit onAVStatus(m_Private->m_AVType, m_Private->m_AVStatus);
            }
        } else {
            if (AVS_Disconnect != m_Private->m_AVStatus) {
                m_Private->m_AVStatus = AVS_Disconnect;
                emit onAVStatus(m_Private->m_AVType, m_Private->m_AVStatus);
            }
        }
#else
        static AVStatus lastStatus = -10086;
        if (m_Private->m_AVStatus != lastStatus) {
            lastStatus = m_Private->m_AVStatus;
            emit onAVStatus(m_Private->m_AVType, m_Private->m_AVStatus);
        }
#endif
    }
}

AVServicePrivate::AVServicePrivate(AVService *parent)
    : m_Parent(parent)
{
    dvr_fd = -1;
    fb_dev_fd = -1;
    m_Timer = NULL;
    m_AVType = AVT_1;
    m_AVStatus = AVS_Disconnect;
    initialize();
    connectAllSlots();
}

AVServicePrivate::~AVServicePrivate()
{
}

void AVServicePrivate::initialize()
{
    bool ret = QDBusConnection::sessionBus().registerObject(QString("/com/arkmicro/av"),
                                                            m_Parent,
                                                            QDBusConnection::ExportNonScriptableSlots);
    if (!ret) {
        qCritical() << "AVService Register QDbus failed!";
        exit(EXIT_FAILURE);
    }
    qDebug() << "AVService Register QDbus ok!";
    m_Timer = new QTimer(m_Parent);
    m_Timer->setSingleShot(false);
    m_Timer->setInterval(1.5 * 1000);
}

void AVServicePrivate::connectAllSlots()
{
    Qt::ConnectionType type = static_cast<Qt::ConnectionType>(Qt::AutoConnection | Qt::UniqueConnection);
    QObject::connect(m_Timer,  SIGNAL(timeout()),
                     m_Parent, SLOT(onTimeout()),
                     type);
}

bool AVServicePrivate::requestAudioSource(const AudioSource source)
{
    qDebug() << "AVServicePrivate::requestAudioSource" << source;
    bool flag(false);
    acquirePreemptiveResource(NULL, NULL);
    QDBusInterface interface(ArkMicroAudioService,
                             ArkMicroAudioPath,
                             ArkMicroAudioInterface,
                             QDBusConnection::sessionBus());
    if (!interface.isValid()) {
        qDebug() << qPrintable(QDBusConnection::sessionBus().lastError().message());
        exit(EXIT_FAILURE);
    }
    QDBusReply<bool> reply = interface.call(ArkMicroAudioRequest,
                                            source,
                                            qApp->applicationName(),
                                            QString("/com/arkmicro/av"),
                                            QString("Local.DbusServer.AV"),
                                            ArkMicroAudioRelease);
    if (reply.isValid()) {
        flag = reply.value();
    } else {
        qDebug() << "AVServicePrivate::requestAudioSource fail!";
    }
    return flag;
}

void AVServicePrivate::releaseAudioSource(const AudioSource source)
{
    qDebug() << "AVServicePrivate::releaseAudioSource";
    clearOwner();
    QDBusInterface interface(ArkMicroAudioService,
                             ArkMicroAudioPath,
                             ArkMicroAudioInterface,
                             QDBusConnection::sessionBus());
    if (!interface.isValid()) {
        qDebug() << qPrintable(QDBusConnection::sessionBus().lastError().message());
        exit(EXIT_FAILURE);
    }
    QDBusReply<void> reply = interface.call(ArkMicroAudioRelease, source);
    if (reply.isValid()) {
        qDebug() << "AVServicePrivate::releaseAudioSource valid!";
    } else {
        qDebug( "AVServicePrivate::releaseAudioSource fail!\n");
    }
}
