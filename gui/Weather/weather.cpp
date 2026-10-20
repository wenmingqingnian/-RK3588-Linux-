#include "Weather/weather.h"
#include "ui_weather.h"
#include "weatherformat.h"
#include "weatherconfig.h"
#include "logmanager.h"

#include <cstdlib>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QtGlobal>

static const char *MODULE = "weather";

Weather::Weather(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::Weather),
    weatherRequestEnabled(false)
{
    ui->setupUi(this);
    this->resize(1024,600);

    // Remove Qt Designer demonstration values before any real weather response
    // is received. This prevents stale TextLabel/sample forecasts from being
    // mistaken for live data.
    const QStringList designerSampleTexts = {
        QStringLiteral("TextLabel"),
        QStringLiteral("周三"),
        QStringLiteral("07月31日"),
        QStringLiteral("26~32℃"),
        QStringLiteral("26~32 ℃"),
        QStringLiteral("阴转雷阵雨"),
        QStringLiteral("东风3-4级"),
        QStringLiteral("26 优")
    };

    const QList<QLabel *> weatherLabels = findChildren<QLabel *>();
    for (QLabel *label : weatherLabels) {
        if (label != nullptr &&
            designerSampleTexts.contains(label->text().trimmed())) {
            label->clear();
        }
    }

    ui->label_Time->setText(QStringLiteral("等待天气数据"));
    ui->label_location->setText(QStringLiteral("--"));

    // The F1D4 global return-home overlay occupies the top-right header area.
    // Hide the original compact header buttons and provide a dedicated refresh
    // button immediately below the global return button.
    ui->pushButton->hide();
    ui->pushButton_2->hide();

    QPushButton *refreshOverlayButton =
        new QPushButton(QStringLiteral("刷新天气"), this);
    refreshOverlayButton->setObjectName(
        QStringLiteral("weatherRefreshOverlayButton"));
    refreshOverlayButton->setGeometry(844, 100, 150, 52);
    refreshOverlayButton->setFocusPolicy(Qt::NoFocus);
    refreshOverlayButton->setStyleSheet(
        "QPushButton {"
        " color: white;"
        " background-color: rgba(35, 35, 35, 220);"
        " border: 2px solid white;"
        " border-radius: 8px;"
        " font-size: 20px;"
        " font-weight: bold;"
        "}"
        "QPushButton:pressed {"
        " background-color: rgba(0, 120, 180, 230);"
        "}");
    connect(refreshOverlayButton, &QPushButton::clicked,
            this, [this]() {
                updateInfo();
            });
    refreshOverlayButton->show();
    refreshOverlayButton->raise();

    std::string configPath = "config/weather_settings.conf";
    const char *envPath = std::getenv("VEHICLE_WEATHER_CONFIG");
    if (envPath != nullptr && envPath[0] != '\0') {
        configPath = envPath;
    }

    bool loaded = false;
    std::string errorMessage;
    try {
        vehicleweather::WeatherConfig weatherConfig =
            vehicleweather::loadWeatherConfigFile(configPath, &loaded, &errorMessage);

        if (!loaded) {
            VLOG_WARN(MODULE, "weather config: %s", errorMessage.c_str());
        }

        if (weatherConfig.enabled) {
            std::string url = vehicleweather::buildWeatherRequestUrl(weatherConfig);
            if (url.empty()) {
                ui->label_Time->setText(QStringLiteral("天气请求地址无效"));
                VLOG_WARN(MODULE, "weather: built empty URL; request skipped");
            } else {
                request.setUrl(QUrl(QString::fromStdString(url)));
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
                request.setTransferTimeout(weatherConfig.timeoutMs);
#endif
                weatherRequestEnabled = true;
                connect(&netManager, SIGNAL(finished(QNetworkReply *)),
                        this, SLOT(getWeatherInfo(QNetworkReply *)));
                ui->label_Time->setText(QStringLiteral("天气更新中..."));
                netManager.get(request);
            }
        } else {
            ui->label_Time->setText(QStringLiteral("天气服务未配置"));
            ui->label_location->setText(QStringLiteral("--"));
            VLOG_DEBUG(MODULE, "weather input disabled");
        }
    } catch (const vehicleweather::WeatherConfigError &e) {
        ui->label_Time->setText(QStringLiteral("天气配置无效"));
        ui->label_location->setText(QStringLiteral("--"));
        VLOG_ERROR(MODULE, "weather config error: %s", e.what());
    }

    ui->groupBox_Total->setObjectName("MainGroupBox");
    ui->groupBox_Total->setStyleSheet("#MainGroupBox{background-image:url(':/weather/Weather/images/UI2.png');}");
}

