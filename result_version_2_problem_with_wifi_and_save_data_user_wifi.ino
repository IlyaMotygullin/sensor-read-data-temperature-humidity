#include <LittleFS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <Wire.h>
#include <ModbusMaster.h>
#include "time.h"
#include <ESP32Time.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "esp_sleep.h"

WiFiClient client;

Preferences preferences;

// TODO: -> реализация включения/выключения системы 

#define PIN_ON_OFF 15 // -> кнопка для включения/выключения системы
#define PIN_CHANGE_DISPLAY 19 // -> кнопка для смены дисплея
volatile long countTouchChoiceDisplay = 1; // счетчик для перерисовки текущего дисплея

void IRAM_ATTR changeDisplay() { // -> перерисовка текущего дисплея
  bool touch = digitalRead(PIN_CHANGE_DISPLAY);
  if (touch) {
    countTouchChoiceDisplay++;
  }
}

ESP32Time rtc(3600); // -> встроенный rtc-модуль esp32
int day = 0;
int month = 0;
int year = 0;
int hour = 0;
int minutes = 0;
int sec = 0;

AsyncWebServer server(80);
IPAddress ip(192, 168, 2, 1);
IPAddress geteway(192, 168, 2, 1);
IPAddress subnet(255, 255, 255, 0);

#define WIDTH_SCREEN 128
#define HEIGHT_SCREEN 64
#define OLED_RESET -1

Adafruit_SSD1306 display(WIDTH_SCREEN, HEIGHT_SCREEN, &Wire, OLED_RESET);

#define RX_PIN 16
#define TX_PIN 17
#define SENSOR_PIN 4

hw_timer_t* timer_modbus = NULL;
volatile bool flag_timer_modbus = false;
ModbusMaster node;

hw_timer_t* timer_google = NULL; // -> таймер для google-таблиц
volatile bool flag_write_to_google = false;

hw_timer_t* timer_file_system = NULL; // -> таймер для файловой системы
volatile bool flag_write_to_file_system = false;

hw_timer_t* timer_excel = NULL; // -> таймер для data streamer
volatile bool flag_write_to_excel = false;

const char* url_ntp = "pool.ntp.org";
const long gmt_offset = 5 * 3600;
const int day_light_offset = 0;

float humidity = 0;
float temperature = 0; 

