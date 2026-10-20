#include "baidumap.h"
#include "ui_baidumap.h"
#include "staticmapconfig.h"
#include "staticmapresponse.h"
#include "vehicledeviceconfig.h"
#include "gpsworker.h"
#include "logmanager.h"

#include <QByteArray>
#include <QPixmap>
#include <QVariant>
#include <algorithm>
#include <cstdio>
#include <string>

static const char *MODULE = "baidumap";

namespace {

std::string deviceConfigPath()
{
    const QByteArray envPath = qgetenv("VEHICLE_DEVICE_CONFIG");
    if (!envPath.isEmpty()) {
        return std::string(envPath.constData(), envPath.size());
    }
    return "./config/vehicle_devices.conf";
}

std::string mapConfigPath()
{
    const QByteArray envPath = qgetenv("VEHICLE_MAP_CONFIG");
    if (!envPath.isEmpty()) {
        return std::string(envPath.constData(), envPath.size());
    }
    return "./config/map_settings.conf";
}

vehiclemap::StaticMapConfig loadMapConfig()
{
    bool loaded = false;
    std::string errorMessage;
    try {
        vehiclemap::StaticMapConfig config =
            vehiclemap::loadStaticMapConfigFile(mapConfigPath(), &loaded, &errorMessage);
        if (!loaded && !errorMessage.empty()) {
            VLOG_WARN(MODULE, "map config: %s; map network stays disabled",
                      errorMessage.c_str());
        }
        return config;
    } catch (const vehiclemap::MapConfigError &error) {
        VLOG_WARN(MODULE, "map config error: %s; map network stays disabled",
                  error.what());
        return vehiclemap::StaticMapConfig();
    }
}

} // namespace

BaiduMap::BaiduMap(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::BaiduMap)
{
    ui->setupUi(this);
    mapConfig = loadMapConfig();
    img_Zoom = mapConfig.defaultZoom;
    this->resize(mapConfig.width, mapConfig.height);
    ui->pBtn_ZoomUp->move(std::max(0, mapConfig.width - 100),
                          std::max(0, mapConfig.height - 100));
    ui->pBtn_ZoomDown->move(std::max(0, mapConfig.width - 200),
                            std::max(0, mapConfig.height - 100));
    ui->pBtn_Close->resize(50, 50);
    ui->pBtn_Close->move(0, 0);
    setupMapImageView();
    setupGpsOverlay();
    connect(&netManager, &QNetworkAccessManager::finished,
            this, &BaiduMap::ReceiveMapImg);

    // Load device config and launch the GPS worker. The worker picks real-GPS
    // or simulation mode based on config. Both default to disabled.
    bool deviceConfigLoaded = false;
    std::string deviceConfigError;
    vehicledevice::VehicleDeviceConfig deviceConfig;
    try {
        deviceConfig = vehicledevice::loadDeviceConfigFile(deviceConfigPath(),
                                                           &deviceConfigLoaded,
                                                           &deviceConfigError);
    } catch (const vehicledevice::DeviceConfigError &error) {
        VLOG_WARN(MODULE, "device config error: %s; gps stays disabled",
                  error.what());
    }

    if (!deviceConfigLoaded && !deviceConfigError.empty()) {
        VLOG_WARN(MODULE, "device config: %s; gps stays disabled",
                  deviceConfigError.c_str());
    }

    // Pick the map refresh interval based on which mode is enabled. Real GPS
    // uses gps_update_interval_ms; simulation uses gps_map_update_interval_ms.
    // Both default to 1000ms; the fallback covers the both-disabled case.
    if (deviceConfig.gpsSimulation.enabled) {
        mapUpdateIntervalMs_ = deviceConfig.gpsSimulation.mapUpdateIntervalMs;
    } else if (deviceConfig.gps.enabled) {
        mapUpdateIntervalMs_ = deviceConfig.gps.updateIntervalMs;
    } else {
        mapUpdateIntervalMs_ = 1000;
    }
    if (mapUpdateIntervalMs_ <= 0) {
        mapUpdateIntervalMs_ = 1000;
    }

    gpsWorker_ = new GpsWorker(deviceConfig.gps,
                               deviceConfig.gpsSimulation,
                               this);
    connect(gpsWorker_, &GpsWorker::fixUpdated,
            this, &BaiduMap::onGpsFixUpdated, Qt::QueuedConnection);
    connect(gpsWorker_, &GpsWorker::statusChanged,
            this, &BaiduMap::onGpsStatusChanged, Qt::QueuedConnection);
    connect(gpsWorker_, &GpsWorker::availabilityChanged,
            this, &BaiduMap::onGpsAvailabilityChanged, Qt::QueuedConnection);
    gpsWorker_->start();

    lastMapUpdate_.start();
    updateMap();
}

BaiduMap::~BaiduMap()
{
    if (gpsWorker_ != nullptr) {
        gpsWorker_->stop();
    }
    delete ui;
}

