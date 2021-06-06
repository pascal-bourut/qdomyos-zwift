#include "sportstechbike.h"
#include "keepawakehelper.h"
#include "virtualbike.h"
#include <QBluetoothLocalDevice>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QMetaEnum>
#include <QSettings>
#include <QThread>
#include <chrono>

using namespace std::chrono_literals;

sportstechbike::sportstechbike(bool noWriteResistance, bool noHeartService) {
    m_watt.setType(metric::METRIC_WATT);
    refresh = new QTimer(this);
    this->noWriteResistance = noWriteResistance;
    this->noHeartService = noHeartService;
    initDone = false;
    connect(refresh, &QTimer::timeout, this, &sportstechbike::update);
    refresh->start(200ms);
}

void sportstechbike::writeCharacteristic(uint8_t *data, uint8_t data_len, const QString &info, bool disable_log,
                                         bool wait_for_response) {
    QEventLoop loop;
    QTimer timeout;

    if (wait_for_response) {
        connect(this, &sportstechbike::packetReceived, &loop, &QEventLoop::quit);
        timeout.singleShot(300ms, &loop, &QEventLoop::quit);
    } else {
        connect(gattCommunicationChannelService, &QLowEnergyService::characteristicWritten, &loop, &QEventLoop::quit);
        timeout.singleShot(300ms, &loop, &QEventLoop::quit);
    }

    gattCommunicationChannelService->writeCharacteristic(gattWriteCharacteristic,
                                                         QByteArray((const char *)data, data_len));

    if (!disable_log) {
        emit debug(QStringLiteral(" >> ") + QByteArray((const char *)data, data_len).toHex(' ') + " // " + info);
    }

    loop.exec();

    if (timeout.isActive() == false) {
        emit debug(QStringLiteral(" exit for timeout"));
    }
}

void sportstechbike::forceResistance(int8_t requestResistance) {
    Q_UNUSED(requestResistance)
    /*
    uint8_t resistance[] = { 0xf0, 0xa6, 0x01, 0x01, 0x00, 0x00 };
    resistance[4] = requestResistance + 1;
    for(uint8_t i=0; i<sizeof(resistance)-1; i++)
    {
       resistance[5] += resistance[i]; // the last byte is a sort of a checksum
    }
    writeCharacteristic((uint8_t*)resistance, sizeof(resistance), "resistance " + QString::number(requestResistance),
    false, true);
    */
}

void sportstechbike::update() {
    // qDebug() << bike.isValid() << m_control->state() << gattCommunicationChannelService <<
    // gattWriteCharacteristic.isValid() << gattNotifyCharacteristic.isValid() << initDone;

    if (!m_control) {
        return;
    }

    if (m_control->state() == QLowEnergyController::UnconnectedState) {
        emit disconnected();
        return;
    }

    if (initRequest) {
        initRequest = false;
        btinit(false);
    } else if (bluetoothDevice.isValid() && m_control->state() == QLowEnergyController::DiscoveredState &&
               gattCommunicationChannelService && gattWriteCharacteristic.isValid() &&
               gattNotify1Characteristic.isValid() && initDone) {
        update_metrics(false, 0);

        // updating the bike console every second
        if (sec1update++ == (1000 / refresh->interval())) {
            sec1update = 0;
            // updateDisplay(elapsed);
        }

        QSettings settings;
        uint8_t noOpData[] = {0xf2, 0xc3, 0x07, 0x04, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xbe};
        if (requestResistance < 0) {
            requestResistance = 0;
        }
        if (requestResistance > 23) {
            requestResistance = 23;
        }
        noOpData[4] = requestResistance;
        noOpData[10] += requestResistance;
        writeCharacteristic((uint8_t *)noOpData, sizeof(noOpData), QStringLiteral("noOp"), false, true);
    }
}

void sportstechbike::serviceDiscovered(const QBluetoothUuid &gatt) {
    emit debug(QStringLiteral("serviceDiscovered ") + gatt.toString());
}

