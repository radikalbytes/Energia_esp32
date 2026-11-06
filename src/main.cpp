#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <DHT.h>
#include <Update.h>
#include <ArduinoOTA.h>

// ---------- WiFi / API ----------
// Las credenciales están en include/secrets.h (ignorado por git; ver secrets.example.h).
#include "secrets.h"
const char* WIFI_SSID = WIFI_SSID_VALUE;
const char* WIFI_PASSWORD = WIFI_PASSWORD_VALUE;
// Contraseña de las actualizaciones OTA (usuario "admin"). Define OTA_PASSWORD_VALUE en secrets.h.
#ifndef OTA_PASSWORD_VALUE
#define OTA_PASSWORD_VALUE "voltio"
#endif
const char* OTA_PASSWORD = OTA_PASSWORD_VALUE;

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

// El SCT-013-030 (versión 3.5mm) entrega 1 Vrms a 30 A (burden resistor integrado).
// El ADS1115 solo mide 0-Vref, por lo que la señal debe polarizarse a mitad de
// escala (~1.65V) con un divisor resistivo + condensador antes de entrar al ADC.
// Lectura DIFERENCIAL A0-A1 (igual que el sketch básico verificado): el offset de
// polarización se anula y la señal queda centrada en 0 V.
// Todos estos valores son ajustables desde la web (se guardan en flash).
struct Settings {
  float calibration = 30.0f;     // A por voltio (30 A / 1 V)
  float correction = 1.0f;       // factor de corrección de corriente (1.0 = sin corrección)
  float noiseFloor = 0.05f;      // A; por debajo se considera ruido (0 A)
  float mains = 230.0f;          // V; tensión de red asumida (no se mide)
  float powerFactor = 1.0f;      // factor de potencia para P = V * I * FP
  unsigned serialInterval = 30;  // s entre medidas mostradas por serie
} cfg;

const unsigned long MEASURE_WINDOW_MS = 1000; // 1 s = 50 ciclos exactos de 50 Hz

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

void cargarAjustes() {
  prefs.begin("cfg", false);
  cfg.calibration = prefs.getFloat("cal", cfg.calibration);
  cfg.correction = prefs.getFloat("corr", cfg.correction);
  cfg.noiseFloor = prefs.getFloat("noise", cfg.noiseFloor);
  cfg.mains = prefs.getFloat("mains", cfg.mains);
  cfg.powerFactor = prefs.getFloat("pf", cfg.powerFactor);
  cfg.serialInterval = prefs.getUInt("sint", cfg.serialInterval);
  prefs.end();
}

void guardarAjustes() {
  prefs.begin("cfg", false);
  prefs.putFloat("cal", cfg.calibration);
  prefs.putFloat("corr", cfg.correction);
  prefs.putFloat("noise", cfg.noiseFloor);
  prefs.putFloat("mains", cfg.mains);
  prefs.putFloat("pf", cfg.powerFactor);
  prefs.putUInt("sint", cfg.serialInterval);
  prefs.end();
}

// ---------- Redes WiFi guardadas ----------
const int MAX_REDES_GUARDADAS = 8;

int buscarRedGuardada(const String& ssid) {
  prefs.begin("nets", false);
  int n = prefs.getUChar("n", 0);
  int found = -1;
  for (int i = 0; i < n && found < 0; i++) {
    if (prefs.getString(("s" + String(i)).c_str(), "") == ssid) found = i;
  }
  prefs.end();
  return found;
}

String passRedGuardada(const String& ssid) {
  int i = buscarRedGuardada(ssid);
  if (i < 0) return "";
  prefs.begin("nets", false);
  String p = prefs.getString(("p" + String(i)).c_str(), "");
  prefs.end();
  return p;
}

