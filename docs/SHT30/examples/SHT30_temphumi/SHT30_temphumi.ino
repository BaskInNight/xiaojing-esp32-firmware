#include <SHT30.h>
SHT30 sht30(0x45);
void setup(){
	Serial.begin(9600);
	Serial.println();
	sht30.begin();
	sht30.enableCRC(true);
	Serial.print("CRC state:");
	Serial.println(sht30.enableCRC());
}

void loop(){
	long interval=sht30.requestMeasurement();
	delay(interval);
	if(sht30.getResult()){
		Serial.print("Temp:");
		Serial.print(sht30.getTempInC());
		Serial.println(" C");
		Serial.print("Humi:");
		Serial.print(sht30.getHumi());
		Serial.println("%");
	}else
		Serial.println("Measurement failed!");
	delay(1000);
}