void sportstechbike::characteristicChanged(const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue) {
    // qDebug() << "characteristicChanged" << characteristic.uuid() << newValue << newValue.length();
    Q_UNUSED(characteristic);
    QSettings settings;
    QString heartRateBeltName =
        settings.value(QStringLiteral("heart_rate_belt_name"), QStringLiteral("Disabled")).toString();
    emit packetReceived();

    emit debug(QStringLiteral(" << ") + newValue.toHex(' '));

    lastPacket = newValue;
    if (newValue.length() != 20) {
        return;
    }

    double speed = GetSpeedFromPacket(newValue);
    double cadence = GetCadenceFromPacket(newValue);
    double resistance = GetResistanceFromPacket(newValue);
    double kcal = GetKcalFromPacket(newValue);
    double watt = GetWattFromPacket(newValue);

#ifdef Q_OS_ANDROID
    if (settings.value("ant_heart", false).toBool())
        Heart = (uint8_t)KeepAwakeHelper::heart();
    else
#endif
    {
        if (heartRateBeltName.startsWith(QStringLiteral("Disabled"))) {
            Heart = ((uint8_t)newValue.at(11));
        }
    }
    FanSpeed = 0;

    if (!firstCharChanged) {
        DistanceCalculated += ((speed / 3600.0) / (1000.0 / (lastTimeCharChanged.msecsTo(QTime::currentTime()))));
    }

    emit debug(QStringLiteral("Current speed: ") + QString::number(speed));
    emit debug(QStringLiteral("Current cadence: ") + QString::number(cadence));
    emit debug(QStringLiteral("Current resistance: ") + QString::number(resistance));
    emit debug(QStringLiteral("Current heart: ") + QString::number(Heart.value()));
    emit debug(QStringLiteral("Current KCal: ") + QString::number(kcal));
    emit debug(QStringLiteral("Current watt: ") + QString::number(watt));
    emit debug(QStringLiteral("Current Elapsed from the bike (not used): ") +
               QString::number(GetElapsedFromPacket(newValue)));
    emit debug(QStringLiteral("Current Distance Calculated: ") + QString::number(DistanceCalculated));

    if (m_control->error() != QLowEnergyController::NoError) {
        qDebug() << QStringLiteral("QLowEnergyController ERROR!!") << m_control->errorString();
    }

    if (!settings.value(QStringLiteral("speed_power_based"), false).toBool()) {
        Speed = speed;
    } else {
        Speed = metric::calculateSpeedFromPower(m_watt.value());
    }
    Resistance = requestResistance;
    emit resistanceRead(Resistance.value());
    KCal = kcal;
    Distance = DistanceCalculated;
    if (settings.value(QStringLiteral("cadence_sensor_name"), QStringLiteral("Disabled"))
            .toString()
            .startsWith(QStringLiteral("Disabled"))) {
        Cadence = cadence;
    }
    m_watt = watt;

    lastTimeCharChanged = QTime::currentTime();
    firstCharChanged = false;
}

uint16_t sportstechbike::GetElapsedFromPacket(const QByteArray &packet) {
    uint16_t convertedDataSec = (packet.at(4));
    uint16_t convertedDataMin = (packet.at(3));
    uint16_t convertedData = convertedDataMin * 60.f + convertedDataSec;
    return convertedData;
}

double sportstechbike::GetSpeedFromPacket(const QByteArray &packet) {
    uint16_t convertedData = (packet.at(12) << 8) | ((uint8_t)packet.at(13));
    double data = (double)(convertedData) / 10.0f;
    return data;
}

double sportstechbike::GetKcalFromPacket(const QByteArray &packet) {
    uint16_t convertedData = (packet.at(7) << 8) | ((uint8_t)packet.at(8));
    return (double)(convertedData);
}

double sportstechbike::GetWattFromPacket(const QByteArray &packet) {
    uint16_t convertedData = (packet.at(9) << 8) | ((uint8_t)packet.at(10));
    double data = ((double)(convertedData));
    return data;
}

double sportstechbike::GetCadenceFromPacket(const QByteArray &packet) {
    uint16_t convertedData = packet.at(17);
    double data = (convertedData);
    if (data < 0) {
        return 0;
    }
    return data;
}

