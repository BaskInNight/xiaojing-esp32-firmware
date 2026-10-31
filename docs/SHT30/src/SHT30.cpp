#include "SHT30.h"
#include <Arduino.h>
#include <Wire.h>
SHT30::SHT30(uint8_t addr) { _addr = addr; }

SHT30::SHT30() { _addr = 0x44; }

SHT30::~SHT30() {}

void SHT30::begin() { Wire.begin(); }

uint32_t SHT30::requestMeasurement() {
  Wire.beginTransmission(_addr);
  Wire.write(0x2c);
  Wire.write(0x06);
  Wire.endTransmission();
  return 15;
}

bool SHT30::getResult() {
  Wire.requestFrom(_addr, (uint8_t)6);
  if (Wire.available() == 6) {
    for (uint8_t i = 0; i < 6; i++) _data[i] = Wire.read();
    return true;
  } else
    return false;
}

double SHT30::getTempInC() {
  if (_crc) {
    uint8_t temp[2];
    temp[0] = _data[0];
    temp[1] = _data[1];
    if (_getCRC(temp) == _data[2])
      return ((((_data[0] * 256.0) + _data[1]) * 175) / 65535.0) - 45;
    else
      return -273.15;
  }
  return ((((_data[0] * 256.0) + _data[1]) * 175) / 65535.0) - 45;
}

double SHT30::getHumi() {
  if (_crc) {
    uint8_t temp[2];
    temp[0] = _data[3];
    temp[1] = _data[4];
    if (_getCRC(temp) == _data[5])
      return ((((_data[3] * 256.0) + _data[4]) * 100) / 65535.0);
    else
      return -1;
  }
  return ((((_data[3] * 256.0) + _data[4]) * 100) / 65535.0);
}

bool SHT30::enableCRC() { return _crc; }

bool SHT30::enableCRC(bool enable) {
  _crc = enable;
  return _crc;
}

uint8_t SHT30::_getCRC(uint8_t* data) {
  uint8_t bit;
  uint8_t crc = 0xFF;
  for (uint8_t dataCounter = 0; dataCounter < 2; dataCounter++) {
    crc ^= (data[dataCounter]);
    for (bit = 8; bit > 0; --bit) {
      if (crc & 0x80)
        crc = (crc << 1) ^ 0x131;
      else
        crc = (crc << 1);
    }
  }

  return crc;
}
