#ifndef _SHT30_H_
#define _SHT30_H_

#include <Arduino.h>
#include <Wire.h>
class SHT30 {
 public:
  SHT30(uint8_t addr);
  SHT30();
  ~SHT30();
  uint32_t requestMeasurement();
  bool getResult();
  void begin();
  bool enableCRC(bool enable);
  bool enableCRC();
  double getTempInC();
  double getHumi();

 private:
  uint8_t _getCRC(uint8_t * data);
  uint8_t _data[6] = {0};
  uint8_t _addr;
  bool _crc = true;
};

#endif
