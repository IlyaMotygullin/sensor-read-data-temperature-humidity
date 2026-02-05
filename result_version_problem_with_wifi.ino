#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h> 
#include <ModbusMaster.h>
#include <ESP32Time.h>
#include "time.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "esp_sleep.h"

#define PIN_SENSOR 19 
#define FORMAT_LITTLEFS_IS_FAILED true
volatile long countTouchChoiceDisplay = 1; // перерисовка текущего дисплея

#define TX_PIN 17
#define RX_PIN 16
#define MY_PIN 4
ModbusMaster node;
volatile bool flag = false;
hw_timer_t* timer = NULL;
float humidity = 0;
float temperature = 0;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

AsyncWebServer server(80);
IPAddress ip(192, 168, 2, 1);
IPAddress getaway(192, 168, 2, 1);
IPAddress subnet(255, 255, 255, 0);

int day = 0;
int month = 0;
int year = 0;
int seconds = 0;
int minutes = 0;
int hours = 0;
ESP32Time rtc(3600);
String get_date = "";
String get_time = "";

const char* url = "https://script.google.com/macros/s/AKfycbxpmCPxy1VAstwCHBI0Db6d4K8zlBAGs-gDfIue1bx3XUp6hGaRM7ioj6aELWTxw9i-/exec";
const char* secretKey = "QWEr8793fdsa!!32LLqqp";
WiFiClient client;

String ssidUserWifi = "";
String passwordUserWifi = "";

#define PIN_ON_SENSOR 15 // on/off
volatile bool systemEnabled = true; // текущее состояние
bool lastSystemState = true; // предыдущее состояние

bool flagConnectWifi = false;
int countConnectWifi = 0;

