#ifndef BAIDUMAP_H
#define BAIDUMAP_H

#include "staticmapconfig.h"
#include "nmeaparser.h"

#include <QByteArray>
#include <QLabel>
#include <QMainWindow>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QEventLoop>
#include <QDebug>
#include <QMessageBox>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QVector>
#include <QFile>
#include <QDate>
#include <QSslSocket>
#include <QPair>
#include <QPainter>
#include <QDebug>
#include <QElapsedTimer>

#include <mutex>

#define MAP_COMMAND_CLOSE 0
#define MAP_COMMAND_SHOW 1
#define MAP_COMMAND_AMPLIFY 2
#define MAP_COMMAND_SHRINK 3

namespace Ui {
class BaiduMap;
}

class GpsWorker;

class BaiduMap : public QMainWindow
{
    Q_OBJECT

public:
    explicit BaiduMap(QWidget *parent = nullptr);
    ~BaiduMap();

public slots:
    void on_handleCommand(int);

private slots:
    void ReceiveMapImg(QNetworkReply *reply);

    void on_pBtn_ZoomUp_clicked();
    void on_pBtn_ZoomDown_clicked();
    void on_pBtn_Close_clicked();

    // GPS worker signal handlers. These run on the GUI thread via Qt queued
    // connections — the worker thread never touches GUI widgets directly.
    void onGpsFixUpdated(vehiclegps::GpsFix fix);
    void onGpsStatusChanged(QString statusText);
    void onGpsAvailabilityChanged(bool available);

private:
    Ui::BaiduMap *ui;

    QNetworkAccessManager netManager;
    QNetworkRequest request;
    QFile file;
    QLabel *mapImageLabel = nullptr;

    // GPS state. Updated only on the GUI thread (via queued signals from the
    // worker). jingdu/weidu are the last known coordinates; gpsFixValid_ marks
    // whether they come from a real/simulated fix or are fallback defaults.
    double jingdu = 113.3800;
    double weidu = 34.3500;
    bool gpsFixValid_ = false;
    bool gpsSimulated_ = false;
    QString gpsStatusText_;
    vehiclegps::GpsFix lastFix_;

    // GPS worker (real serial or simulation). Owned by BaiduMap and stopped in
    // the destructor before the GUI tears down.
    GpsWorker *gpsWorker_ = nullptr;
    int mapUpdateIntervalMs_ = 1000;
    QElapsedTimer lastMapUpdate_;

    // GPS status/info overlay labels (created dynamically, no .ui change).
    QLabel *gpsStatusLabel_ = nullptr;
    QLabel *gpsInfoLabel_ = nullptr;

    vehiclemap::StaticMapConfig mapConfig;
    int img_Zoom = 10;

    void setupMapImageView();
    void setupGpsOverlay();
    void displayMapImage(const QByteArray &data, const QString &sourceName);
    void updateMap();
    void updateGpsOverlay();
    bool shouldRefreshMap();
};

#endif // BAIDUMAP_H