double sportstechbike::GetResistanceFromPacket(const QByteArray &packet) {
    uint16_t convertedData = packet.at(15);
    double data = (convertedData);
    if (data < 0) {
        return 0;
    }
    return data;
}

void sportstechbike::btinit(bool startTape) {
    Q_UNUSED(startTape);
    QSettings settings;

    const uint8_t initData1[] = {0xf2, 0xc0, 0x00, 0xb2};
    const uint8_t initData2[] = {0xf2, 0xc1, 0x05, 0x01, 0xff, 0xff, 0xff, 0xff, 0xb5};
    const uint8_t initData3[] = {0xf2, 0xc4, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xc0};
    const uint8_t initData4[] = {0xf2, 0xc3, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xbb};

    writeCharacteristic((uint8_t *)initData1, sizeof(initData1), QStringLiteral("init"), false, true);
    writeCharacteristic((uint8_t *)initData2, sizeof(initData2), QStringLiteral("init"), false, true);
    writeCharacteristic((uint8_t *)initData3, sizeof(initData3), QStringLiteral("init"), false, true);
    writeCharacteristic((uint8_t *)initData4, sizeof(initData4), QStringLiteral("init"), false, true);

    initDone = true;
}

void sportstechbike::stateChanged(QLowEnergyService::ServiceState state) {
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceState>();
    emit debug(QStringLiteral("BTLE stateChanged ") + QString::fromLocal8Bit(metaEnum.valueToKey(state)));

    if (state == QLowEnergyService::ServiceDiscovered) {
        auto characteristics_list = gattCommunicationChannelService->characteristics();
        for (const QLowEnergyCharacteristic &c : qAsConst(characteristics_list)) {
            emit debug(QStringLiteral("characteristic ") + c.uuid().toString());
        }

        //        QString uuidWrite = "0000fff2-0000-1000-8000-00805f9b34fb";
        //        QString uuidNotify1 = "0000fff1-0000-1000-8000-00805f9b34fb";

        QBluetoothUuid _gattWriteCharacteristicId(QStringLiteral("0000fff2-0000-1000-8000-00805f9b34fb"));
        QBluetoothUuid _gattNotify1CharacteristicId(QStringLiteral("0000fff1-0000-1000-8000-00805f9b34fb"));

        gattWriteCharacteristic = gattCommunicationChannelService->characteristic(_gattWriteCharacteristicId);
        gattNotify1Characteristic = gattCommunicationChannelService->characteristic(_gattNotify1CharacteristicId);
        Q_ASSERT(gattWriteCharacteristic.isValid());
        Q_ASSERT(gattNotify1Characteristic.isValid());

        // establish hook into notifications
        connect(gattCommunicationChannelService, &QLowEnergyService::characteristicChanged, this,
                &sportstechbike::characteristicChanged);
        connect(gattCommunicationChannelService, &QLowEnergyService::characteristicWritten, this,
                &sportstechbike::characteristicWritten);
        connect(gattCommunicationChannelService,
                static_cast<void (QLowEnergyService::*)(QLowEnergyService::ServiceError)>(&QLowEnergyService::error),
                this, &sportstechbike::errorService);
        connect(gattCommunicationChannelService, &QLowEnergyService::descriptorWritten, this,
                &sportstechbike::descriptorWritten);

        // ******************************************* virtual bike init *************************************
        if (!firstVirtualBike && !virtualBike) {
            QSettings settings;
            bool virtual_device_enabled = settings.value(QStringLiteral("virtual_device_enabled"), true).toBool();
            if (virtual_device_enabled) {
                emit debug(QStringLiteral("creating virtual bike interface..."));
                virtualBike = new virtualbike(this, noWriteResistance, noHeartService);
                // connect(virtualBike,&virtualbike::debug ,this,&sportstechbike::debug);
            }
        }
        firstVirtualBike = 1;
        // ********************************************************************************************************

        QByteArray descriptor;
        descriptor.append((char)0x01);
        descriptor.append((char)0x00);
        gattCommunicationChannelService->writeDescriptor(
            gattNotify1Characteristic.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), descriptor);
    }
}