const char index_html[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Дашборд датчика</title>
  <style>
    :root {
      --bg:#0b1220;
      --card:#0f1724;
      --glass:rgba(255,255,255,0.05);
      --accent-start:#22c1c3;
      --accent-end:#4ee0a8;
      --muted:#9aa6b2;
      --good:#10b981;
      --warn:#f59e0b;
      --danger:#ef4444;
      font-family: Inter, Roboto, "Segoe UI", system-ui, sans-serif;
    }
    body {
      margin:0;
      padding:0;
      background:linear-gradient(180deg,#061025 0%, #071226 60%);
      color:#e6eef3;
      display:flex;
      flex-direction:column;
      align-items:center;
      justify-content:flex-start;
      min-height:100vh;
    }
    header {
      width:100%;
      max-width:600px;
      padding:20px;
      text-align:center;
    }
    h1 { margin:0; font-size:20px; }
    p { color:var(--muted); margin:4px 0 0; font-size:14px; }
    .card {
      background:var(--card);
      border-radius:12px;
      box-shadow:0 4px 20px rgba(0,0,0,0.4);
      padding:20px;
      margin:20px;
      width:90%;
      max-width:400px;
      text-align:center;
    }
    .value {
      font-size:40px;
      font-weight:700;
      margin:10px 0;
      background:linear-gradient(90deg,var(--accent-start),var(--accent-end));
      -webkit-background-clip:text;
      -webkit-text-fill-color:transparent;
    }
    .label { color:var(--muted); font-size:13px; }
    footer { color:var(--muted); font-size:13px; margin-bottom:20px; }

    /* Кнопка скачать */
    .download-btn {
      margin-top: 20px;
      background: linear-gradient(90deg, var(--accent-start), var(--accent-end));
      border: none;
      border-radius: 8px;
      padding: 10px 18px;
      color: #fff;
      font-size: 14px;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.2s ease;
      width: 100%;
    }
    .download-btn:hover { opacity: 0.9; transform: translateY(-1px); }
    .download-btn:active { transform: translateY(1px); opacity: 0.8; }

    /* ===== Добавлено: кнопка открытия формы Wi-Fi ===== */
    .wifi-open-btn{
      margin-top: 10px;
      background: rgba(255,255,255,0.10);
      border: 1px solid rgba(255,255,255,0.18);
      border-radius: 8px;
      padding: 10px 18px;
      color: #fff;
      font-size: 14px;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.2s ease;
      width: 100%;
    }
    .wifi-open-btn:hover { opacity: 0.95; }
    .wifi-open-btn:active { opacity: 0.85; }

    /* ===== Важно: твой // в CSS невалиден, поэтому делаем нормальное скрытие ===== */
    .user-input { display:none; margin-top: 12px; text-align:left; padding-top: 12px; border-top: 1px solid rgba(255,255,255,0.08); }
    .user-input.open { display:block; }

    /* Немного стиля полей */
    .wifi-form input {
      width: 100%;
      box-sizing: border-box;
      padding: 10px 12px;
      border-radius: 8px;
      border: 1px solid rgba(255,255,255,0.15);
      background: rgba(255,255,255,0.05);
      color: #e6eef3;
      outline: none;
    }
    .wifi-form input::placeholder { color: rgba(230,238,243,0.55); }
    .wifi-form .user_field { margin: 10px 0; }

    .wifi-status{
      margin-top: 10px;
      font-size: 13px;
      min-height: 18px;
      color: var(--muted);
      text-align:left;
    }
    .wifi-status.ok{ color: var(--good); }
    .wifi-status.err{ color: var(--danger); }
  </style>
</head>
<body>
  <header>
    <h1>Дашборд датчика</h1>
    <p>Температура и влажность в реальном времени</p>
  </header>

  <main class="card">
    <div class="label">Температура</div>
    <div class="value" id="temperature">--°C</div>

    <div class="label">Влажность</div>
    <div class="value" id="humidity">--%</div>

    <p class="label" id="updateTime">Обновление: --:--:--</p>
    <p id="full-date"></p>

    <!-- Новая кнопка для скачивания файла -->
    <button id="download-btn" class="download-btn" type="button">📥 Скачать файл</button>

    <!-- ===== Добавлено: кнопка, которая открывает окно ввода (ниже фрейма датчика) ===== -->
    <button id="open-wifi" class="wifi-open-btn" type="button">📶 Подключить к Wi-Fi</button>

    <!-- Окно ввода Wi-Fi ниже фрейма датчика (как ты просил) -->
    <div class="user-input" id="wifi-box">
      <form class="wifi-form" id="wifi-form">
        <p class="user_field">
          <input id="ssid" name="ssid" type="text" placeholder="Введите название своей сети" required>
        </p>
        <p class="user_field">
          <input id="pass" name="pass" type="password" placeholder="Введите пароль своей сети" required>
        </p>
        <button class="download-btn" type="submit">Отправить данные</button>
        <div id="wifi-status" class="wifi-status"></div>
      </form>
    </div>
  </main>

  <footer>
    © Torex Monitoring
  </footer>

  <script>
    async function fetchData() {
      try {
        const [tResp, hResp] = await Promise.all([
          fetch('/temp').catch(() => null),
          fetch('/hum').catch(() => null)
        ]);
        if (!tResp || !hResp) throw new Error('нет связи');
        const t = parseFloat(await tResp.text());
        const h = parseFloat(await hResp.text());
        updateDashboard(t, h);
      } catch (e) {
        document.getElementById('temperature').textContent = '--°C';
        document.getElementById('humidity').textContent = '--%';
        document.getElementById('updateTime').textContent = 'Ошибка связи';
      }
    }

    function updateDashboard(temp, hum) {
      document.getElementById('temperature').textContent = temp.toFixed(1) + '°C';
      document.getElementById('humidity').textContent = hum.toFixed(1) + '%';
      document.getElementById('updateTime').textContent =
        'Обновление: ' + new Date().toLocaleTimeString('ru-RU');
    }

    async function requestServerDataOfDate() {
      let date = new Date();
      let dateDay = date.getDate();
      let dateMonth = date.getMonth() + 1;
      let dateYear = date.getFullYear();
      let sec = date.getSeconds();
      let min = date.getMinutes();
      let hour = date.getHours();

      try {
        await fetch(
          "/getDate?day=" + encodeURIComponent(dateDay) + "&" +
          "month=" + encodeURIComponent(dateMonth) + "&" +
          "year=" + encodeURIComponent(dateYear) + "&" +
          "sec=" + encodeURIComponent(sec) + "&" +
          "min=" + encodeURIComponent(min) + "&" +
          "hour=" + encodeURIComponent(hour)
        );
      } catch (err) {
        console.log(err);
      }
    }

    function getFile() {
      document.getElementById('download-btn').addEventListener('click', async () => {
        try {
          const response = await fetch("/getFile");

          if (!response.ok) {
            console.log("Не удалось получить данные");
            return;
          }

          const blob = await response.blob();
          const url = window.URL.createObjectURL(blob);

          const ref = document.createElement("a");
          ref.href = url;
          ref.download = "data_sensor.txt";

          document.body.appendChild(ref);
          ref.click();
          document.body.removeChild(ref);

          window.URL.revokeObjectURL(url);
        } catch (err) {
          console.log("Ошибка:", err);
        }
      });
    }

    function setupWifiToggle() {
      const openBtn = document.getElementById('open-wifi');
      const box = document.getElementById('wifi-box');
      const ssidInput = document.getElementById('ssid');

      openBtn.addEventListener('click', () => {
        const isOpen = box.classList.toggle('open');
        if (isOpen) ssidInput.focus();
      });
    }

    function setupWifiSubmit() {
      const form = document.getElementById('wifi-form');
      const statusEl = document.getElementById('wifi-status');

      function setStatus(text, kind) {
        statusEl.textContent = text || '';
        statusEl.classList.remove('ok', 'err');
        if (kind) statusEl.classList.add(kind);
      }

      form.addEventListener('submit', async (e) => {
        e.preventDefault();

        const ssid = document.getElementById('ssid').value.trim();
        const pass = document.getElementById('pass').value;

        if (!ssid || !pass) return;

        try {
          const url =
          "/setWiFi?ssid=" + encodeURIComponent(ssid) +
          "&pass=" + encodeURIComponent(pass);

          const resp = await fetch(url);
          const text = await resp.text();

          console.log(text); // "ok" или "error"
        } catch (err) {
          console.log(err);
        }
      });
    }

    getFile();
    setupWifiToggle();
    setupWifiSubmit();
    requestServerDataOfDate();
    fetchData();
    setInterval(fetchData, 2000);
  </script>
</body>
</html>
)rawliteral";

void IRAM_ATTR onWork() {
  systemEnabled = !systemEnabled;
}

void startWiFiAp(String ssid, String password) {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid, password);
  WiFi.softAPConfig(ip, getaway, subnet);
}