Weather::~Weather()
{
    delete ui;
}

/* 将日期转换为星期  */
QString Weather::TransDataToWeek(QString Data)
{
    QDate date;
    date =  QDate::fromString(Data,"yyyy-MM-dd");
    int week = date.dayOfWeek();
    switch (week) {
    case 1:
        return "星期一";
    case 2:
        return "星期二";
    case 3:
        return "星期三";
    case 4:
        return "星期四";
    case 5:
        return "星期五";
    case 6:
        return "星期六";
    case 7:
        return "星期日";
    default:
        return "未知";
    }
}

/* 将 xx-xx 转换为xx月xx日  */
QString Weather::TransDataToMyData(QString Data)
{
    std::string formatted;
    if (!vehicleweather::formatIsoDateToMonthDay(Data.toStdString(), &formatted)) {
        VLOG_WARN(MODULE, "invalid weather date: %s",
                  Data.toUtf8().constData());
    }
    return QString::fromStdString(formatted);
}

/* 拼接显示 昼夜温度  */
QString Weather::TransTempToStr(QString tmpDay, QString tmpNight)
{
    return QString("%1~%2 ℃").arg(tmpDay).arg(tmpNight);
}

/* 根据天气选择对应图片  */
QString Weather::SelectWeatherImg(QString WeatherDes)
{
    return QString(":/weather/Weather/images/%1.png").arg(WeatherDes);
}

void Weather::updateInfo()
{
    if (!weatherRequestEnabled || !request.url().isValid() || request.url().isEmpty()) {
        ui->label_Time->setText(QStringLiteral("天气服务未配置"));
        VLOG_WARN(MODULE, "weather request skipped: config disabled or invalid");
        return;
    }

    ui->label_Time->setText(QStringLiteral("天气更新中..."));
    netManager.get(request);
}