void guardarRed(const String& ssid, const String& pass) {
  int idx = buscarRedGuardada(ssid);
  prefs.begin("nets", false);
  int n = prefs.getUChar("n", 0);
  if (idx < 0) {
    if (n >= MAX_REDES_GUARDADAS) { // llena: descarta la más antigua
      for (int i = 1; i < n; i++) {
        prefs.putString(("s" + String(i - 1)).c_str(), prefs.getString(("s" + String(i)).c_str(), ""));
        prefs.putString(("p" + String(i - 1)).c_str(), prefs.getString(("p" + String(i)).c_str(), ""));
      }
      n--;
    }
    idx = n++;
    prefs.putUChar("n", n);
  }
  prefs.putString(("s" + String(idx)).c_str(), ssid);
  prefs.putString(("p" + String(idx)).c_str(), pass);
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
             "button{background:#facc15;font-weight:700}a{color:#facc15}label{display:block;margin:6px 0}"
             "input[type=radio]{width:auto;margin-right:8px}</style></head><body>"
             "<h1>&#9889; Voltio monitor</h1><p>Elige tu red WiFi:</p>"
             "<form method='POST' action='/save'><select name='ssid' id='ssid'>";
  for (int i = 0; i < n; i++) {
    String name = WiFi.SSID(i);
    if (name.isEmpty()) continue;
    bool saved = buscarRedGuardada(name) >= 0;
    h += "<option data-saved='" + String(saved ? 1 : 0) + "' value=\"" + htmlEscape(name) + "\">" +
         (saved ? "&#9733; " : "") + htmlEscape(name) + " (" + String(WiFi.RSSI(i)) + " dBm" +
         (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "" : ", &#128274;") + ")</option>";
  }
  h += "</select><div id='sv' style='display:none'>"
       "<label><input type='radio' name='mode' value='saved' checked>Usar contrase&ntilde;a guardada</label>"
       "<label><input type='radio' name='mode' value='new'>Introducir una nueva</label></div>"
       "<input type='password' name='pass' id='pass' placeholder='Contrase&ntilde;a'>"
       "<button type='submit'>Guardar y conectar</button></form>"
       "<p><a href='/'>Buscar redes de nuevo</a></p>"
       "<script>var s=document.getElementById('ssid'),v=document.getElementById('sv'),p=document.getElementById('pass');"
       "function u(){var sv=s.selectedOptions.length&&s.selectedOptions[0].dataset.saved==='1';"
       "v.style.display=sv?'block':'none';"
       "var m=document.querySelector('input[name=mode]:checked').value;"
       "p.style.display=(sv&&m==='saved')?'none':'block';}"
       "s.onchange=u;document.querySelectorAll('input[name=mode]').forEach(function(r){r.onchange=u;});u();"
       "</script></body></html>";
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
    String pass = server.arg("pass");
    if (server.arg("mode") == "saved" && buscarRedGuardada(ssid) >= 0) pass = passRedGuardada(ssid);
    guardarCredenciales(ssid, pass);
    guardarRed(ssid, pass);
    wifiSsid = ssid;
    wifiPass = pass;
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
  if (buscarRedGuardada(wifiSsid) < 0) guardarRed(wifiSsid, wifiPass);
  return true;
}

float potenciaDe(float irms) { return irms * cfg.mains * cfg.powerFactor; }

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
    float v = ads.computeVolts(ads.readADC_Differential_0_1());
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
  r.irmsRaw = r.vrms * cfg.calibration;
  r.irms = r.irmsRaw < cfg.noiseFloor ? 0 : r.irmsRaw * cfg.correction;
  return r;
}

void informarCorriente(const CurrentStats& c) {
  Serial.printf("[I] Irms=%.3f A (bruta %.3f) | P=%.1f W | Vac=%.1f mV | offset=%.3f V | rango=%.3f..%.3f V | %u muestras (%.0f SPS)\n",
                c.irms, c.irmsRaw, potenciaDe(c.irms), c.vrms * 1000.0f, c.offset, c.vMin, c.vMax, c.samples, c.sps);
  if (c.samples == 0) {
    Serial.println("[!] Sin muestras del ADS1115, revisa el I2C");
  } else {
    if (fabsf(c.offset) > 0.3f) {
      Serial.println("[!] Offset diferencial alto: revisa la polarizacion A0/A1 del SCT-013");
    }
    if (c.vMin < -1.9f || c.vMax > 1.9f) {
      Serial.println("[!] Senal cerca de saturacion (+/-2.048 V): corriente excesiva o cableado incorrecto");
    }
    if (c.sps < 300) {
      Serial.println("[!] Frecuencia de muestreo baja, la medida puede ser imprecisa");
    }
  }
}

