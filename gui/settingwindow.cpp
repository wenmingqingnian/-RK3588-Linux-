#include "settingwindow.h"
#include "settinggifscanner.h"
#include "ui_settingwindow.h"
#include "logmanager.h"

#include <QCoreApplication>
#include <QRandomGenerator>

static const char *MODULE = "setting";

SettingWindow::SettingWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::SettingWindow),
    movie(new QMovie(this)),
    GifSum(0),
    timer(new QTimer(this))
{
    ui->setupUi(this);
    this->resize(1024, 600);

    ui->label_ShowGif->setFixedSize(500, 300);
    ui->label_ShowGif->setScaledContents(true);
    ui->label_setting_date->setFixedWidth(225);

    timer->setInterval(500);
    timer->start();
    connect(timer, SIGNAL(timeout()), this, SLOT(on_timer_updateTime()));
    ScanGif();
}

SettingWindow::~SettingWindow()
{
    if (movie != nullptr) {
        movie->stop();
    }
    delete ui;
}

void SettingWindow::on_pushButton_8_clicked()
{
    this->hide();
}

void SettingWindow::ScanGif()
{
    GifSum = 0;
    currentGifIndex = 0;
    gif_Files.clear();

    if (movie != nullptr) {
        movie->stop();
    }

    const QString gifDirectory = QCoreApplication::applicationDirPath() + LocalGifPath;
    const QByteArray gifDirectoryBytes = gifDirectory.toLocal8Bit();
    const std::vector<std::string> files =
        vehiclesettings::collectGifFiles(std::string(gifDirectoryBytes.constData(),
                                                     gifDirectoryBytes.size()));

    for (std::vector<std::string>::const_iterator it = files.begin();
         it != files.end();
         ++it) {
        gif_Files.append(QString::fromLocal8Bit(it->c_str()));
    }

    GifSum = gif_Files.count();
    if (GifSum <= 0 || gif_Files.isEmpty()) {
        ui->label_ShowGif->setMovie(nullptr);
        ui->label_ShowGif->setText("./MyGif directory has no usable GIF file");
        VLOG_WARN(MODULE, "setting gif directory has no usable gif files: %s",
                  gifDirectory.toUtf8().constData());
        return;
    }

    if (movie == nullptr) {
        VLOG_WARN(MODULE, "%s", "setting gif movie is not initialized");
        return;
    }

    movie->setFileName(gif_Files.first());
    ui->label_ShowGif->setMovie(movie);
    if (!movie->isValid()) {
        VLOG_WARN(MODULE, "setting gif file is invalid: %s",
                  gif_Files.first().toUtf8().constData());
        return;
    }
    movie->start();
}

void SettingWindow::on_pBtn_PauseGif_clicked(bool checked)
{
    if (GifSum <= 0 || movie == nullptr) {
        return;
    }

    if (!checked) {
        ui->pBtn_PauseGif->setText("Pause");
        movie->start();
    } else {
        ui->pBtn_PauseGif->setText("Play");
        movie->stop();
    }
}

void SettingWindow::on_pBtn_SwitchGif_clicked()
{
    if (GifSum <= 0 || gif_Files.isEmpty() || movie == nullptr) {
        return;
    }

    const int id = QRandomGenerator::global()->bounded(GifSum);
    currentGifIndex = id;
    movie->stop();
    movie->setFileName(gif_Files.at(id));

    if (!ui->pBtn_ModifyTime->isChecked()) {
        movie->start();
    }
}

void SettingWindow::on_pBtn_ModifyDate_clicked()
{
    QString dateStr = ui->dateEdit->date().toString("yyyy-MM-dd");
    QString timeStr = QTime::currentTime().toString("hh:mm:ss");
    QString string = QString("%1 %2").arg(dateStr).arg(timeStr);
    VLOG_DEBUG(MODULE, "set date: %s", string.toUtf8().constData());
    // date 与 hwclock 必须串行执行：同一 QProcess 实例第二次 start 会终止第一次
    process.start("date", QStringList() << "-s" << string);
    if (!process.waitForFinished(3000)) {
        VLOG_WARN(MODULE, "date set timeout or failed: %s",
                  process.errorString().toUtf8().constData());
    }
    process.close();
    process.start("hwclock", QStringList() << "-w");
    if (!process.waitForFinished(3000)) {
        VLOG_WARN(MODULE, "hwclock write timeout or failed: %s",
                  process.errorString().toUtf8().constData());
    }
    process.close();
}