// -> html-страница с показаниями
const char index_html[] PROGMEM = R"rawliteral(
  <!DOCTYPE html>
  <html lang="ru">
    <head>
      <meta charset="UTF-8" />
      <meta name="viewport" content="width=device-width, initial-scale=1.0" />
      <meta name="color-scheme" content="dark" />
      <title>Дашборд датчика</title>

      <style>
      :root{
        --bg0:#060f22;
        --bg1:#071a33;
        --glass:rgba(255,255,255,.06);
        --stroke:rgba(255,255,255,.10);
        --text:#e6eef3;
        --muted:#9aa6b2;
        --accent1:#22c1c3;
        --accent2:#4ee0a8;
        --shadow: 0 14px 40px rgba(0,0,0,.45);
        --r: 18px;
        --max: 720px;
      }

      *{ box-sizing:border-box; }
      html,body{ height:100%; }

      body{
        margin:0;
        color:var(--text);
        font-family: Inter, system-ui, -apple-system, "Segoe UI", Roboto, Arial, sans-serif;
        background:
        radial-gradient(1100px 700px at 15% 0%, rgba(34,193,195,.18), transparent 60%),
        radial-gradient(900px 600px at 95% 20%, rgba(78,224,168,.14), transparent 55%),
        linear-gradient(180deg, var(--bg0) 0%, var(--bg1) 65%, #060f22 100%);
        min-height:100vh;
        display:flex;
        align-items:center;
        justify-content:center;
        padding:16px;
        position:relative;
      }

      /* Кнопка настроек в угол */
      .settings-btn{
        position: fixed;
        top: 14px;
        right: 14px;
        z-index: 10;
        width: 44px;
        height: 44px;
        border-radius: 14px;
        border: 1px solid var(--stroke);
        background: rgba(15,23,36,.55);
        backdrop-filter: blur(10px);
        -webkit-backdrop-filter: blur(10px);
        color: var(--text);
        display:flex;
        align-items:center;
        justify-content:center;
        cursor:pointer;
        box-shadow: 0 10px 26px rgba(0,0,0,.35);
        transition: transform .15s ease, opacity .15s ease;
        -webkit-tap-highlight-color: transparent;
        user-select:none;
      }
      .settings-btn:hover{ opacity:.95; transform: translateY(-1px); }
      .settings-btn:active{ opacity:.9; transform: translateY(1px); }
      .settings-btn svg{ width:20px; height:20px; opacity:.95; }

      .wrap{
        width:100%;
        max-width:var(--max);
        display:flex;
        flex-direction:column;
        gap:14px;
      }

      header{
        padding:12px 8px 6px;
        text-align:center;
      }
      
      header h1{
        margin:0;
        font-size: clamp(18px, 3.8vw, 26px);
        letter-spacing:.2px;
      }
      header p{
        margin:6px 0 0;
        color:var(--muted);
        font-size: clamp(13px, 2.5vw, 15px);
        line-height:1.35;
      }

      .card{
        background: linear-gradient(180deg, rgba(255,255,255,.06), rgba(255,255,255,.03));
        border: 1px solid var(--stroke);
        backdrop-filter: blur(10px);
        -webkit-backdrop-filter: blur(10px);
        border-radius: var(--r);
        box-shadow: var(--shadow);
        overflow:hidden;
      }

      .card-inner{
        padding:16px;
        display:flex;
        flex-direction:column;
        gap:14px;
      }

      .grid{
        display:grid;
        grid-template-columns: 1fr;
        gap:12px;
      }

      @media (min-width: 520px){
        .grid{ grid-template-columns: 1fr 1fr; }
        .card-inner{ padding:18px; }
      }

      .metric{
        padding:14px 14px 12px;
        border-radius: 14px;
        background: rgba(15,23,36,.72);
        border: 1px solid rgba(255,255,255,.07);
        position:relative;
        overflow:hidden;
      }

      .metric::before{
        content:"";
        position:absolute;
        inset:-2px;
        background:
        radial-gradient(420px 120px at 30% 0%, rgba(34,193,195,.22), transparent 60%),
        radial-gradient(420px 120px at 80% 10%, rgba(78,224,168,.18), transparent 60%);
        pointer-events:none;
        opacity:.9;
      }

      .metric > *{ position:relative; z-index:1; }

      .label{
        color:var(--muted);
        font-size: 13px;
        display:flex;
        align-items:center;
        gap:8px;
        letter-spacing:.2px;
      }

      .value{
        margin-top:10px;
        font-weight:800;
        line-height:1;
        font-size: clamp(34px, 7vw, 48px);
        background: linear-gradient(90deg, var(--accent1), var(--accent2));
        -webkit-background-clip:text;
        background-clip:text;
        color:transparent;
        text-shadow: 0 0 24px rgba(78,224,168,.08);
      }

      .meta{
        display:flex;
        align-items:center;
        justify-content:space-between;
        gap:10px;
        padding:12px 14px;
        border-radius:14px;
        background: rgba(15,23,36,.55);
        border: 1px solid rgba(255,255,255,.07);
      }

      .meta .left{
      display:flex;
      flex-direction:column;
      gap:4px;
      min-width: 0;
      }

      .meta .left .small{
      color:var(--muted);
      font-size:12px;
      }

      .meta .left .time{
      font-size:14px;
      font-weight:600;
      white-space:nowrap;
      overflow:hidden;
      text-overflow:ellipsis;
      max-width: 100%;
      }

      .btn{
      width:100%;
      display:inline-flex;
      align-items:center;
      justify-content:center;
      gap:10px;
      padding:12px 14px;
      border:none;
      border-radius: 14px;
      font-size:14px;
      font-weight:700;
      color:#062017;
      cursor:pointer;
      background: linear-gradient(90deg, var(--accent1), var(--accent2));
      box-shadow: 0 14px 30px rgba(34,193,195,.18);
      transition: transform .15s ease, opacity .15s ease;
      user-select:none;
      -webkit-tap-highlight-color: transparent;
      }

      .btn:hover{ opacity:.95; transform: translateY(-1px); }
      .btn:active{ transform: translateY(1px); opacity:.9; }

      footer{
      text-align:center;
      color:var(--muted);
      font-size:12.5px;
        padding:6px 0 2px;
      }

      @media (prefers-reduced-motion: reduce){
        .btn, .settings-btn{ transition:none; }
      }

      .dot{
        width:10px; height:10px; border-radius:50%;
        background:linear-gradient(90deg,var(--accent1),var(--accent2));
        box-shadow: 0 0 18px rgba(78,224,168,.25);
        flex:0 0 10px;
      }
      </style>
  </head>

  <body>
    <a class="settings-btn" href="/settings" aria-label="Настройки" title="Настройки">
      <svg viewBox="0 0 24 24" fill="none" aria-hidden="true">
        <path d="M12 15.5a3.5 3.5 0 1 0 0-7 3.5 3.5 0 0 0 0 7Z" stroke="currentColor" stroke-width="2"/>
        <path d="M19.4 13a7.8 7.8 0 0 0 0-2l2-1.5-2-3.5-2.4 1a7.6 7.6 0 0 0-1.7-1L15 3h-6l-.3 2.5a7.6 7.6 0 0 0-1.7 1l-2.4-1-2 3.5 2 1.5a7.8 7.8 0 0 0 0 2l-2 1.5 2 3.5 2.4-1a7.6 7.6 0 0 0 1.7 1L9 21h6l.3-2.5a7.6 7.6 0 0 0 1.7-1l2.4 1 2-3.5-2-1.5Z"
            stroke="currentColor" stroke-width="2" stroke-linejoin="round"/>
      </svg>
    </a>

    <div class="wrap">
      <header>
        <h1>Показания датчика</h1>
        <p>Температура и влажность в реальном времени</p>
      </header>

      <section class="card" aria-label="Показания датчика">
        <div class="card-inner">
          <div class="grid">
            <div class="metric" aria-label="Температура">
              <div class="label"><span class="dot" aria-hidden="true"></span>Температура</div>
              <div class="value" id="temperature">--°C</div>
            </div>

            <div class="metric" aria-label="Влажность">
              <div class="label"><span class="dot" aria-hidden="true"></span>Влажность</div>
              <div class="value" id="humidity">--%</div>
            </div>
          </div>

          <div class="meta" aria-label="Информация об обновлении">
            <div class="left">
              <div class="small">Обновление</div>
              <div class="time" id="updateTime">--:--:--</div>
              <div class="small" id="full-date">—</div>
            </div>
          </div>
        </div>
      </section>

      <button id="download-btn" class="btn" type="button">📥 Скачать данные</button>

      <footer>© Torex Monitoring</footer>
    </div>

    <script> 

      function getDateHtml() { // -> получение даты и времени
        let date = new Date();

        let day = String(date.getDate()).padStart(2, "0");
        let month = String(date.getMonth() + 1).padStart(2, "0");
        let year = date.getFullYear();

        let fullYear = day + "." + month + "." + year;

        document
          .getElementById('full-date')
          .textContent = fullYear;

        let hours = date.getHours();
        let minutes = date.getMinutes();
        let seconds = date.getSeconds();
        let time = hours + ":" + minutes + ":" + seconds;

        document
          .getElementById('updateTime')
          .textContent = time;
      }

      async function getDataSensor() { // -> получение данных с сервера
        try {
          
          let responseTemp; // -> Ответ с url /temp
          let responseHum; // -> Ответ с url /hum

          responseTemp = await fetch('/temp');
          responseHum = await fetch('/hum');

          if (responseTemp.ok && responseHum.ok) {
            document
              .getElementById('temperature')
              .textContent = await responseTemp.json();

            document
              .getElementById('humidity')
              .textContent = await responseHum.json();
          } else {
            alert('Не удалось получить данные');
          }

        } catch (error) {
          console.error(error);
        }
      }

      setInterval(() => { // -> обновление экрана
        getDateHtml();
        getDataSensor();
      }, 1000);

      // -> нужен скрипт для получения времени с устройства, которое подключается к esp32
      async function sendDateOnServer() { // -> данная функция будет отправлять данные о времени на сервер
        // -> формат времени должен быть таким: 
        // dd-mm-YYYY -> дата
        // HH-mm-ss -> время

        let dateSend = new Date(); 
        
        let day = dateSend.getDate(); // -> возвращает день месяца
        let month = dateSend.getMonth() + 1;
        let year = dateSend.getFullYear();

        let hour = dateSend.getHours();
        let min = dateSend.getMinutes();
        let sec = dateSend.getSeconds();

        try {
          await fetch("/getDate?day=" + encodeURIComponent(String(day).padStart(2, "0")) + "&" +
            "month=" + encodeURIComponent(String(month).padStart(2, "0")) + "&" +
            "year=" + encodeURIComponent(year) + "&" +
            "hour=" + encodeURIComponent(hour) + "&" + 
            "min=" + encodeURIComponent(min) + "&" +
            "sec=" + encodeURIComponent(sec)
          );
        } catch(error) {
          console.log(error);
        }
      }
      sendDateOnServer();

      function getDataFile() { // -> функция для получения файла с сервера
        document
          .getElementById("download-btn")
          .addEventListener('click', async () => {
            try {
              const response = await fetch("/getFile");

              let blob = await response.blob();
              let url = window.URL.createObjectURL(blob);

              let ref = document.createElement("a");
              ref.href = url;
              ref.download = "data_sensor.txt";

              document.body.appendChild(ref);
              ref.click();
              document.body.removeChild(ref);

              window.URL.revokeObjectURL(url);
            } catch (error) {
              console.error(error);
            }
          });
          getDataFile();
      }

    </script>
  </body>
  </html>
)rawliteral";

