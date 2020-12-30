#include "heartratebelt.h"
#include <QFile>
#include <QThread>
#include <QDateTime>
#include <QMetaEnum>
#include <QEventLoop>
#include <QBluetoothLocalDevice>
#include <QSettings>

heartratebelt::heartratebelt()
{

}

void heartratebelt::update()
{

}

void heartratebelt::serviceDiscovered(const QBluetoothUuid &gatt)
{
    debug("serviceDiscovered " + gatt.toString());
}

void heartratebelt::characteristicChanged(const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue)
{
    //qDebug() << "characteristicChanged" << characteristic.uuid() << newValue << newValue.length();
    Q_UNUSED(characteristic);
    emit packetReceived();

    debug(" << " + newValue.toHex(' '));

    if(newValue.length() > 1)
    {
        Heart = newValue[1];
        emit heartRate(Heart);
    }

    debug("Current heart: " + QString::number(Heart));
}

void heartratebelt::stateChanged(QLowEnergyService::ServiceState state)
{
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceState>();
    debug("BTLE stateChanged " + QString::fromLocal8Bit(metaEnum.valueToKey(state)));

    if(state == QLowEnergyService::ServiceDiscovered)
    {
        foreach(QLowEnergyCharacteristic c,gattCommunicationChannelService->characteristics())
        {
            debug("characteristic " + c.uuid().toString());
        }

        gattNotifyCharacteristic = gattCommunicationChannelService->characteristic(QBluetoothUuid(QBluetoothUuid::HeartRateMeasurement));
        Q_ASSERT(gattNotifyCharacteristic.isValid());

        // establish hook into notifications
        connect(gattCommunicationChannelService, SIGNAL(characteristicChanged(QLowEnergyCharacteristic,QByteArray)),
                this, SLOT(characteristicChanged(QLowEnergyCharacteristic,QByteArray)));
        connect(gattCommunicationChannelService, SIGNAL(characteristicWritten(const QLowEnergyCharacteristic, const QByteArray)),
                this, SLOT(characteristicWritten(const QLowEnergyCharacteristic, const QByteArray)));
        connect(gattCommunicationChannelService, SIGNAL(error(QLowEnergyService::ServiceError)),
                this, SLOT(errorService(QLowEnergyService::ServiceError)));
        connect(gattCommunicationChannelService, SIGNAL(descriptorWritten(const QLowEnergyDescriptor, const QByteArray)), this,
                SLOT(descriptorWritten(const QLowEnergyDescriptor, const QByteArray)));

        QByteArray descriptor;
        descriptor.append((char)0x01);
        descriptor.append((char)0x00);
        gattCommunicationChannelService->writeDescriptor(gattNotifyCharacteristic.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), descriptor);
    }
}

void heartratebelt::descriptorWritten(const QLowEnergyDescriptor &descriptor, const QByteArray &newValue)
{
    debug("descriptorWritten " + descriptor.name() + " " + newValue.toHex(' '));
}

void heartratebelt::characteristicWritten(const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue)
{
    Q_UNUSED(characteristic);
    debug("characteristicWritten " + newValue.toHex(' '));
}

void heartratebelt::serviceScanDone(void)
{
    debug("serviceScanDone");

    QBluetoothUuid _gattCommunicationChannelServiceId(QBluetoothUuid::HeartRate);
    gattCommunicationChannelService = m_control->createServiceObject(_gattCommunicationChannelServiceId);
    connect(gattCommunicationChannelService, SIGNAL(stateChanged(QLowEnergyService::ServiceState)), this, SLOT(stateChanged(QLowEnergyService::ServiceState)));
    gattCommunicationChannelService->discoverDetails();
}

void heartratebelt::errorService(QLowEnergyService::ServiceError err)
{
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceError>();
    debug("heartratebelt::errorService" + QString::fromLocal8Bit(metaEnum.valueToKey(err)) + m_control->errorString());
}

void heartratebelt::error(QLowEnergyController::Error err)
{
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyController::Error>();
    debug("heartratebelt::error" + QString::fromLocal8Bit(metaEnum.valueToKey(err)) + m_control->errorString());
    m_control->disconnect();
}

void heartratebelt::deviceDiscovered(const QBluetoothDeviceInfo &device)
{
    QSettings settings;
    QString heartRateBeltName = settings.value("heart_rate_belt_name", "Disabled").toString();
    debug("Found new device: " + device.name() + " (" + device.address().toString() + ')');
    if(device.name().startsWith(heartRateBeltName))
    {
        bluetoothDevice = device;
        m_control = QLowEnergyController::createCentral(bluetoothDevice, this);
        connect(m_control, SIGNAL(serviceDiscovered(const QBluetoothUuid &)),
                this, SLOT(serviceDiscovered(const QBluetoothUuid &)));
        connect(m_control, SIGNAL(discoveryFinished()),
                this, SLOT(serviceScanDone()));
        connect(m_control, SIGNAL(error(QLowEnergyController::Error)),
                this, SLOT(error(QLowEnergyController::Error)));

        connect(m_control, static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, [this](QLowEnergyController::Error error) {
            Q_UNUSED(error);
            Q_UNUSED(this);
            debug("Cannot connect to remote device.");
            emit disconnected();
        });
        connect(m_control, &QLowEnergyController::connected, this, [this]() {
            Q_UNUSED(this);
            debug("Controller connected. Search services...");
            m_control->discoverServices();
        });
        connect(m_control, &QLowEnergyController::disconnected, this, [this]() {
            Q_UNUSED(this);
            debug("LowEnergy controller disconnected");
            emit disconnected();
        });

        // Connect
        m_control->connectToDevice();
        return;
    }
}

bool heartratebelt::connected()
{
    if(!m_control)
        return false;
    return m_control->state() == QLowEnergyController::DiscoveredState;
}