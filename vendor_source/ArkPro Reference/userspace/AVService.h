#ifndef AVSERVICE_H
#define AVSERVICE_H

#include <QObject>
#include <QScopedPointer>

enum AVType {
    AVT_1 = 0,
    AVT_2 = 1
};
#define AVType int

enum AVStatus {
    AVS_Connected = 0,
    AVS_Connecting = 1,
    AVS_Disconnect = 2
};
#define AVStatus int

class AVServicePrivate;
class AVService : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(AVService)
    Q_CLASSINFO("D-Bus Interface", "Local.DbusServer.AV")
public:
    class Interface {
        Q_DISABLE_COPY(Interface)
    protected:
        Interface() {}
        virtual ~Interface() {}
        //    public:
        virtual void startAV(const int type, const int width, const int height) = 0;
        //    void startAV(const AVType type, const int width, const int height);
        virtual void stopAV(const int type) = 0;
        //    void stopAV(const AVType type);
        //    signals:
        virtual void onAVStatus(const int type, const int status) = 0;
        //    void onStartAV(const AVType type, const AVStatus status);
    };
    explicit AVService(QObject* parent = NULL);
    ~AVService();
public slots:
    void releaseAudioSource(const int source);
//    void releaseAudioSource(const AudioSource source);
public:
    void startAV(const int type, const int width, const int height);
    //    void startAV(const int type, const int width, const int height);
    void stopAV(const int type);
    //    void stopAV(const AVType type);
signals:
    void onAVStatus(const int type, const int status);
    //    void onStartAV(const AVType type, const AVStatus status);
private slots:
    void onTimeout();
private:
    friend class AVServicePrivate;
    QScopedPointer<AVServicePrivate> m_Private;
};

#endif // AVSERVICE_H