// -> html-страница для настроек работы esp32
const char index_settings_html[] PROGMEM = R"rawliteral(
  <!DOCTYPE html>
  <html lang="ru">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <meta name="color-scheme" content="dark" />
    <title>Настройки ESP32</title>

    <style>
      :root{
      --bg0:#060f22; --bg1:#071a33; --stroke:rgba(255,255,255,.10);
      --text:#e6eef3; --muted:#9aa6b2; --accent1:#22c1c3; --accent2:#4ee0a8;
      --shadow: 0 14px 40px rgba(0,0,0,.45); --r: 18px; --max: 780px;
    }

      *{ box-sizing:border-box; margin:0; }

      body{
      color:var(--text);
      font-family: system-ui, -apple-system, sans-serif;
      background: radial-gradient(1100px 700px at 15% 0%, rgba(34,193,195,.18), transparent 60%),
                  radial-gradient(900px 600px at 95% 20%, rgba(78,224,168,.14), transparent 55%),
                  linear-gradient(180deg, var(--bg0) 0%, var(--bg1) 65%, #060f22 100%);
      min-height:100vh;
      padding:16px;
      }

      .wrap{
        max-width:var(--max); margin:0 auto;
      }

      header{
        text-align:center; padding:12px 8px 6px;
      }
      h1{ font-size:clamp(18px, 3.8vw, 26px); margin:0; }
      .subtitle{ color:var(--muted); font-size:clamp(13px, 2.5vw, 15px); margin-top:6px; }

      .card{
      background: linear-gradient(180deg, rgba(255,255,255,.06), rgba(255,255,255,.03));
      border:1px solid var(--stroke); border-radius:var(--r);
      backdrop-filter:blur(10px); box-shadow:var(--shadow); margin-top:14px;
      }
      .card-inner{ padding:16px; }
      @media (min-width:420px){ .card-inner{ padding:18px; } }

      .section{
      padding:12px 14px; border-radius:14px;
      background:rgba(15,23,36,.55); border:1px solid rgba(255,255,255,.07);
      margin-bottom:14px;
      }
      .section-title{ font-weight:800; font-size:14px; }
      .section-desc{ color:var(--muted); font-size:12px; margin-top:2px; }

      .field{ margin-bottom:12px; }
      label{ color:var(--muted); font-size:12px; display:block; margin-bottom:6px; }
    
      .input-group{ display:flex; gap:10px; }
      .input, select{
        width:100%; padding:12px; border-radius:12px; border:1px solid rgba(255,255,255,.10);
        background:rgba(6,15,34,.55); color:var(--text); font-size:14px;
      }
      .input:focus, select:focus{
      border-color:rgba(78,224,168,.35); box-shadow:0 0 0 3px rgba(78,224,168,.12);
      outline:none;
      }
    
      .unit{
      color:var(--muted); font-size:12px; white-space:nowrap;
      padding:10px; border-radius:12px; border:1px solid rgba(255,255,255,.10);
      background:rgba(6,15,34,.35);
      }

      .option{
      padding:14px; border-radius:14px; background:rgba(15,23,36,.72);
      border:1px solid rgba(255,255,255,.07); margin-bottom:12px;
      }
      .option-header{
      display:flex; align-items:flex-start; gap:10px; cursor:pointer;
      }
      .option-title{ font-weight:800; font-size:14px; }
      .option-desc{ color:var(--muted); font-size:12px; margin-top:4px; }
    
      .panel{
      margin-top:10px; padding:12px; border-radius:14px;
      background:rgba(255,255,255,.03); border:1px dashed rgba(255,255,255,.12);
      }
      .hidden{ display:none; }

      .grid{ display:grid; gap:10px; }
      @media (min-width:720px){ .grid{ grid-template-columns:1fr 1fr; } }

      .actions{
      display:flex; flex-direction:column; gap:10px; margin-top:20px;
      }
      @media (min-width:520px){ .actions{ flex-direction:row; } }
    
      .btn{
      padding:12px 14px; border-radius:14px; border:none; font-size:14px;
      font-weight:800; cursor:pointer; flex:1; text-align:center;
      text-decoration:none; display:inline-block;
      }
      .btn-primary{
      background:linear-gradient(90deg, var(--accent1), var(--accent2));
      color:#062017; box-shadow:0 14px 30px rgba(34,193,195,.18);
      }
      .btn-secondary{
      background:rgba(15,23,36,.55); color:var(--text);
      border:1px solid rgba(255,255,255,.12);
      }

      .hint{ color:rgba(154,166,178,.85); font-size:12px; margin-top:6px; }
    </style>
  </head>

  <body>
  <div class="wrap">
  <header>
    <h1>Настройки ESP32</h1>
    <p class="subtitle">Частота опроса и сохранение данных</p>
  </header>

  <section class="card">
    <form class="card-inner" action="/" method="GET">

      <div class="section">
        <div class="section-title">Частота получения данных</div>
        <div class="section-desc">Интервал чтения датчика (в секундах).</div>
      </div>

      <div class="field">
        <label for="poll_s">Интервал опроса</label>
        <div class="input-group">
          <select class="input" id="poll_s" name="poll_s">
            <option value="2" selected>2</option><option value="5">5</option>
            <option value="10">10</option><option value="15">15</option>
            <option value="30">30</option>
          </select>
          <span class="unit">сек</span>
        </div>
        <div class="hint">Минимальный интервал записи для всех способов сохранения.</div>
      </div>

      <div class="section">
        <div class="section-title">Сохранение данных</div>
        <div class="section-desc">Выбери один или несколько способов</div>
      </div>

      <!-- Google Sheets -->
      <div class="option">
        <div class="option-header" onclick="document.getElementById('use_gs').click()">
          <input type="checkbox" id="use_gs" name="use_gs" style="margin-top:2px;">
          <div>
            <div class="option-title">Google Таблицы</div>
            <div class="option-desc">Отправка данных в Google Sheets</div>
          </div>
        </div>
        
        <div id="panel_gs" class="panel hidden">
          <div class="grid">
            <div><label for="wifi_ssid">Wi-Fi SSID</label>
              <input class="input" id="wifi_ssid" name="wifi_ssid" placeholder="Имя сети"></div>
            <div><label for="wifi_pass">Wi-Fi пароль</label>
              <input class="input" id="wifi_pass" name="wifi_pass" placeholder="Пароль" type="password"></div>
          </div>
          
          <div class="grid">
            <div><label for="gs_url">URL Google Sheets</label>
              <input class="input" id="gs_url" name="gs_url" placeholder="https://script.google.com/..."></div>
            <div><label for="gs_key">Секретный ключ</label>
              <input class="input" id="gs_key" name="gs_key" placeholder="Ваш ключ"></div>
          </div>
          
          <div style="margin-top:10px;">
            <label for="gs_period_s">Скорость отправки</label>
            <div class="input-group">
              <select class="input save-period" id="gs_period_s" name="gs_period_s">
                <option value="2">2</option><option value="5" selected>5</option>
                <option value="10">10</option><option value="15">15</option>
                <option value="30">30</option>
              </select>
              <span class="unit">сек</span>
            </div>
          </div>
        </div>
      </div>

      <!-- Excel -->
      <div class="option">
        <div class="option-header" onclick="document.getElementById('use_excel').click()">
          <input type="checkbox" id="use_excel" name="use_excel" style="margin-top:2px;">
          <div>
            <div class="option-title">Excel (Data Streamer)</div>
            <div class="option-desc">Передача данных в Excel</div>
          </div>
        </div>
        
        <div id="panel_excel" class="panel hidden">
          <label for="excel_period_s">Скорость передачи</label>
          <div class="input-group">
            <select class="input save-period" id="excel_period_s" name="excel_period_s">
              <option value="2" selected>2</option><option value="5">5</option>
              <option value="10">10</option><option value="15">15</option>
              <option value="30">30</option>
            </select>
            <span class="unit">сек</span>
          </div>
        </div>
      </div>

      <!-- Файловая система -->
      <div class="option">
        <div class="option-header" onclick="document.getElementById('use_fs').click()">
          <input type="checkbox" id="use_fs" name="use_fs" style="margin-top:2px;">
          <div>
            <div class="option-title">Файловая система ESP32</div>
            <div class="option-desc">Локальное логирование</div>
          </div>
        </div>
        
        <div id="panel_fs" class="panel hidden">
          <div style="margin-bottom:10px;">
            <label for="fs_period_s">Скорость логирования</label>
            <div class="input-group">
              <select class="input save-period" id="fs_period_s" name="fs_period_s">
                <option value="2" selected>2</option><option value="5">5</option>
                <option value="10">10</option><option value="15">15</option>
                <option value="30">30</option>
              </select>
              <span class="unit">сек</span>
            </div>
          </div>
          
          <div class="grid">
            <div><label for="fs_filename">Имя файла</label>
              <input class="input" id="fs_filename" name="fs_filename" value="log.txt"></div>
            <div><label for="fs_format">Формат</label>
              <input class="input" id="fs_format" name="fs_format" value="txt"></div>
          </div>
        </div>
      </div>

      <div class="actions">
        <button class="btn btn-primary" type="button" id="send-server">✅ Применить</button>
        <a class="btn btn-secondary" href="/">← Назад к показаниям</a>
      </div>

      <div class="hint" style="margin-top:14px;">
        Сейчас форма отправляет параметры через URL (GET). Для пароля/ключа лучше сделать POST.
      </div>
    </form>
  </section>
  </div>

  <script>

    function sendSettingsOnServer() { // -> привязка к кнопке и отправка на сервер настроек
      document
        .getElementById("send-server")
        .addEventListener('click', async () => {
          try {
            let valueInterval = document.getElementById('poll_s').value; // -> первый input - интервал чтения modbus
            let valueCheckBoxGoogle = document.getElementById('use_gs').checked;
            let valueCheckBoxExcel = document.getElementById('use_excel').checked;
            let valueCheckBoxFileSystem = document.getElementById('use_fs').checked;

            let valueSsidWifi = document.getElementById('wifi_ssid').value;
            let valuePasswordWifi = document.getElementById('wifi_pass').value;
            let valueUrl = document.getElementById('gs_url').value;
            let valueSecretKey = document.getElementById('gs_key').value;
            let valueWriteInGoogle = document.getElementById('gs_period_s').value;

            let valueWriteInExcel = document.getElementById('excel_period_s').value;

            let valueWriteFs = document.getElementById('fs_period_s').value;
            let valueFileName = document.getElementById('fs_filename').value;

            await fetch('/get_settings', {
              method: 'POST',
              headers: {
                'Content-Type': 'application/json',
              },
              body: JSON.stringify({
                "interval": valueInterval,
                "google": {
                  "flag-google": valueCheckBoxGoogle,
                  "wifi-ssid": valueSsidWifi,
                  "wifi-pass": valuePasswordWifi,
                  "url-gs": valueUrl,
                  "secret-key": valueSecretKey,
                  "interval-write": valueWriteInGoogle
                },
                "excel": {
                  "flag-excel": valueCheckBoxExcel,
                  "interval-write": valueWriteInExcel
                },
                "file-system": {
                  "flag-fs": valueCheckBoxFileSystem,
                  "interval-write": valueWriteFs,
                  "file-name": valueFileName
                }
              })
            });
          } catch(error) {
            console.error(error);
          }
        });
    }

    sendSettingsOnServer();

    document.addEventListener('DOMContentLoaded', function() {
      // Показ/скрытие панелей настроек
      ['gs', 'excel', 'fs'].forEach(id => {
        const chk = document.getElementById('use_' + id);
        const panel = document.getElementById('panel_' + id);
        if (chk && panel) {
          chk.addEventListener('change', () => panel.classList.toggle('hidden', !chk.checked));
        }
      });

      // Валидация периодов сохранения
      const pollSelect = document.getElementById('poll_s');
      const saveSelects = document.querySelectorAll('.save-period');
    
      function updateSavePeriods() {
        const min = parseInt(pollSelect.value);
      
        saveSelects.forEach(select => {
          Array.from(select.options).forEach(option => {
            option.disabled = parseInt(option.value) < min;
          });
        
          // Автоматический выбор допустимого значения
          if (parseInt(select.value) < min) {
            const validOptions = Array.from(select.options).filter(opt => !opt.disabled);
            if (validOptions.length > 0) {
              select.value = validOptions[0].value;
            }
          }
        });
      }
    
      if (pollSelect) {
        pollSelect.addEventListener('change', updateSavePeriods);
        updateSavePeriods();
      }
    });
  </script>
  </body>
  </html>
)rawliteral";

