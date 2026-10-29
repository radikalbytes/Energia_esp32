#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <DHT.h>

// ---------- WiFi / API ----------
// Las credenciales están en include/secrets.h (ignorado por git; ver secrets.example.h).
#include "secrets.h"
const char* WIFI_SSID = WIFI_SSID_VALUE;
const char* WIFI_PASSWORD = WIFI_PASSWORD_VALUE;

// Local: sustituye por la IP de tu ordenador.
// Producción: https://energy-monitoring.vercel.app/api/data
const char* API_URL = "https://energy-monitoring.vercel.app/api/data"; //"http://192.168.1.100:3000/api/data";

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
#define HUMIDITY_MAX_VALID 95.0f  // por encima, el sensor suele estar saturado

// ---------- ADS1115 + SCT-013-030 ----------
Adafruit_ADS1115 ads;
#define SCT_CHANNEL 0

// El SCT-013-030 (versión 3.5mm) entrega 1 Vrms a 30 A (burden resistor integrado).
// El ADS1115 solo mide 0-Vref, por lo que la señal debe polarizarse a mitad de
// escala (~1.65V) con un divisor resistivo + condensador antes de entrar al ADC.
const float CT_CALIBRATION = 30.0; // Amperios por voltio leído
const float MAINS_VOLTAGE = 230.0; // Tensión de red asumida (no se mide)

const unsigned long MEASURE_WINDOW_MS = 1000; // 1 s = 50 ciclos exactos de 50 Hz
const float NOISE_FLOOR_A = 0.05;              // por debajo se considera ruido (0 A)

const unsigned long SERIAL_INTERVAL_MS = 30000; // medida + informe por puerto serie
const unsigned long SEND_INTERVAL_MS = 60000;   // envío al servidor

void conectarWifi() {
  Serial.printf("Conectando a WiFi '%s'", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWiFi conectado. IP: %s, RSSI: %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

struct CurrentStats {
  float irms;     // A (ya filtrado el ruido)
  float irmsRaw;  // A sin filtrar
  float vrms;     // V RMS de la componente alterna en el ADC
  float offset;   // V de polarización (media de la señal)
  float vMin, vMax;
  unsigned samples;
  float sps;
};

// RMS de la componente alterna en una ventana de tiempo fija (ciclos enteros de red):
// Vrms = sqrt(E[x^2] - E[x]^2). Elimina el offset DC exacto de la ventana, sin
// depender de un filtro que tarda en converger tras arrancar.
CurrentStats medirCorriente(unsigned long windowMs) {
  double sum = 0, sumSq = 0;
  float vMin = 99, vMax = -99;
  unsigned n = 0;
  unsigned long t0 = millis();

  while (millis() - t0 < windowMs) {
    float v = ads.computeVolts(ads.readADC_SingleEnded(SCT_CHANNEL));
    sum += v;
    sumSq += (double)v * v;
    if (v < vMin) vMin = v;
    if (v > vMax) vMax = v;
    n++;
  }
  unsigned long elapsed = millis() - t0;

  CurrentStats r = {};
  if (n == 0) return r;
  double mean = sum / n;
  double var = sumSq / n - mean * mean;
  if (var < 0) var = 0;
  r.vrms = sqrt(var);
  r.offset = mean;
  r.vMin = vMin;
  r.vMax = vMax;
  r.samples = n;
  r.sps = n * 1000.0f / elapsed;
  r.irmsRaw = r.vrms * CT_CALIBRATION;
  r.irms = r.irmsRaw < NOISE_FLOOR_A ? 0 : r.irmsRaw;
  return r;
}

void informarCorriente(const CurrentStats& c) {
  Serial.printf("[I] Irms=%.3f A (bruta %.3f) | P=%.1f W | Vac=%.1f mV | offset=%.3f V | rango=%.3f..%.3f V | %u muestras (%.0f SPS)\n",
                c.irms, c.irmsRaw, c.irms * MAINS_VOLTAGE, c.vrms * 1000.0f, c.offset, c.vMin, c.vMax, c.samples, c.sps);
  if (c.samples == 0) {
    Serial.println("[!] Sin muestras del ADS1115, revisa el I2C");
  } else {
    if (c.offset < 1.4f || c.offset > 1.9f) {
      Serial.println("[!] Offset lejos de 1.65 V: revisa el divisor de polarizacion del SCT-013");
    }
    if (c.vMin < 0.05f || c.vMax > 3.25f) {
      Serial.println("[!] Senal cerca de saturacion/recorte (corriente excesiva o polarizacion incorrecta)");
    }
    if (c.sps < 300) {
      Serial.println("[!] Frecuencia de muestreo baja, la medida puede ser imprecisa");
    }
  }
}

bool enviarMedicion(float irms, float potencia, float temperatura, float humedad) {
  if (WiFi.status() != WL_CONNECTED) {
    conectarWifi();
  }

  HTTPClient http;
  http.begin(API_URL);
  http.setTimeout(10000);
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
  delay(2000); // da tiempo a que el monitor USB se conecte
  Serial.println("Arrancando...");
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
  static unsigned long nextMeasure = 0;
  static unsigned long lastSend = 0;
  static double irmsSum = 0;
  static unsigned irmsCount = 0;

  if ((long)(millis() - nextMeasure) >= 0) {
    nextMeasure = millis() + SERIAL_INTERVAL_MS;
    CurrentStats c = medirCorriente(MEASURE_WINDOW_MS);
    informarCorriente(c);
    irmsSum += c.irms;
    irmsCount++;
  }

  if (irmsCount > 0 && millis() - lastSend >= SEND_INTERVAL_MS) {
    lastSend = millis();
    float irms = irmsSum / irmsCount;
    float potencia = irms * MAINS_VOLTAGE;
    irmsSum = 0;
    irmsCount = 0;

    float temperatura = dht.readTemperature();
    float humedad = dht.readHumidity();

    if (isnan(temperatura) || isnan(humedad)) {
      Serial.println("Error leyendo el DHT22");
      temperatura = 0;
      humedad = 0;
    } else if (temperatura < -40 || temperatura > 80 || humedad < 0 || humedad > HUMIDITY_MAX_VALID) {
      // Humedad saturada o fuera de rango: sensor mojado/defectuoso. Se envía 0 (= sin dato).
      Serial.printf("Humedad no fiable (%.1f%%), se descarta\n", humedad);
      humedad = 0;
    }

    enviarMedicion(irms, potencia, temperatura, humedad);
  }

  delay(10);
}