void BaiduMap::setupMapImageView()
{
    ui->widget->resize(mapConfig.width, mapConfig.height);

    mapImageLabel = new QLabel(ui->widget);
    mapImageLabel->setObjectName("mapImageLabel");
    mapImageLabel->setGeometry(0, 0, mapConfig.width, mapConfig.height);
    mapImageLabel->setScaledContents(true);
    mapImageLabel->setStyleSheet(
        "QLabel { background-color: black; border: none; }");
    mapImageLabel->show();

    // Put the decoded map above the page background. Interactive overlays are
    // raised immediately afterwards so they remain clickable.
    mapImageLabel->raise();
    ui->pBtn_ZoomUp->raise();
    ui->pBtn_ZoomDown->raise();
    ui->pBtn_Close->raise();
}

void BaiduMap::setupGpsOverlay()
{
    // Status label: top-left corner, below the close button.
    gpsStatusLabel_ = new QLabel(ui->widget);
    gpsStatusLabel_->setObjectName("gpsStatusLabel");
    gpsStatusLabel_->setGeometry(10, 60, 300, 30);
    gpsStatusLabel_->setStyleSheet(
        "QLabel { color: white; background-color: rgba(0, 0, 0, 160); "
        "font-size: 14px; padding: 4px; border-radius: 4px; }");
    gpsStatusLabel_->setText(QString::fromUtf8("GPS已关闭"));
    gpsStatusLabel_->raise();
    gpsStatusLabel_->show();

    // Info label: below the status label, shows lat/lon/speed/etc.
    gpsInfoLabel_ = new QLabel(ui->widget);
    gpsInfoLabel_->setObjectName("gpsInfoLabel");
    gpsInfoLabel_->setGeometry(10, 95, 300, 80);
    gpsInfoLabel_->setStyleSheet(
        "QLabel { color: white; background-color: rgba(0, 0, 0, 160); "
        "font-size: 12px; padding: 4px; border-radius: 4px; }");
    gpsInfoLabel_->setText(QString::fromUtf8("回退坐标"));
    gpsInfoLabel_->raise();
    gpsInfoLabel_->show();
}

void BaiduMap::displayMapImage(const QByteArray &data, const QString &sourceName)
{
    if (mapImageLabel == nullptr) {
        VLOG_WARN(MODULE, "map image label not initialized");
        return;
    }

    QPixmap pixmap;
    if (!pixmap.loadFromData(data)) {
        VLOG_WARN(MODULE, "map image decode failed: %s",
                  sourceName.toUtf8().constData());
        return;
    }

    mapImageLabel->setGeometry(0, 0, mapConfig.width, mapConfig.height);
    mapImageLabel->setPixmap(pixmap);
    mapImageLabel->show();
    mapImageLabel->raise();
    mapImageLabel->update();

    // Keep all controls above the map image.
    ui->pBtn_ZoomUp->raise();
    ui->pBtn_ZoomDown->raise();
    ui->pBtn_Close->raise();
    if (gpsStatusLabel_ != nullptr) {
        gpsStatusLabel_->raise();
    }
    if (gpsInfoLabel_ != nullptr) {
        gpsInfoLabel_->raise();
    }

    VLOG_INFO(MODULE,
              "map image displayed: source=%s pixmap=%dx%d "
              "label=%dx%d visible=%d",
              sourceName.toUtf8().constData(),
              pixmap.width(), pixmap.height(),
              mapImageLabel->width(), mapImageLabel->height(),
              mapImageLabel->isVisible() ? 1 : 0);
}

void BaiduMap::updateMap()
{
    // Build the center coordinate string. Only use GPS coordinates when a
    // valid fix exists; otherwise fall back to the configured default and mark
    // the data as fallback (never send 0,0).
    std::string center = mapConfig.defaultCenter;
    if (gpsFixValid_) {
        center = QString("%1,%2")
            .arg(QString::number(jingdu, 'f', 5))
            .arg(QString::number(weidu, 'f', 5))
            .toStdString();
    }

    if (!mapConfig.enabled || mapConfig.urlTemplate.empty() ||
        mapConfig.apiKey.empty()) {
        VLOG_WARN(MODULE, "map config disabled or missing url/apikey; request skipped");
        return;
    }

    const QString url = QString::fromStdString(
        vehiclemap::buildStaticMapUrl(mapConfig, center, img_Zoom));
    VLOG_DEBUG(MODULE, "map request queued (center=%s, zoom=%d, source=%s)",
               center.c_str(), img_Zoom,
               gpsFixValid_ ? (gpsSimulated_ ? "sim" : "gps") : "fallback");
    request.setUrl(QUrl(url));
    netManager.get(request);
    lastMapUpdate_.restart();
}

