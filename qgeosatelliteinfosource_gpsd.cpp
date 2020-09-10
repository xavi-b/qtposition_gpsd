/*
  The MIT License (MIT)

  Copyright (c) 2016 Jörg Mechnich <joerg.mechnich@gmail.com>

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:
  
  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.
  
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
  LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "qgeosatelliteinfosource_gpsd.h"

#include "gpsdmasterdevice.h"

#include <QGeoSatelliteInfo>
#include <QIODevice>
#include <QDebug>
#include <QTimer>

namespace 
{

// from qlocationutils.cpp
bool hasValidNmeaChecksum(const char *data, int size)
{
    int asteriskIndex = -1;
    for (int i = 0; i < size; ++i)
    {
        if (data[i] == '*')
        {
            asteriskIndex = i;
            break;
        }
    }

    const int CSUM_LEN = 2;
    if (asteriskIndex < 0 || asteriskIndex + CSUM_LEN >= size)
        return false;

    // XOR byte value of all characters between '$' and '*'
    int result = 0;
    for (int i = 1; i < asteriskIndex; ++i)
        result ^= data[i];

    QByteArray checkSumBytes(&data[asteriskIndex + 1], 2);
    bool ok = false;
    int checksum = checkSumBytes.toInt(&ok,16);
    return ok && checksum == result;
}

}

QGeoSatelliteInfoSourceGpsd::QGeoSatelliteInfoSourceGpsd(QObject* parent)
    : QGeoSatelliteInfoSource(parent)
    , _device(0)
    , _lastError(QGeoSatelliteInfoSource::NoError)
    , _running(false)
    , _wasRunning(false)
    , _reqDone(0)
    , _reqTimer(new QTimer(this))
{
    _reqTimer->setSingleShot(true);
    connect(_reqTimer,SIGNAL(timeout()),this, SLOT(reqTimerTimeout()));
}

void
QGeoSatelliteInfoSourceGpsd::reqTimerTimeout()
{
    if(!_wasRunning)
        stopUpdates();

    if(_reqDone != (ReqSatellitesInView | ReqSatellitesInUse))
        emit requestTimeout();
}

QGeoSatelliteInfoSource::Error
QGeoSatelliteInfoSourceGpsd::error() const
{
    return _lastError;
}

int
QGeoSatelliteInfoSourceGpsd::minimumUpdateInterval() const
{
    return 5000;
}

void
QGeoSatelliteInfoSourceGpsd::requestUpdate(int timeout)
{
    if( _reqTimer->isActive())
        return;

    if( timeout == 0)
        timeout = minimumUpdateInterval();

    if(timeout < minimumUpdateInterval())
    {
        emit requestTimeout();
        return;
    }

    _wasRunning = _running;
    _reqDone = 0;

    if(!_running)
        startUpdates();
    _reqTimer->start(timeout);
}

QGeoSatelliteInfoSourceGpsd::~QGeoSatelliteInfoSourceGpsd()
{
    if(_running)
        stopUpdates();
}

void QGeoSatelliteInfoSourceGpsd::startUpdates()
{
    if(!_running)
    {
        _device = GpsdMasterDevice::instance()->createSlave();
        if(!_device)
        {
            _lastError = QGeoSatelliteInfoSource::AccessError;
            emit QGeoSatelliteInfoSource::error(_lastError);
            return;
        }

        connect(_device,SIGNAL(readyRead()),this,SLOT(tryReadLine()));
        GpsdMasterDevice::instance()->unpauseSlave(_device);
        _running = true;
    }
}

void QGeoSatelliteInfoSourceGpsd::stopUpdates()
{
    if(_running)
    {
        disconnect(_device,SIGNAL(readyRead()),this,SLOT(tryReadLine()));
        GpsdMasterDevice::instance()->pauseSlave(_device);
        _running = false;
        GpsdMasterDevice::instance()->destroySlave(_device);
        _device = 0;
    }
}

void QGeoSatelliteInfoSourceGpsd::tryReadLine()
{
    while(_device->canReadLine())
    {
        QByteArray buf(_device->readLine());
        parseNmeaData(buf, buf.size());
    }
}

void QGeoSatelliteInfoSourceGpsd::readGSV(const char *data, int size)
{
    static QMap<int,QGeoSatelliteInfo> sats;
    /*
    $GPGSV,2,1,08,01,40,083,46,02,17,308,41,12,07,344,39,14,22,228,45*75
    Where:
    
      GSV          Satellites in view
      2            Number of sentences for full data
      1            sentence 1 of 2
      08           Number of satellites in view
      
      01           Satellite PRN number
      40           Elevation, degrees
      083          Azimuth, degrees
      46           SNR - higher is better
         for up to 4 satellites per sentence
      *75          the checksum data, always begins with *
  */
    QList<QByteArray> parts = QByteArray::fromRawData(data,size).split(',');

    int senMax = parts[1].toUInt();
    int senIdx = parts[2].toUInt();
    int nSats  = parts[3].toUInt();

    if( senIdx == 1)
        sats.clear();

    int pos = 4;
    for( int sat=0; (sat+1)*4 < parts.size()-3; ++sat)
    {
        QGeoSatelliteInfo info;
        int prn = parts[pos++].toUInt();
        int ele = parts[pos++].toUInt();
        int azi = parts[pos++].toUInt();
        int snr = parts[pos++].toUInt();
        info.setSatelliteSystem(QGeoSatelliteInfo::GPS);
        info.setSatelliteIdentifier(prn);
        info.setAttribute(QGeoSatelliteInfo::Elevation, ele);
        info.setAttribute(QGeoSatelliteInfo::Azimuth, azi);
        info.setSignalStrength(snr);
        sats[prn] = info;
    }

    // last sentence
    if( senIdx == senMax)
    {
        if( sats.size() != nSats)
            qInfo() << "nSats != sats.size()!" << nSats << sats.size();
        _satellitesInView = sats;

        bool emitSignal = true;
        if(_reqTimer->isActive())
        {
            if(!(_reqDone & ReqSatellitesInView))
                _reqDone |= ReqSatellitesInView;
            if(!_wasRunning)
                emitSignal = false;
        }

        if(emitSignal)
            emit satellitesInViewUpdated(_satellitesInView.values());
    }
}