/* 根据网络请求结果解析得到天气数据  并转换为合适格式后显示  */
void Weather::getWeatherInfo(QNetworkReply *reply)
{
    if (reply == nullptr) {
        VLOG_WARN(MODULE, "weather reply is null");
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        ui->label_Time->setText(QStringLiteral("天气请求失败"));
        VLOG_ERROR(MODULE, "weather network error %d: %s",
                   reply->error(),
                   reply->errorString().toUtf8().constData());
        reply->deleteLater();
        return;
    }

    QByteArray bytes = reply->readAll();
    //qDebug()<<bytes;
    QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if(!doc.isObject()){
        ui->label_Time->setText(QStringLiteral("天气数据格式错误"));
        VLOG_DEBUG(MODULE, "netjson not a json object");
        reply->deleteLater();
        return;
    }
    QJsonObject obj = doc.object();
   // QString cityId = obj.value("cityid").toString();
    cityName = obj.value("city").toString();
    UpdateTime = obj.value("update_time").toString();
    QJsonArray arrays = obj.value("data").toArray();
    if (!obj.value("data").isArray() || arrays.size() < 7) {
        ui->label_Time->setText(QStringLiteral("天气数据不完整"));
        VLOG_WARN(MODULE, "weather data array missing or too short (size=%d)",
                  arrays.size());
        reply->deleteLater();
        return;
    }

    for (int i = 0; i < 7; ++i) {
        const QString date = arrays.at(i).toObject().value("date").toString();
        std::string formatted;
        if (!vehicleweather::formatIsoDateToMonthDay(date.toStdString(), &formatted)) {
            VLOG_WARN(MODULE, "weather day %d invalid date: %s", i,
                      date.toUtf8().constData());
            reply->deleteLater();
            return;
        }
    }

    weather_data.clear();
    weather_wea.clear();
    weather_tem_day.clear();
    weather_tem_night.clear();
    weather_win.clear();
    weather_win_speed.clear();
    weather_wea_img.clear();

    QJsonObject weather0 = arrays[0].toObject();
    QJsonObject weather1 = arrays[1].toObject();
    QJsonObject weather2 = arrays[2].toObject();
    QJsonObject weather3 = arrays[3].toObject();
    QJsonObject weather4 = arrays[4].toObject();
    for(int i=0;i<7;i++)
    {
        weather_data.append(arrays[i].toObject().value("date").toString());
        weather_wea.append(arrays[i].toObject().value("wea").toString());
        weather_tem_day.append(arrays[i].toObject().value("tem_day").toString());
        weather_tem_night.append(arrays[i].toObject().value("tem_night").toString());
        weather_win.append(arrays[i].toObject().value("win").toString());
        weather_win_speed.append(arrays[i].toObject().value("win_speed").toString());
        weather_wea_img.append(arrays[i].toObject().value("wea_img").toString());
    }

/*
    for(int i=0;i<7;i++)
    {
        QString result = QString("%1  %2  %3  %4  %5  %6").arg(weather_data.at(i)).arg(weather_wea.at(i))
            .arg(weather_tem_day.at(i)).arg(weather_tem_night.at(i)).arg(weather_win.at(i)).arg(weather_win_speed.at(i));
        qDebug()<<result;
    }
*/
    ui->label_day1_data->setText(TransDataToMyData(weather_data.at(1)));
    ui->label_day2_data->setText(TransDataToMyData(weather_data.at(2)));
    ui->label_day3_data->setText(TransDataToMyData(weather_data.at(3)));
    ui->label_day4_data->setText(TransDataToMyData(weather_data.at(4)));

    ui->label_day1_week->setText(TransDataToWeek(weather_data.at(1)));
    ui->label_day2_week->setText(TransDataToWeek(weather_data.at(2)));
    ui->label_day3_week->setText(TransDataToWeek(weather_data.at(3)));
    ui->label_day4_week->setText(TransDataToWeek(weather_data.at(4)));

    ui->label_day1_des->setText(weather_wea.at(1));
    ui->label_day2_des->setText(weather_wea.at(2));
    ui->label_day3_des->setText(weather_wea.at(3));
    ui->label_day4_des->setText(weather_wea.at(4));

    ui->label_day1_win->setText(weather_win.at(1));
    ui->label_day2_win->setText(weather_win.at(2));
    ui->label_day3_win->setText(weather_win.at(3));
    ui->label_day4_win->setText(weather_win.at(4));

    ui->label_day1_winSpeed->setText(weather_win_speed.at(1));
    ui->label_day2_winSpeed->setText(weather_win_speed.at(2));
    ui->label_day3_winSpeed->setText(weather_win_speed.at(3));
    ui->label_day4_winSpeed->setText(weather_win_speed.at(4));

    ui->label_day1_tmp->setText(TransTempToStr(weather_tem_day.at(1),weather_tem_night.at(1)));
    ui->label_day2_tmp->setText(TransTempToStr(weather_tem_day.at(2),weather_tem_night.at(2)));
    ui->label_day3_tmp->setText(TransTempToStr(weather_tem_day.at(3),weather_tem_night.at(3)));
    ui->label_day4_tmp->setText(TransTempToStr(weather_tem_day.at(4),weather_tem_night.at(4)));

    QPixmap pixmap;

    ui->label_day1_img->setScaledContents(true);
    ui->label_day2_img->setScaledContents(true);
    ui->label_day3_img->setScaledContents(true);
    ui->label_day4_img->setScaledContents(true);
    pixmap.load(SelectWeatherImg(weather_wea_img.at(1)));
    // QPixmap::scaled 是 const 函数，返回新对象，必须使用返回值；原 pixmap 不会改变
    ui->label_day1_img->setPixmap(pixmap.scaled(ui->label_day1_img->size(),
                                                 Qt::KeepAspectRatio,
                                                 Qt::SmoothTransformation));
    pixmap.load(SelectWeatherImg(weather_wea_img.at(2)));
    ui->label_day2_img->setPixmap(pixmap);
    pixmap.load(SelectWeatherImg(weather_wea_img.at(3)));
    ui->label_day3_img->setPixmap(pixmap);
    pixmap.load(SelectWeatherImg(weather_wea_img.at(4)));
    ui->label_day4_img->setPixmap(pixmap);

    ui->label_now_img->setScaledContents(true);
    pixmap.load(SelectWeatherImg(weather_wea_img.at(0)));
    ui->label_now_img->setPixmap(pixmap);
    ui->label_now_des->setText(weather_wea.at(0));
    ui->label_now_win->setText(weather_win.at(0));
    ui->label_now_winSpeed->setText(weather_win_speed.at(0));
    ui->label_now_tmp->setText(weather_tem_day.at(0));

    ui->label_Time->setText(UpdateTime);
    ui->label_location->setText(cityName);

    reply->deleteLater();
}


void Weather::on_pushButton_clicked()
{
    updateInfo();
}

void Weather::on_pushButton_2_clicked()
{
    this->hide();
}