void writeFile(fs::FS &fs, const char* path, String data) {
  File file = fs.open(path, FILE_APPEND);
  if (!file) {
    return;
  }
  file.seek(file.size());
  file.print(data);
  file.close();
}

void startServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request -> send_P(200, "text/html", index_html);
  });

  server.on("/hum", HTTP_GET, [](AsyncWebServerRequest* request) {
    request -> send(200, "text/plain", String(humidity));
  });

  server.on("/temp", HTTP_GET, [](AsyncWebServerRequest* request) {
    request -> send(200, "text/plain", String(temperature));
  });

  server.on("/setWiFi", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request -> hasParam("ssid") && request -> hasParam("pass")) {
      ssidUserWifi = request -> getParam("ssid") -> value();
      passwordUserWifi = request -> getParam("pass") -> value();
      request -> send(200, "text/plain", "ok");
      return;
    }
    request -> send(400, "text/plain", "error");
  });

  server.on("/getDate", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request -> hasParam("day") &&
    request -> hasParam("month") &&
    request -> hasParam("year") &&
    request -> hasParam("sec") &&
    request -> hasParam("min") &&
    request -> hasParam("hour")) {
      day = request -> getParam("day") -> value().toInt();
      month = request -> getParam("month") -> value().toInt();
      year = request -> getParam("year") -> value().toInt();
      seconds = request -> getParam("sec") -> value().toInt();
      minutes = request -> getParam("min") -> value().toInt();
      hours = request -> getParam("hour") -> value().toInt();

      rtc.setTime(seconds, minutes, (hours-1), day, month, year);
      get_date = rtc.getDate() + "\n";
      get_time = rtc.getTime();
      writeFile(LittleFS, "/date_sensor.txt", get_date);

      request -> send(200, "text/plain", "ok");
      return;
    }
    request -> send(400, "text/plain", "error");
  });

  server.on("/getFile", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request -> beginResponse(LittleFS, "/data_sensor.txt", "text/plain");
    response -> addHeader("Content-Disposition", "attachment; filename=\"data_sensor.txt\"");
    request -> send(response);
  });
  server.begin();
}