void sportstechbike::descriptorWritten(const QLowEnergyDescriptor &descriptor, const QByteArray &newValue) {
    emit debug(QStringLiteral("descriptorWritten ") + descriptor.name() + QStringLiteral(" ") + newValue.toHex(' '));

    initRequest = true;
    emit connectedAndDiscovered();
}

void sportstechbike::characteristicWritten(const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue) {
    Q_UNUSED(characteristic);
    emit debug(QStringLiteral("characteristicWritten ") + newValue.toHex(' '));
}

void sportstechbike::serviceScanDone(void) {
    emit debug(QStringLiteral("serviceScanDone"));

    // QString uuid = "0000fff0-0000-1000-8000-00805f9b34fb";

    QBluetoothUuid _gattCommunicationChannelServiceId(QStringLiteral("0000fff0-0000-1000-8000-00805f9b34fb"));
    gattCommunicationChannelService = m_control->createServiceObject(_gattCommunicationChannelServiceId);

    if (gattCommunicationChannelService == nullptr) {
        qDebug() << QStringLiteral("invalid service") << _gattCommunicationChannelServiceId.toString();
        return;
    }

    connect(gattCommunicationChannelService, &QLowEnergyService::stateChanged, this, &sportstechbike::stateChanged);
    gattCommunicationChannelService->discoverDetails();
}

void sportstechbike::errorService(QLowEnergyService::ServiceError err) {
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceError>();
    emit debug(QStringLiteral("sportstechbike::errorService") + QString::fromLocal8Bit(metaEnum.valueToKey(err)) +
               m_control->errorString());
}

void sportstechbike::error(QLowEnergyController::Error err) {
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyController::Error>();
    emit debug(QStringLiteral("sportstechbike::error") + QString::fromLocal8Bit(metaEnum.valueToKey(err)) +
               m_control->errorString());
}

void sportstechbike::deviceDiscovered(const QBluetoothDeviceInfo &device) {
    emit debug(QStringLiteral("Found new device: ") + device.name() + QStringLiteral(" (") +
               device.address().toString() + ')');
    {
        bluetoothDevice = device;
        m_control = QLowEnergyController::createCentral(bluetoothDevice, this);
        connect(m_control, &QLowEnergyController::serviceDiscovered, this, &sportstechbike::serviceDiscovered);
        connect(m_control, &QLowEnergyController::discoveryFinished, this, &sportstechbike::serviceScanDone);
        connect(m_control,
                static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, &sportstechbike::error);
        connect(m_control, &QLowEnergyController::stateChanged, this, &sportstechbike::controllerStateChanged);

        connect(m_control,
                static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, [this](QLowEnergyController::Error error) {
                    Q_UNUSED(error);
                    Q_UNUSED(this);
                    emit debug(QStringLiteral("Cannot connect to remote device."));
                    emit disconnected();
                });
        connect(m_control, &QLowEnergyController::connected, this, [this]() {
            Q_UNUSED(this);
            emit debug(QStringLiteral("Controller connected. Search services..."));
            m_control->discoverServices();
        });
        connect(m_control, &QLowEnergyController::disconnected, this, [this]() {
            Q_UNUSED(this);
            emit debug(QStringLiteral("LowEnergy controller disconnected"));
            emit disconnected();
        });

        // Connect
        m_control->connectToDevice();
        return;
    }
}

uint16_t sportstechbike::watts() {
    if (currentCadence().value() == 0) {
        return 0;
    }

    return m_watt.value();
}

bool sportstechbike::connected() {
    if (!m_control) {
        return false;
    }
    return m_control->state() == QLowEnergyController::DiscoveredState;
}

void *sportstechbike::VirtualBike() { return virtualBike; }

void *sportstechbike::VirtualDevice() { return VirtualBike(); }

double sportstechbike::odometer() { return DistanceCalculated; }

void sportstechbike::controllerStateChanged(QLowEnergyController::ControllerState state) {
    qDebug() << QStringLiteral("controllerStateChanged") << state;
    if (state == QLowEnergyController::UnconnectedState && m_control) {
        qDebug() << QStringLiteral("trying to connect back again...");
        initDone = false;
        m_control->connectToDevice();
    }
}
