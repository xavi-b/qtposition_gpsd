TARGET = qtposition_gpsd
QT = core network positioning

unix {
target.path = $$[QT_INSTALL_PLUGINS]/position/
INSTALLS += target
}

TEMPLATE = lib
CONFIG += plugin

HEADERS += \
    gpsdmasterdevice.h \
    qgeopositioninfosource_gpsd.h \
    qgeopositioninfosourcefactory_gpsd.h \
    qgeosatelliteinfosource_gpsd.h

SOURCES += \
    gpsdmasterdevice.cpp \
    qgeopositioninfosource_gpsd.cpp \
    qgeopositioninfosourcefactory_gpsd.cpp \
    qgeosatelliteinfosource_gpsd.cpp

OTHER_FILES += plugin.json