void IRAM_ATTR changeFlagModbus() {
  flag_timer_modbus = true;
}
 
void timerModbusStart() {
  int dataInterval = preferences.getInt("interval_modbus", 2);
  timer_modbus = timerBegin(1000000);
  timerAttachInterrupt(timer_modbus, &changeFlagModbus);
  timerAlarm(timer_modbus, (dataInterval * 1000000), true, 0);
  timerStart(timer_modbus); 
}

void IRAM_ATTR changeGoogleFlag() {
  flag_write_to_google = !flag_write_to_google;
}

void timerWriteToGoogle() {
  int dataInterval = preferences.getInt("interval_google", 2);
  timer_google = timerBegin(1000000);
  timerAttachInterrupt(timer_google, &changeGoogleFlag);
  timerAlarm(timer_google, (dataInterval * 1000000), true, 0);
  timerStart(timer_google);
}

void IRAM_ATTR changeFileSystmFlag() {
  flag_write_to_file_system = !flag_write_to_file_system;
}

void timerWriteToFileSystem() {
  int dataInterval = preferences.getInt("interval_file_system", 2);
  timer_file_system = timerBegin(1000000);
  timerAttachInterrupt(timer_file_system, &changeFileSystmFlag);
  timerAlarm(timer_file_system, (dataInterval * 1000000), true, 0);
  timerStart(timer_file_system);
}

