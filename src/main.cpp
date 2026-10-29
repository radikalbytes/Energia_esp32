#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
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

// ---------- Portal de configuración WiFi ----------
const char* AP_NAME = "Voltio_monitor";
const unsigned long PORTAL_WINDOW_MS = 60000; // el punto de acceso está activo 60 s tras el reset
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;

Preferences prefs;
String wifiSsid;
String wifiPass;

void cargarCredenciales() {
  prefs.begin("wifi", true);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  prefs.end();
  if (wifiSsid.isEmpty()) { // valores por defecto de secrets.h
    wifiSsid = WIFI_SSID;
    wifiPass = WIFI_PASSWORD;
  }
}

void guardarCredenciales(const String& ssid, const String& pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

String htmlEscape(String v) {
  v.replace("&", "&amp;");
  v.replace("<", "&lt;");
  v.replace(">", "&gt;");
  v.replace("\"", "&quot;");
  return v;
}

String paginaPortal(int n) {
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>Voltio</title><style>"
             "body{font-family:system-ui;background:#0f172a;color:#e2e8f0;max-width:420px;margin:0 auto;padding:20px}"
             "h1{color:#facc15}select,input,button{width:100%;padding:12px;margin:8px 0;border-radius:10px;border:0;font-size:16px;box-sizing:border-box}"
             "button{background:#facc15;font-weight:700}a{color:#facc15}</style></head><body>"
             "<h1>&#9889; Voltio monitor</h1><p>Elige tu red WiFi:</p>"
             "<form method='POST' action='/save'><select name='ssid'>";
  for (int i = 0; i < n; i++) {
    String name = WiFi.SSID(i);
    if (name.isEmpty()) continue;
    h += "<option value=\"" + htmlEscape(name) + "\">" + htmlEscape(name) + " (" + String(WiFi.RSSI(i)) + " dBm" +
         (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "" : ", &#128274;") + ")</option>";
  }
  h += "</select><input type='password' name='pass' placeholder='Contrase&ntilde;a'>"
       "<button type='submit'>Guardar y conectar</button></form>"
       "<p><a href='/'>Buscar redes de nuevo</a></p></body></html>";
  return h;
}

void portalConfiguracion() {
  WebServer server(80);
  DNSServer dns;
  bool guardado = false;
  int redes = 0;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_NAME);
  dns.start(53, "*", WiFi.softAPIP());
  Serial.printf("Portal WiFi '%s' activo %lu s en http://%s\n", AP_NAME, PORTAL_WINDOW_MS / 1000,
                WiFi.softAPIP().toString().c_str());

  redes = WiFi.scanNetworks();

  server.on("/", HTTP_GET, [&]() {
    redes = WiFi.scanNetworks();
    server.send(200, "text/html", paginaPortal(redes));
  });
  server.on("/save", HTTP_POST, [&]() {
    String ssid = server.arg("ssid");
    if (ssid.isEmpty()) {
      server.send(400, "text/plain", "Falta la red");
      return;
    }
    guardarCredenciales(ssid, server.arg("pass"));
    wifiSsid = ssid;
    wifiPass = server.arg("pass");
    guardado = true;
    server.send(200, "text/html",
                "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<body style='font-family:system-ui;background:#0f172a;color:#e2e8f0;padding:20px'>"
                "<h2>Guardado</h2><p>El monitor se conecta ahora a la red elegida.</p></body>");
  });
  server.onNotFound([&]() { // portal cautivo: redirige todo a la página de configuración
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();

  unsigned long fin = millis() + PORTAL_WINDOW_MS;
  unsigned long finGuardado = 0;
  while (true) {
    dns.processNextRequest();
    server.handleClient();
    if (guardado && finGuardado == 0) finGuardado = millis() + 2000; // deja enviar la respuesta
    if (finGuardado && (long)(millis() - finGuardado) >= 0) break;
    // Si hay alguien conectado al portal, no se cierra hasta que termine
    if (WiFi.softAPgetStationNum() > 0) fin = millis() + PORTAL_WINDOW_MS;
    if (!guardado && (long)(millis() - fin) >= 0) break;
    delay(2);
  }
  server.stop();
  dns.stop();
  WiFi.scanDelete();
  WiFi.softAPdisconnect(true);
  Serial.println("Portal WiFi cerrado");
}

bool conectarWifi() {
  Serial.printf("Conectando a WiFi '%s'", wifiSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nNo se pudo conectar a la WiFi");
    return false;
  }
  Serial.printf("\nWiFi conectado. IP: %s, RSSI: %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
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
  if (WiFi.status() != WL_CONNECTED && !conectarWifi()) {
    return false;
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
  cargarCredenciales();
  portalConfiguracion();
  if (!conectarWifi()) {
    // Credenciales erróneas: reabre el portal para poder corregirlas
    portalConfiguracion();
    conectarWifi();
  }

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