void QGeoSatelliteInfoSourceGpsd::readGSA(const char *data, int size)
{
    /*
    $GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39

    Where:
      GSA      Satellite status
      A        Auto selection of 2D or 3D fix (M = manual)
      3        3D fix - values include: 1 = no fix
                                        2 = 2D fix
                                        3 = 3D fix
      04,05... PRNs of satellites used for fix (space for 12)
      2.5      PDOP (dilution of precision)
      1.3      Horizontal dilution of precision (HDOP)
      2.1      Vertical dilution of precision (VDOP)
      *39      the checksum data, always begins with *
  */

    if(!_satellitesInView.size()) return;

    QList<QByteArray> parts = QByteArray::fromRawData(data, size).split(',');
    //int fixType = parts[2].toUInt();

    QSet<int> satsInUse;
    for(int i=3; i<15; ++i)
    {
        if( parts[i].isEmpty()) continue;
        int prn = parts[i].toUInt();
        satsInUse.insert(prn);
    }

    QList<QGeoSatelliteInfo> satellitesInUse;
    QSetIterator<int> setIt(satsInUse);
    while(setIt.hasNext())
    {
        int prn = setIt.next();
        QMap<int,QGeoSatelliteInfo>::const_iterator mapIt =
                _satellitesInView.find(prn);
        if( mapIt == _satellitesInView.end())
            qInfo() << "Used sat" << prn << "not found";
        else
            satellitesInUse.append(mapIt.value());
    }

    if(satellitesInUse.size() == satsInUse.size())
    {
        bool emitSignal = true;
        if(_reqTimer->isActive())
        {
            if(!(_reqDone & ReqSatellitesInUse))
                _reqDone |= ReqSatellitesInUse;

            if(_reqDone == (ReqSatellitesInUse | ReqSatellitesInView))
            {
                _reqTimer->stop();
                if(!_wasRunning)
                    QTimer::singleShot(0, this, SLOT(stopUpdates()));
                emit satellitesInViewUpdated( _satellitesInView.values());
            }
            else if(!_wasRunning)
                emitSignal = false;
        }
        if(emitSignal)
        {
            emit satellitesInUseUpdated( satellitesInUse);
        }
    }
}

bool QGeoSatelliteInfoSourceGpsd::parseNmeaData(const char *data, int size)
{
    if (size < 6 || data[0] != '$' || !hasValidNmeaChecksum(data, size))
        return false;

    // subtract checksum from data size
    for (int i = 0; i < size; ++i)
    {
        if (data[i] == '*')
        {
            size = i;
            break;
        }
    }

    if (data[3] == 'G' && data[4] == 'S' && data[5] == 'A')
    {
        // detailed satellite data
        readGSA(data, size);
        return true;
    }
    else if (data[3] == 'G' && data[4] == 'S' && data[5] == 'V')
    {
        // detailed satellite data
        readGSV(data, size);
        return true;
    }

    return false;
}