void IRAM_ATTR changeExcelFlag() {
  flag_write_to_excel = !flag_write_to_excel;
}

void timerWriteToExcel() {
  int dataInterval = preferences.getInt("interval_excel", 2);
  timer_excel = timerBegin(1000000);
  timerAttachInterrupt(timer_excel, &changeExcelFlag);
  timerAlarm(timer_excel, (dataInterval * 1000000), true, 0);
  timerStart(timer_excel);
}

void preTransmission() {
  digitalWrite(SENSOR_PIN, HIGH);
}

void postTransmission() {
  digitalWrite(SENSOR_PIN, LOW);
}

void startModbus() { // -> настройки modbus
  Serial1.begin(4800, SERIAL_8N1, RX_PIN, TX_PIN);
  node.begin(1, Serial1);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
}

void startServer() { // -> инициализация сервера + endpoints сервера 
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) { // -> страница с показаниями
    request -> send_P(200, "text/html", index_html);
  });

  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* request) { // -> страница настроек
    request -> send_P(200, "text/html", index_settings_html);
  });

  server.on("/temp", HTTP_GET, [](AsyncWebServerRequest* request) { // endpoint для отправки данных о температуре
    request -> send(200, "text/plain", String(temperature)); //temperature
  });

  server.on("/hum", HTTP_GET, [](AsyncWebServerRequest* request) { // endpoint для отправки данных о влажности
    request -> send(200, "text/plain", String(humidity));
  });

  // int day = 0;
  // int month = 0;
  // int year = 0;
  // int hour = 0;
  // int minutes = 0;
  // int sec = 0;

  server.on("/getDate", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request -> hasParam("day") &&
      request -> hasParam("month") &&
      request -> hasParam("year") &&
      request -> hasParam("hour") &&
      request -> hasParam("min") &&
      request -> hasParam("sec")) {
        day = request -> getParam("day") -> value().toInt();
        month = request -> getParam("month") -> value().toInt();
        year = request -> getParam("year") -> value().toInt();

        hour = request -> getParam("hour") -> value().toInt();
        minutes = request -> getParam("min") -> value().toInt();
        sec = request -> getParam("sec") -> value().toInt();

        rtc.setTime(sec, minutes, (hour - 1), day, month, year); // -> синхронизация времени с модулем rtc
        request -> send(200, "text/plain", "ok");
        return;
      } 
      request -> send(400, "text/plain", "error");
  });

  // server.on("/read_sensor", HTTP_GET, [](AsyncWebServerRequest* request) {
  //   if (request -> hasParam("read")) {
  //     int getDataIntervalRead = request -> getParam("read") -> value().toInt();
  //     preferences.putInt("interval", getDataIntervalRead);      
  //     request -> send(200, "text/plain", "ok");
  //     // ESP.restart(); // -> плохая идея
  //     return;
  //   }

  //   request -> send(400, "text/plain", "error");
  // });

  server.on("/get_settings", HTTP_POST, 
    [](AsyncWebServerRequest* request) {}, 
    NULL,
    [](AsyncWebServerRequest* request, uint8_t* data_part, size_t len_part, size_t index_start_part, size_t total_size) {
      
      String* json_object = new String();
      json_object -> reserve(total_size);

      for (size_t i = 0; i < len_part; i++) {
        json_object -> concat((char)data_part[i]);
      }

      DynamicJsonDocument document(2048);
      DeserializationError deserialize_document = deserializeJson(document, *json_object);
      
      if (deserialize_document) {
        Serial.println("JSON error");
        return;
      }

      JsonObject root = document.as<JsonObject>();
      int interval = atoi(root["interval"] | "2"); // -> получение интервала из JSON объекта
      preferences.putInt("interval_modbus", interval);

      // google
      JsonObject google = root["google"]; // -> получение JSON объекта google(все его данные)
      bool flag_google = google["flag-google"] | false;
      preferences.putBool("flag_google", flag_google);

      String ssid = google["wifi-ssid"] | "nothing";
      preferences.putString("ssid_wifi", ssid);

      String password = google["wifi-pass"] | "nothing";
      preferences.putString("password_wifi", password);

      String url_google_sheet = google["url-gs"] | "nothing";
      preferences.putString("url_google", url_google_sheet);

      String secret_key = google["secret-key"] | "nothing";
      preferences.putString("secret_key", secret_key);

      int interval_write_google = atoi(google["interval-write"] | "2");
      preferences.putInt("interval_google", interval_write_google);

      // excel
      JsonObject excel = root["excel"]; // -> получение JSON объекта excel
      bool excel_flag = excel["flag-excel"] | false;
      preferences.putBool("flag_excel", excel_flag);

      int interval_write_excel = atoi(excel["interval-write"] | "2");
      preferences.putInt("interval_excel", interval_write_excel);

      // file system
      JsonObject file_system = root["file-system"]; // -> получение JSON объекта file system
      bool flag_file_system = file_system["flag-fs"] | false;
      preferences.putBool("flag_file_system", flag_file_system);

      int interval_write_file_system = atoi(file_system["interval-write"] | "2");
      preferences.putInt("interval_file_system", interval_write_file_system);

      String file_name = file_system["file-name"] | "log.txt";
      preferences.putString("file_name", file_name);

      request -> send(200, "text/plain", "ok");
      ESP.restart();
  /*
      // if (index_start_part == 0) {
      //   request -> _tempObject = new String(); // -> обращение к полю _tempObject(void*) класса AsyncWebServerRequest
      //   ((String*) request -> _tempObject) -> reserve(total_size);

      //   String* body = (String*) request -> _tempObject;
      //   body -> concat((char*) data_part, len_part); // -> объединение всех частей JSON объекта из тела запроса

      //   if (index_start_part + len_part == total_size) {
      //     DynamicJsonDocument document(2048);
      //     DeserializationError deserialize_document = deserializeJson(document, *body);

      //     if (deserialize_document) {
      //       Serial.println("Invalid JSON object!");
      //       return;
      //     } 

      //     delete body;
      //     request -> _tempObject = nullptr;

      //     JsonObject root = document.as<JsonObject>();
      //     int interval = atoi(root["interval"] | "2"); // -> получение интервала из JSON объекта
          
      //     JsonObject google = root["google"]; // -> получение JSON объекта google(все его данные)
      //     bool flag_google = google["flag-google"] | false;
      //     const char* ssid = google["wifi-ssid"] | "nothing";
      //     const char* password = google["wifi-pass"] | "nothing";
      //     const char* url_google_sheet = google["url-gs"] | "nothing";
      //     const char* secretKey = google["secret-key"] | "nothing";
      //     int interval_write_google = atoi(google["interval-write"] | "2");

      //     JsonObject excel = root["excel"]; // -> получение JSON объекта excel
      //     bool excel_flag = excel["flag-excel"] | false;
      //     int interval_write_excel = atoi(excel["interval-write"] | "2");

      //     JsonObject file_system = root["file-system"]; // -> получение JSON объекта file system
      //     bool flag_file_system = file_system["flag-fs"] | false;
      //     int interval_write_file_system = atoi(file_system["interval-write"] | "2");
      //     const char* file_name = file_system["file-name"] | "log.txt";

      //     request -> send(200, "text/plain", "ok");
      //   }
      // }
    }
  */
  });

  // -> endpoint для скачивания файла
  server.on("/getFile", HTTP_GET, [](AsyncWebServerRequest* request) {
    String file_name = preferences.getString("file_name", "log.txt"); // -> файл приходит без /
    String path = file_name.startsWith("/") ? file_name : "/" + file_name; // -> добавление / для файловой системы LittleFS

    AsyncWebServerResponse* response = request -> beginResponse(LittleFS, path, "text/plain");
    String response_name_file = file_name;

    if (response_name_file.startsWith("/")) {
      response_name_file = response_name_file.substring(1); 
    }  
    
    response -> addHeader("Content-Disposition", "attachment; filename=\"" + response_name_file + "\"");
    request -> send(response);
  });

  server.begin();
}

