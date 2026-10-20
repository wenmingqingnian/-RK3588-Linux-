#ifndef VEHICLEIOINPUT_H
#define VEHICLEIOINPUT_H

#include <QObject>
#include <QString>

class QSocketNotifier;
class QTimer;

class VehicleIoInput : public QObject
{
    Q_OBJECT

public:
    explicit VehicleIoInput(QObject *parent = nullptr);
    ~VehicleIoInput() override;

    bool start(const QString &devicePath = QString());
    void stop();

    QString devicePath() const;
    bool isConnected() const;

    bool reverseActive() const;
    bool accActive() const;
    bool buttonActive() const;

signals:
    void deviceConnected(const QString &devicePath);
    void deviceDisconnected(const QString &devicePath);
    void errorOccurred(const QString &message);

    void reverseChanged(bool active);
    void accChanged(bool active);
    void buttonChanged(bool active);

private slots:
    void tryAutoDetect();
    void readAvailableEvents();

private:
    QString findVehicleInputDevice() const;
    bool openDevice(const QString &devicePath);
    void closeDevice(bool notifyDisconnect);
    void readInitialKeyState();
    void handleKeyEvent(unsigned short code, int value);
    void setReverseActive(bool active);
    void setAccActive(bool active);
    void setButtonActive(bool active);

    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QTimer *m_scanTimer = nullptr;

    QString m_devicePath;
    bool m_autoScan = false;
    bool m_reverseActive = false;
    bool m_accActive = false;
    bool m_buttonActive = false;
};

#endif // VEHICLEIOINPUT_H
