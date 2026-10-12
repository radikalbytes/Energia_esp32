#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <DHT.h>

// ---------- WiFi / API ----------
const char* WIFI_SSID = "TU_WIFI";
const char* WIFI_PASSWORD = "TU_CONTRASENA";

// Local: sustituye por la IP de tu ordenador.
// Producción: https://energy-monitoring.vercel.app/api/data
const char* API_URL = "http://192.168.1.100:3000/api/data";

#define PROCESSOR_ESP32_C3 1
#define PROCESSOR_ESP32_S3 2

#ifndef PROCESSOR_TYPE
#error "Define PROCESSOR_TYPE in platformio.ini (1=C3, 2=S3)"
#elif PROCESSOR_TYPE == PROCESSOR_ESP32_C3
const char* DEVICE_UUID = "esp32-c3-salon-001";
#elif PROCESSOR_TYPE == PROCESSOR_ESP32_S3
const char* DEVICE_UUID = "esp32-s3-salon-001";
#else
#error "Unsupported PROCESSOR_TYPE (use 1=C3 or 2=S3)"
#endif

// ---------- DHT22 ----------
#define DHT_PIN 4
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);

// ---------- ADS1115 + SCT-013-030 ----------
Adafruit_ADS1115 ads;
#define SCT_CHANNEL 0

// El SCT-013-030 (versión 3.5mm) entrega 1 Vrms a 30 A (burden resistor integrado).
// El ADS1115 solo mide 0-Vref, por lo que la señal debe polarizarse a mitad de
// escala (~1.65V) con un divisor resistivo + condensador antes de entrar al ADC.
const float CT_CALIBRATION = 30.0; // Amperios por voltio leído
const float MAINS_VOLTAGE = 230.0; // Tensión de red asumida (no se mide)

const unsigned int SAMPLES = 1000; // ~1.2s a 860 SPS, suficiente para varios ciclos de 50Hz

float adcOffset = 1.65; // se autoajusta con un filtro paso-alto en calcIrms()

void conectarWifi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
}

// Calcula la corriente RMS eliminando el offset DC mediante un filtro paso-alto,
// igual que hace la librería EmonLib de OpenEnergyMonitor.
float calcIrms(unsigned int numberOfSamples) {
  double sumSq = 0;

  for (unsigned int i = 0; i < numberOfSamples; i++) {
    int16_t raw = ads.readADC_SingleEnded(SCT_CHANNEL);
    double sample = ads.computeVolts(raw);

    adcOffset += (sample - adcOffset) / 1024.0;
    double filteredSample = sample - adcOffset;

    sumSq += filteredSample * filteredSample;
  }

  double rmsVoltage = sqrt(sumSq / numberOfSamples);
  return rmsVoltage * CT_CALIBRATION;
}

bool enviarMedicion(float irms, float potencia, float temperatura, float humedad) {
  if (WiFi.status() != WL_CONNECTED) {
    conectarWifi();
  }

  HTTPClient http;
  http.begin(API_URL);
  http.addHeader("Content-Type", "application/json");

  String payload = "{";
  payload += "\"uuid\":\"" + String(DEVICE_UUID) + "\",";
  payload += "\"irms\":" + String(irms, 2) + ",";
  payload += "\"power\":" + String(potencia, 2) + ",";
  payload += "\"temperature\":" + String(temperatura, 1) + ",";
  payload += "\"humidity\":" + String(humedad, 1);
  payload += "}";

  int httpCode = http.POST(payload);
  String response = http.getString();
  http.end();

  Serial.printf("POST: %d\n", httpCode);
  Serial.println(response);

  return httpCode == 201;
}

void setup() {
  Serial.begin(115200);
  conectarWifi();

  dht.begin();

  if (!ads.begin()) {
    Serial.println("No se detectó el ADS1115, revisa el cableado I2C");
    while (true) {
      delay(1000);
    }
  }
  ads.setGain(GAIN_ONE);              // +/-4.096V, adecuado para una señal centrada en ~1.65V
  ads.setDataRate(RATE_ADS1115_860SPS);
}

void loop() {
  float irms = calcIrms(SAMPLES);
  float potencia = irms * MAINS_VOLTAGE;

  float temperatura = dht.readTemperature();
  float humedad = dht.readHumidity();

  if (isnan(temperatura) || isnan(humedad)) {
    Serial.println("Error leyendo el DHT22");
    temperatura = 0;
    humedad = 0;
  }

  enviarMedicion(irms, potencia, temperatura, humedad);

  delay(300000); // 5 minutos
}