void BaiduMap::ReceiveMapImg(QNetworkReply *reply)
{
    if (reply == nullptr) {
        VLOG_WARN(MODULE, "map reply is null");
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        VLOG_ERROR(MODULE, "map network error %d: %s",
                   reply->error(),
                   reply->errorString().toUtf8().constData());
        reply->deleteLater();
        return;
    }

    const QByteArray data = reply->readAll();
    if (data.isEmpty()) {
        VLOG_WARN(MODULE, "map reply data empty (HTTP %d)",
                   reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        reply->deleteLater();
        return;
    }

    const QString contentType =
        reply->header(QNetworkRequest::ContentTypeHeader).toString();
    const std::string responseData(data.constData(),
                                   static_cast<std::size_t>(data.size()));
    if (!vehiclemap::isSupportedStaticMapImageResponse(contentType.toStdString(),
                                                       responseData)) {
        VLOG_WARN(MODULE, "map reply not a supported image (HTTP %d, type=%s)",
                   reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                   contentType.toUtf8().constData());
        reply->deleteLater();
        return;
    }

    QFile output(QString::fromStdString(mapConfig.outputFile));
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        VLOG_ERROR(MODULE, "map output file open failed: %s",
                   output.fileName().toUtf8().constData());
        reply->deleteLater();
        return;
    }

    const qint64 written = output.write(data);
    if (written != data.size()) {
        VLOG_ERROR(MODULE, "map output file write failed: %s (wrote %lld, expected %d)",
                   output.fileName().toUtf8().constData(),
                   written, data.size());
        output.close();
        reply->deleteLater();
        return;
    }

    output.close();
    VLOG_INFO(MODULE, "map image saved: %s (%d bytes)",
              output.fileName().toUtf8().constData(), data.size());
    displayMapImage(data, output.fileName());
    reply->deleteLater();
}

void BaiduMap::on_pBtn_ZoomUp_clicked()
{
    if (img_Zoom == mapConfig.maxZoom) {
        return;
    }

    ++img_Zoom;
    updateMap();
}

void BaiduMap::on_pBtn_ZoomDown_clicked()
{
    if (img_Zoom == mapConfig.minZoom) {
        return;
    }

    --img_Zoom;
    updateMap();
}

void BaiduMap::on_pBtn_Close_clicked()
{
    this->hide();
}

void BaiduMap::on_handleCommand(int command)
{
    switch (command) {
    case MAP_COMMAND_SHOW:
        this->show();
        break;
    case MAP_COMMAND_CLOSE:
        this->close();
        break;
    case MAP_COMMAND_AMPLIFY:
        on_pBtn_ZoomUp_clicked();
        break;
    case MAP_COMMAND_SHRINK:
        on_pBtn_ZoomDown_clicked();
        break;
    default:
        break;
    }
}

void BaiduMap::onGpsFixUpdated(vehiclegps::GpsFix fix)
{
    // Reject 0,0 defensively — the parser already validates ranges, but a
    // bad sentence should never reach the map as a real coordinate.
    if (fix.latitude == 0.0 && fix.longitude == 0.0) {
        VLOG_WARN(MODULE, "gps fix rejected: 0,0 coordinate");
        return;
    }

    jingdu = fix.longitude;
    weidu = fix.latitude;
    gpsFixValid_ = true;
    gpsSimulated_ = fix.simulated;
    lastFix_ = fix;

    updateGpsOverlay();

    if (shouldRefreshMap()) {
        updateMap();
    }
}

void BaiduMap::onGpsStatusChanged(QString statusText)
{
    gpsStatusText_ = statusText;
    updateGpsOverlay();
}

void BaiduMap::onGpsAvailabilityChanged(bool available)
{
    if (!available && !gpsFixValid_) {
        // Device/simulator unavailable and no cached fix — mark as fallback.
        gpsStatusText_ = QString::fromUtf8("GPS已关闭");
        updateGpsOverlay();
    }
}

void BaiduMap::updateGpsOverlay()
{
    if (gpsStatusLabel_ == nullptr || gpsInfoLabel_ == nullptr) {
        return;
    }

    gpsStatusLabel_->setText(gpsStatusText_.isEmpty()
        ? QString::fromUtf8("GPS已关闭")
        : gpsStatusText_);

    QString sourceTag;
    if (!gpsFixValid_) {
        sourceTag = QString::fromUtf8("回退坐标");
    } else if (gpsSimulated_) {
        sourceTag = QString::fromUtf8("模拟数据");
    } else {
        sourceTag = QString::fromUtf8("GPS数据");
    }

    gpsInfoLabel_->setText(
        QString::fromUtf8("来源: %1\n经度: %2  纬度: %3\n速度: %4 km/h  航向: %5°\n卫星: %6")
            .arg(sourceTag)
            .arg(jingdu, 0, 'f', 5)
            .arg(weidu, 0, 'f', 5)
            .arg(gpsFixValid_ ? QString::number(lastFix_.speedKmh, 'f', 1)
                              : QString::fromUtf8("—"))
            .arg(gpsFixValid_ ? QString::number(lastFix_.courseDegrees, 'f', 0)
                              : QString::fromUtf8("—"))
            .arg(gpsFixValid_ ? QString::number(lastFix_.satellites)
                              : QString::fromUtf8("—")));
}

bool BaiduMap::shouldRefreshMap()
{
    if (!lastMapUpdate_.isValid()) {
        lastMapUpdate_.start();
        return true;
    }
    return lastMapUpdate_.elapsed() >= mapUpdateIntervalMs_;
}