void getConnection() { // -> функция для подключения к сети wifi
  // ssid_wifi
  // password_wifi
  
  String ssid_wifi = preferences.getString("ssid_wifi", "None");
  String password_wifi = preferences.getString("password_wifi", "None");

  WiFi.begin(ssid_wifi, password_wifi);
  // while (WiFi.status() != WL_CONNECTED) {
  //   delay(1000);
  // }

  for (int i = 0; i < 30; i++) {
    if (WL_CONNECTED == WiFi.status()) {
      return;
    }
    delay(1000);
  }
}

void getDataNtp(const char* url, long gmtOffset, int daylightOffset) { // -> получение данных с сервера ntp
  configTime(gmtOffset, daylightOffset, url);
  struct tm timeInfo;

  if (!getLocalTime(&timeInfo)) {
    return;
  }

  rtc.setTime(
    timeInfo.tm_sec,
    timeInfo.tm_min,
    (timeInfo.tm_hour - 1),
    timeInfo.tm_mday,
    (timeInfo.tm_mon + 1),
    (timeInfo.tm_year + 1900)
  );
}

bool postToGoogle(float temp, float hum, const char* info = "") { // -> отправка данных в google-таблицы
  HTTPClient http;
  String url = preferences.getString("url_google", "None"); // -> получение url
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");

  DynamicJsonDocument document(512);
  String secret_key = preferences.getString("secret_key", "None"); // -> получение секретного ключа
  
  document["key"] = secret_key;
  document["temperature"] = temperature;
  document["humidity"] = humidity;
  document["info"] = info;

  String payload;
  serializeJson(document, payload);

  int httpResponseCode = http.POST(payload);
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    http.end();
    return response.indexOf("\"result\":\"ok\"") != -1;
  } else {
    http.end();
    return false;
  }
}

