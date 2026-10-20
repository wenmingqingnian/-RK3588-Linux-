#include "speechrecognition.h"
#include "logmanager.h"
#include <QByteArray>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/ioctl.h>

static const char *MODULE = "speechrecognition";

SpeechRecognition::SpeechRecognition()
{
    timer = new QTimer(this);
    initAudioRecord();
    initKeyRead();
    connect(timer, SIGNAL(timeout()), this, SLOT(timer_timerout()));
    VLOG_INFO(MODULE, "speech recognition initialized");
}

void SpeechRecognition::initAudioRecord()
{
    m_audioRecorder = new QAudioRecorder(this);
    /* 扫描本地声卡设备 */
    devicesVar.append(QVariant(QString()));
    for (auto &device: m_audioRecorder->audioInputs()) {
        devicesVar.append(QVariant(device));
        //qDebug()<<"本地声卡设备："<<device<<endl;
    }

    /* 音频编码 */
    codecsVar.append(QVariant(QString()));
    for (auto &codecName: m_audioRecorder->supportedAudioCodecs()) {
        codecsVar.append(QVariant(codecName));
        //qDebug()<<"音频编码："<<codecName<<endl;
    }

    /* 容器/支持的格式 */
    containersVar.append(QVariant(QString()));
    for (auto &containerName: m_audioRecorder->supportedContainers()) {
        containersVar.append(QVariant(containerName));
        //qDebug()<<"支持的格式："<<containerName<<endl;
    }

    /* 采样率 */
    sampleRateVar.append(QVariant(0));
    sampleRateVar.append(QVariant(8000));
    sampleRateVar.append(QVariant(16000));
    for (int sampleRate: m_audioRecorder->supportedAudioSampleRates()) {
        sampleRateVar.append(QVariant(sampleRate));
        //qDebug()<<"采样率："<<sampleRate<<endl;
    }

    /* 通道 */
    channelsVar.append(QVariant(-1));
    channelsVar.append(QVariant(1));
    channelsVar.append(QVariant(2));
    channelsVar.append(QVariant(4));

    /* 质量 */
    qualityVar.append(QVariant(int(QMultimedia::LowQuality)));
    qualityVar.append(QVariant(int(QMultimedia::NormalQuality)));
    qualityVar.append(QVariant(int(QMultimedia::HighQuality)));

    /* 比特率 */
    bitratesVar.append(QVariant(0));
    bitratesVar.append(QVariant(32000));
    bitratesVar.append(QVariant(64000));
    bitratesVar.append(QVariant(96000));
    bitratesVar.append(QVariant(128000));
}

void SpeechRecognition::initKeyRead()
{
    bool loaded = false;
    std::string errorMessage;
    deviceConfig = vehicledevice::loadDeviceConfigFile(deviceConfigPath(),
                                                       &loaded,
                                                       &errorMessage);
    if (!loaded && !errorMessage.empty()) {
        VLOG_WARN(MODULE, "device config: %s; key input stays disabled",
                  errorMessage.c_str());
    }

    keyInputDevice.configure(deviceConfig.keyInput);
    if (!keyInputDevice.openDevice(&errorMessage)) {
        VLOG_WARN(MODULE, "key device: %s; key input stays disabled",
                  errorMessage.c_str());
    }
    timer->setInterval(deviceConfig.keyInput.pollIntervalMs);
}

std::string SpeechRecognition::deviceConfigPath() const
{
    const QByteArray envPath = qgetenv("VEHICLE_DEVICE_CONFIG");
    if (!envPath.isEmpty()) {
        return std::string(envPath.constData(), envPath.size());
    }
    return "./config/vehicle_devices.conf";
}