void preTransmission() {
  digitalWrite(MY_PIN, HIGH);
}

void postTransmission() {
  digitalWrite(MY_PIN, LOW);
}

void IRAM_ATTR onTimer() {
  flag = true;
}

void IRAM_ATTR changeDisplay() {
  bool touch = digitalRead(PIN_SENSOR);
  if (touch) {
    countTouchChoiceDisplay++;
  }
}

void startModbus() {
  Serial1.begin(4800, SERIAL_8N1, RX_PIN, TX_PIN);
  node.begin(1, Serial1);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
}

void printDisplay(int sizeText, String text, int xPosition, int yPosition) {
  display.setTextColor(WHITE);
  display.setTextSize(sizeText);
  display.setCursor(xPosition, yPosition);
  display.print(text);
}

void timerStart() {
  timer = timerBegin(1000000);
  timerAttachInterrupt(timer, &onTimer);
  timerAlarm(timer, 2000000, true, 0);
  timerStart(timer);
}

// был тип bool
bool connectWiFi(String ssid, String password) {
  display.clearDisplay();
  printDisplay(1, "Connect to wifi", 0, 10);
  display.display();

  WiFi.begin(ssid, password);
  for (int i = 0; i < 30; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      return true;
    }
    delay(1000);
  }
  return false;
}

void getDataNtp(const char* url, long gmtOffset, long daylightOffset) {
  configTime(gmtOffset, daylightOffset, url);
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    return;
  }

  rtc.setTime(timeinfo.tm_sec,
  timeinfo.tm_min,
  (timeinfo.tm_hour - 1),
  timeinfo.tm_mday,
  (timeinfo.tm_mon + 1), 
  (timeinfo.tm_year + 1900));
}

void checkWifi() {
  bool connect = connectWiFi(ssidUserWifi, passwordUserWifi);
  if (connect) {
    Serial.println("Подключение есть!"); // -> нужно будет убрать!
    getDataNtp("pool.ntp.org", 18000, 0);
  } else {
    String ssidWiFi = "Torex";
    String passwordWiFi = "123Torex";
    startWiFiAp(ssidWiFi, passwordWiFi);

    display.clearDisplay();
    String infoDisplay = "You sure connected wifi: " + ssidWiFi + 
    " password: " + passwordWiFi + ". Localhost: 192.168.2.1";
    printDisplay(1, infoDisplay, 0, 10);
    display.display(); 
    // delay(30000);
  }
  startServer();
}