// ---------- Web de ajustes (siempre activa en la IP del dispositivo) ----------
WebServer webAjustes(80);
CurrentStats ultima = {};

String paginaAjustes(const String& aviso) {
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>Voltio - Ajustes</title><style>"
             "body{font-family:system-ui;background:#0f172a;color:#e2e8f0;max-width:460px;margin:0 auto;padding:20px}"
             "h1{color:#facc15}label{display:block;margin-top:10px;font-size:14px}"
             "input,button{width:100%;padding:10px;margin:4px 0;border-radius:10px;border:0;font-size:16px;box-sizing:border-box}"
             "button{background:#facc15;font-weight:700;margin-top:16px}.b{background:#1e293b;padding:12px;border-radius:10px}"
             "small{color:#94a3b8}</style></head><body><h1>&#9889; Voltio - Ajustes</h1>";
  if (aviso.length()) h += "<p class='b'>" + aviso + "</p>";
  h += "<div class='b'>IP del dispositivo: <b>" + WiFi.localIP().toString() + "</b><br>WiFi: " +
       htmlEscape(WiFi.SSID()) + " (" + String(WiFi.RSSI()) + " dBm)<br>MAC: " + WiFi.macAddress() +
       "<br>Firmware: " + String(__DATE__) + " " + String(__TIME__) + "</div>";
  h += "<div class='b'>&Uacute;ltima medida:<br>Irms = <b>" + String(ultima.irms, 3) + " A</b> (bruta " +
       String(ultima.irmsRaw, 3) + ")<br>Potencia = <b>" + String(potenciaDe(ultima.irms), 1) + " W</b><br>"
       "Vac = " + String(ultima.vrms * 1000.0f, 1) + " mV<br><small>Se actualiza al recargar la p&aacute;gina.</small></div>"
       "<form method='POST' action='/ajustes'>"
       "<label>Calibraci&oacute;n (A por voltio; SCT-013-030 = 30)</label>"
       "<input name='cal' type='number' step='any' min='0.1' max='1000' value='" + String(cfg.calibration, 3) + "'>"
       "<label>Factor de correcci&oacute;n de corriente (1.000 = sin correcci&oacute;n)</label>"
       "<input name='corr' type='number' step='any' min='0.1' max='10' value='" + String(cfg.correction, 4) + "'>"
       "<label>Ruido m&iacute;nimo (A; por debajo se muestra 0)</label>"
       "<input name='noise' type='number' step='any' min='0' max='5' value='" + String(cfg.noiseFloor, 3) + "'>"
       "<label>Tensi&oacute;n de red (V)</label>"
       "<input name='mains' type='number' step='any' min='50' max='500' value='" + String(cfg.mains, 1) + "'>"
       "<label>Factor de potencia (0.1 - 1.0)</label>"
       "<input name='pf' type='number' step='any' min='0.1' max='1' value='" + String(cfg.powerFactor, 3) + "'>"
       "<label>Intervalo de medida por serie (segundos, m&iacute;n. 1)</label>"
       "<input name='sint' type='number' step='1' min='1' max='3600' value='" + String(cfg.serialInterval) + "'>"
       "<button type='submit'>Guardar</button></form>"
       "<h2>Actualizar firmware (OTA)</h2><form method='POST' action='/update' enctype='multipart/form-data'>"
       "<input type='file' name='fw' accept='.bin' required>"
       "<button type='submit'>Subir firmware</button></form>"
       "<small>Usuario: admin. Archivo: .pio/build/&lt;entorno&gt;/firmware.bin</small></body></html>";
  return h;
}