void writeFile(fs::FS &fs, String path, String data) { // -> запись в файл
  File file = fs.open(path, FILE_APPEND);
  if (!file) {
    return;
  }
  file.seek(file.size());
  file.print(data);
  file.close();
}

void printDataStreamer(float humidity, float temperature) { // -> запись в excel
  Serial.print("Humidity: ");
  Serial.print(",");
  Serial.print(humidity);
  Serial.print(",");
  Serial.print("Temperature: ");
  Serial.print(",");
  Serial.println(temperature);
}
 

void setup() {
  Serial.begin(115200);

  preferences.begin("interval", false); // -> для записи и чтения

  // -> нужна инициализация файловой системы
  LittleFS.begin();
  
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(ip, geteway, subnet);
  WiFi.softAP("Torex", "Torex123");

  bool flag_write_to_google = preferences.getBool("flag_google", false);
  if (flag_write_to_google) {
    timerWriteToGoogle(); // -> запуск timer для записи в google-таблицы
    getConnection();

    // url_ntp
    // gmt_offset
    // day_light_offset
    getDataNtp(url_ntp, gmt_offset, day_light_offset);
  }

  bool flag_write_to_file_system = preferences.getBool("flag_file_system", false);
  if (flag_write_to_file_system) {
    timerWriteToFileSystem(); // -> запуск timer для записи в файловую систему
  }

  bool flag_writte_to_excel = preferences.getBool("flag_excel", false);
  if (flag_writte_to_excel) {
    timerWriteToExcel(); // -> запуск timer для записи в excel-таблицы 
  }

  pinMode(SENSOR_PIN, OUTPUT);
  digitalWrite(SENSOR_PIN, LOW);

  // -> инициализация дисплея:
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for(;;);
  }

  startServer();
  timerModbusStart(); // -> объявление в setup() одно значение до тех пор пока esp32 не перезагрузится
  startModbus();

  attachInterrupt(digitalPinToInterrupt(PIN_CHANGE_DISPLAY), changeDisplay, RISING);
}