bool postToGoogle(float temp, float hum, const char* info = "") {
    // client.setInsecure();
    HTTPClient http;
    http.begin(url);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");

    DynamicJsonDocument doc(512);
    doc["key"] = secretKey;
    doc["temperature"] = temp;
    doc["humidity"] = hum;
    doc["info"] = info;

    String payload;
    serializeJson(doc, payload);

    int httpResponseCode = http.POST(payload);
    if (httpResponseCode > 0) {
      String resopnse = http.getString();
      http.end();
      return resopnse.indexOf("\"result\":\"ok\"") != -1;
    } else {
      http.end();
      return false;
    }
}

// выключение системы
void stopSystem() {
  if (timer != NULL) {
    timerStop(timer);
    timerDetachInterrupt(timer);
    timerEnd(timer);
    timer = nullptr;
  }
  Serial1.end();
  server.end();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  LittleFS.end();
  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
}

// перевод esp32 в сонный режим
void stopEspWork() {
  stopSystem();

  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_ON_SENSOR, 0);
  esp_deep_sleep_start();
}

// включение системы
void startSystem() {
  display.ssd1306_command(SSD1306_DISPLAYON);
  LittleFS.begin();
  checkWifi();
  startModbus();
  timerStart();
}

void applySystemState() {
  if (systemEnabled) {
    startSystem();
  } else {
    stopEspWork();
  }
}

void setup() {
  Serial.begin(9600);
  delay(1000);

  pinMode(PIN_ON_SENSOR, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ON_SENSOR), onWork, FALLING);

  pinMode(MY_PIN, OUTPUT);
  digitalWrite(MY_PIN, LOW);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for (; ;);
  }
  display.clearDisplay();

  // if (!LittleFS.begin(FORMAT_LITTLEFS_IS_FAILED)) {
  //   return;
  // }

  LittleFS.begin();

  checkWifi(); // -> инициализация один раз за проект
  startModbus();
  timerStart();

  attachInterrupt(digitalPinToInterrupt(PIN_SENSOR), changeDisplay, RISING);
  // startServer();
}

void printDataStreamer(float humidity, float temp) {
  Serial.print("Humidity: ");
  Serial.print(",");
  Serial.print(humidity);
  Serial.print(",");
  Serial.print("Temperature: ");
  Serial.print(",");
  Serial.println(temp);
}

void loop() {
  Serial.print("Сеть пользователя: ");
  Serial.println(ssidUserWifi);
  Serial.println();
  Serial.print("Пароль пользователя: ");
  Serial.println(passwordUserWifi);

  if (systemEnabled != lastSystemState) {
    lastSystemState = systemEnabled;
    applySystemState();
  }

  if (!systemEnabled) {
    delay(200);
    return;
  }

  if (flag) {
    flag = false;

    uint8_t result;
    uint16_t dataModbus[2];
    result = node.readInputRegisters(0x0000, 2);
    if (result == node.ku8MBSuccess) {
      dataModbus[0] = node.getResponseBuffer(0x00);
      dataModbus[1] = node.getResponseBuffer(0x01);

      humidity = dataModbus[0] / 10.0;
      temperature = dataModbus[1] / 10.0;

      String results = " Humidity: " + String(humidity) + "\t" + "Temperature: " + String(temperature) + "\n\n";
      String timer = rtc.getTime() + "\t";
      writeFile(LittleFS, "/data_sensor.txt", timer);
      writeFile(LittleFS, "/data_sensor.txt", results);
      printDataStreamer(humidity, temperature);

      postToGoogle(temperature, humidity, "sensor_get");
    }
  }

  if (countTouchChoiceDisplay % 2 != 0) {
    display.clearDisplay();
    printDisplay(1, "Temperature:", 0, 30);
    printDisplay(1, "Humidity:", 0, 50);
    printDisplay(1, String(temperature), 80, 30);
    printDisplay(1, String(humidity), 60, 50);
    display.display();
  } else {
    display.clearDisplay();
    printDisplay(1, rtc.getDate(), 0, 30);
    printDisplay(1, rtc.getTime(), 0, 50);
    display.display();
  }
}