void iniciarWebAjustes() {
  webAjustes.on("/", HTTP_GET, []() { webAjustes.send(200, "text/html", paginaAjustes("")); });
  webAjustes.on("/ajustes", HTTP_POST, []() {
    cfg.calibration = constrain(webAjustes.arg("cal").toFloat(), 0.1f, 1000.0f);
    cfg.correction = constrain(webAjustes.arg("corr").toFloat(), 0.1f, 10.0f);
    cfg.noiseFloor = constrain(webAjustes.arg("noise").toFloat(), 0.0f, 5.0f);
    cfg.mains = constrain(webAjustes.arg("mains").toFloat(), 50.0f, 500.0f);
    cfg.powerFactor = constrain(webAjustes.arg("pf").toFloat(), 0.1f, 1.0f);
    cfg.serialInterval = constrain(webAjustes.arg("sint").toInt(), 1, 3600);
    guardarAjustes();
    Serial.printf("[cfg] cal=%.3f corr=%.4f ruido=%.3f V=%.1f FP=%.3f intervalo=%us\n", cfg.calibration,
                  cfg.correction, cfg.noiseFloor, cfg.mains, cfg.powerFactor, cfg.serialInterval);
    webAjustes.send(200, "text/html", paginaAjustes("Ajustes guardados."));
  });
  webAjustes.on("/update", HTTP_POST, []() {
    if (!webAjustes.authenticate("admin", OTA_PASSWORD)) return webAjustes.requestAuthentication();
    bool ok = !Update.hasError();
    webAjustes.send(ok ? 200 : 500, "text/html",
                    ok ? "<meta charset='utf-8'><body style='font-family:system-ui;background:#0f172a;color:#e2e8f0;padding:20px'>"
                         "<h2>Firmware actualizado</h2><p>El dispositivo se reinicia...</p></body>"
                       : "<meta charset='utf-8'>Error al actualizar");
    if (ok) { delay(500); ESP.restart(); }
  }, []() {
    if (!webAjustes.authenticate("admin", OTA_PASSWORD)) return;
    HTTPUpload& up = webAjustes.upload();
    if (up.status == UPLOAD_FILE_START) {
      Serial.printf("[OTA] Recibiendo %s\n", up.filename.c_str());
      Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (up.status == UPLOAD_FILE_WRITE) {
      Update.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
      Serial.printf("[OTA] %s (%u bytes)\n", Update.end(true) ? "OK" : "FALLO", up.totalSize);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
      Update.abort();
    }
  });
  webAjustes.begin();
  ArduinoOTA.setHostname("voltio-monitor");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();
  Serial.printf("Web de ajustes en http://%s/\n", WiFi.localIP().toString().c_str());
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
  payload += "\"humidity\":" + String(humedad, 1) + ",";
  payload += "\"device_ip\":\"" + WiFi.localIP().toString() + "\"";
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
  cargarAjustes();
  cargarCredenciales();
  portalConfiguracion();
  if (!conectarWifi()) {
    // Credenciales erróneas: reabre el portal para poder corregirlas
    portalConfiguracion();
    conectarWifi();
  }

  iniciarWebAjustes();
  dht.begin();

  if (!ads.begin()) {
    Serial.println("No se detectó el ADS1115, revisa el cableado I2C");
    while (true) {
      delay(1000);
    }
  }
  ads.setGain(GAIN_TWO);              // +/-2.048V, 1 bit = 0.0625 mV (igual que el sketch básico)
  ads.setDataRate(RATE_ADS1115_860SPS);
}

void loop() {
  static unsigned long nextMeasure = 0;
  static unsigned long lastSend = 0;
  static double irmsSum = 0;
  static unsigned irmsCount = 0;

  webAjustes.handleClient();
  ArduinoOTA.handle();

  if ((long)(millis() - nextMeasure) >= 0) {
    nextMeasure = millis() + cfg.serialInterval * 1000UL;
    CurrentStats c = medirCorriente(MEASURE_WINDOW_MS);
    informarCorriente(c);
    ultima = c;
    irmsSum += c.irms;
    irmsCount++;
  }

  if (irmsCount > 0 && millis() - lastSend >= SEND_INTERVAL_MS) {
    lastSend = millis();
    float irms = irmsSum / irmsCount;
    float potencia = potenciaDe(irms);
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
