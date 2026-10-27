// Global objects the Arduino/M5 compat headers declare.
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>

HardwareSerial Serial;
EspClass ESP;
M5CardputerClass M5Cardputer;
M5CardputerClass &M5 = M5Cardputer;
SDClass SD;
SPIClass SPI;
WiFiClass WiFi;