void printDisplay(float humidity, float temperature) { // -> экран для отображения данных
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  
  display.setCursor(0, 0);
  display.println("Create by: Torex");

  display.setCursor(0, 20);
  display.println("Humidity:");

  display.setCursor(0, 40);
  display.println("Temperature:");

  display.setCursor(62, 20);
  display.println(String(humidity));

  display.setCursor(76, 40);
  display.println(String(temperature));

  display.display();
}

void printErrorDisplay(uint8_t status_error, String message) { // -> экран для отображения ошибок
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  display.setCursor(0, 20);
  display.println("Status:");

  display.setCursor(45, 20);
  display.println(String(status_error));

  display.setCursor(0, 40);
  display.println(message);

  display.display();
}

void printInfoDisplay() { // -> экран на котором будет расположена информация о том как зайти на сервер и как настроить esp32
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  display.setCursor(0, 10);
  display.println("Log in to the server at: 192.168.2.1.");
  display.println("Select the device's operating settings in the upper-right corner.");

  display.display();
}

void printTimeDisplay(String date, String time) { // -> экран для отображения времени и даты
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  display.setCursor(0, 10);
  display.println(date);

  display.setCursor(0, 30);
  display.println("Time:");

  display.setCursor(35, 30);
  display.println(time);

  display.display();
}

void loop() {

  if(flag_timer_modbus) {
    // Serial.println("tick");
    flag_timer_modbus = false;
    uint8_t result; // -> значение получения доступа к регистрам modbus
    uint16_t data_modbus[2]; // -> данные с регистров modbus

    result = node.readInputRegisters(0x0000, 2);
    if(result == node.ku8MBSuccess) {
      data_modbus[0] = node.getResponseBuffer(0x00);
      data_modbus[1] = node.getResponseBuffer(0x01);

      humidity = data_modbus[0] / 10.0;
      temperature = data_modbus[1] / 10.0;

      printDisplay(humidity, temperature); // -> вывод на экран

      String result_read = "Температура: " + String(temperature) + "\n" + "Влажность: " + String(humidity) + "\n";
      Serial.println(result_read); // -> запись в excel-таблицы
    } else {
      printErrorDisplay(result, "Error: couldn't get a response");
    }
  }
  
  if (flag_write_to_google) { // -> запись по прерыванию в google-Таблицы
    flag_write_to_google = !flag_write_to_google;
    postToGoogle(temperature, humidity, "sensor_get");
  }

  if (flag_write_to_file_system) { // -> запись по прерыванию в файловую систему
    // -> нужна функция для записи в файловую систему
    String path_file = preferences.getString("file_name", "log.txt");
    
    if (!path_file.startsWith("/")) {
      path_file = "/" + path_file;
    }

    String time_data = String(rtc.getTime()) + "\t";
    String sensor_data = "Humidity: " + String(humidity) + "\t" + "Temperature: " + String(temperature) + "\n";
    writeFile(LittleFS, path_file, time_data); // -> запись времени в файл
    writeFile(LittleFS, path_file, sensor_data); // -> запись данных с датчика в файл
  }

  if (flag_write_to_excel) { // -> запись по прерыванию в файл excel
    printDataStreamer(humidity, temperature);
  }

  if (countTouchChoiceDisplay % 2 == 0) {
    String date_info = String(rtc.getDate());
    String time_info = String(rtc.getTime());
    printTimeDisplay(date_info, time_info);
  } else if (countTouchChoiceDisplay % 3 == 0) {
    printInfoDisplay();
  }
}