void SettingWindow::on_pBtn_ModifyTime_clicked()
{
    int hour = ui->spinBox_hour->value();
    int min = ui->spinBox_Min->value();
    int second = ui->spinBox_Sec->value();
    QString string = QString("%1:%2:%3").arg(hour).arg(min).arg(second);
    VLOG_DEBUG(MODULE, "set time: %s", string.toUtf8().constData());
    // date 与 hwclock 必须串行执行：同一 QProcess 实例第二次 start 会终止第一次
    process.start("date", QStringList() << "-s" << string);
    if (!process.waitForFinished(3000)) {
        VLOG_WARN(MODULE, "date set timeout or failed: %s",
                  process.errorString().toUtf8().constData());
    }
    process.close();
    process.start("hwclock", QStringList() << "-w");
    if (!process.waitForFinished(3000)) {
        VLOG_WARN(MODULE, "hwclock write timeout or failed: %s",
                  process.errorString().toUtf8().constData());
    }
    process.close();
}

void SettingWindow::on_timer_updateTime()
{
    ui->label_setting_date->setText(QDate::currentDate().toString("yyyy-MM-dd"));
    ui->label_setting_time->setText(QTime::currentTime().toString("hh:mm:ss"));
}

void SettingWindow::on_pushButton_3_clicked()
{
    process.start("ifconfig", QStringList() << "wlan0" << "up");
    if (!process.waitForFinished(3000)) {
        VLOG_WARN(MODULE, "ifconfig wlan0 up timeout or failed: %s",
                  process.errorString().toUtf8().constData());
    }
    QString error = QString(process.readAllStandardError());
    process.close();
    if (0 < error.length()) {
        QMessageBox::warning(this, "error", error);
        return;
    }
}

void SettingWindow::on_pushButton_4_clicked()
{
    process.start("wpa_cli", QStringList() << "-i" << "wlan0" << "scan_result");
    if (!process.waitForFinished(3000)) {
        VLOG_WARN(MODULE, "wpa_cli scan_result timeout or failed: %s",
                  process.errorString().toUtf8().constData());
    }
    QString error = QString(process.readAllStandardError());
    if (0 < error.length()) {
        QMessageBox::warning(this, "error", error);
        return;
    }

    QStringList scanResult = QString(process.readAllStandardOutput()).split("\n");
    process.close();
    ui->listWidget->clear();
    for (int i = 1; i < scanResult.count(); i++) {
        QString str = scanResult.at(i).split("\t").last();
        ui->listWidget->addItem(str);
        VLOG_DEBUG(MODULE, "wifi scan: %s", scanResult.at(i).toUtf8().constData());
    }
}

void SettingWindow::on_pushButton_5_clicked()
{
    if (ui->listWidget->selectedItems().isEmpty()) {
        QMessageBox::warning(this, "error", "no wifi item selected");
        return;
    }

    QString wifiName = ui->listWidget->selectedItems()[0]->text();
    QString password = ui->lineEdit_password->text();
    VLOG_DEBUG(MODULE, "wifi connect: %s", wifiName.toUtf8().constData());
    // source 是 shell 内建命令，不能作为 QProcess 的可执行程序入口
    // 改用 /bin/bash 显式执行脚本，无需脚本具有可执行权限
    process.start("/bin/bash", QStringList()
                  << "/home/root/shell/wifi/alientek_usb_wifi_setup.sh"
                  << "-m" << "station"
                  << "-i" << wifiName
                  << "-p" << password
                  << "-d" << "wlan0");
    if (!process.waitForFinished(15000)) {
        VLOG_WARN(MODULE, "wifi setup timeout or failed: %s",
                  process.errorString().toUtf8().constData());
    }
    QString error = QString(process.readAllStandardError());
    if (0 < error.length()) {
        QMessageBox::warning(this, "error", error);
        return;
    }
    QMessageBox::information(this, "Wifi Connected", "wifi connected");
}