/* 开启录音  */
void SpeechRecognition::startRecord()
{
    VLOG_INFO(MODULE, "starting audio recording");
    m_audioRecorder->setAudioInput(devicesVar.at(0).toString());
    QAudioEncoderSettings settings;
    settings.setCodec(codecsVar.at(0).toString());
    settings.setSampleRate(sampleRateVar[2].toInt());
    settings.setBitRate(bitratesVar[0].toInt());
    settings.setChannelCount(channelsVar[1].toInt());
    settings.setQuality(QMultimedia::EncodingQuality(
        qualityVar[0].toInt()));
    /* 以恒定的质量录制，可选恒定的比特率 */
    settings.setEncodingMode(QMultimedia::ConstantQualityEncoding);
    /* 按名称查找 WAV 容器，避免依赖平台相关的硬编码下标 */
    int containerIndex = 0;  // 默认空容器（由平台选择）
    for (int i = 1; i < containersVar.size(); ++i) {
        const QString name = containersVar.at(i).toString().toLower();
        if (name.contains(QLatin1String("wav"))) {
            containerIndex = i;
            break;
        }
    }
    QString container = containersVar.at(containerIndex).toString();
    m_audioRecorder->setEncodingSettings(settings,QVideoEncoderSettings(),container);
    m_audioRecorder->setOutputLocation(QUrl::fromLocalFile(tr("./record.wav")));
    m_audioRecorder->record();
}


/*  录音停止 并发送信号，调用对应槽函数处理  */
void SpeechRecognition::stopRecord()
{
    m_audioRecorder->stop();
    emit RecordFinished();
}

/* 获取按键值，如果是按下 按键值为240，否则为0  */
int SpeechRecognition::readKeyValue()
{
    std::string errorMessage;
    const int keyValue = keyInputDevice.readKeyValue(&errorMessage);
    if (!errorMessage.empty()) {
        VLOG_WARN(MODULE, "key read error: %s", errorMessage.c_str());
    }
    return keyValue;
}


/* 定时器槽函数，用来实时检测按键是否按下松开，按下时录音，松开后结束录音并进行识别并控制其他功能 */
void SpeechRecognition::timer_timerout()
{
    int keyValue = readKeyValue();
    if(keyValue==deviceConfig.keyInput.pressedValue)
    {
       if(hasRecord==0) startRecord();
        hasRecord =1;
    }
    else
    {
        if(hasRecord==1)stopRecord();
        hasRecord = 0;
    }
}


/*得到网络请求的槽函数，对得到的网络请求结果进行处理*/
void SpeechRecognition::getSpeechResult(QNetworkReply *reply)
{
    reply->waitForReadyRead(500);
    QByteArray content = reply->readAll();
    // lastIndexOf 找不到时返回 -1，QByteArray::remove 会将负 position 视为 0，从而误删首字节
    const int newlinePos = content.lastIndexOf('\n');
    if (newlinePos >= 0) {
        content.remove(newlinePos, 1);
    }
    QJsonDocument doc = QJsonDocument::fromJson(content);
    if (!doc.isObject()) {
        VLOG_WARN(MODULE, "ASR reply is not a JSON object");
        reply->deleteLater();
        return;
    }
    QJsonObject obj = doc.object();
    QJsonArray resutls = obj.value("result").toArray();
    if (resutls.isEmpty()) {
        VLOG_WARN(MODULE, "ASR result array is empty");
        reply->deleteLater();
        return;
    }
    QString AsrResult = resutls.at(0).toString();
    VLOG_INFO(MODULE, "ASR result: %s", AsrResult.toUtf8().constData());
    reply->deleteLater();
}


/* 开启语音控制功能 ，也就是开启定时器进行实时按键检测  */
void SpeechRecognition::startSpeechRecognition()
{
    if (!keyInputDevice.isAvailable()) {
        VLOG_WARN(MODULE, "key input disabled; speech recognition not started");
        return;
    }
    timer->start();
    VLOG_INFO(MODULE, "speech recognition started (poll interval=%dms)",
              deviceConfig.keyInput.pollIntervalMs);
}
