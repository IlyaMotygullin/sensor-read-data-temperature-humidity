#include <lvgl.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ModbusMaster.h>
#include <AsyncTCP.h>
#include <Arduino.h>
#include <ESP32Time.h>
#include "time.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include "pin_config.h"
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "XPowersLib.h"
#include <Adafruit_XCA9554.h>
#include "SD_MMC.h"
#include <FS.h>

XPowersPMU power;
Adafruit_XCA9554 expander;

bool pmu_flag = false;
bool backlight_on = true; // -> флаг который говорит что дисплей включен/выключен
bool touch_enabled = true; // -> флаг выключенного дисплея

SemaphoreHandle_t mutex_esp_work;
SemaphoreHandle_t time_mutex;
Preferences preferences;

// -> esp_work(блок, где данные читаются с датчиков, идет запись в google-таблицы и т.д)
  TaskHandle_t task_esp;

  volatile float humidity;
  volatile float temperature; 

  ESP32Time rtc(3600); // -> встроенный rtc-модуль esp32
  int day = 0;
  int month = 0;
  int year = 0;
  int hour = 0;
  int minutes = 0;
  int sec = 0;

  // -> блок modbus
    // TODO: поменять конфигурацию пинов
    #define RX_PIN 18
    #define TX_PIN 17
    #define SENSOR_PIN 41

    ModbusMaster node;

    hw_timer_t* timer_modbus = NULL;
    volatile bool modbus_flag = false;

    void IRAM_ATTR change_flag_modbus() {
      modbus_flag = true;
    }

    // -> нужно доставать данные из Flash-памяти
    void timer_modbus_start() {
      int data_interval = preferences.getInt("interval_modbus", 2);
      timer_modbus = timerBegin(1000000);
      timerAttachInterrupt(timer_modbus, &change_flag_modbus);
      timerAlarm(timer_modbus, (data_interval * 1000000), true, 0);
      timerStart(timer_modbus);
    }

    void preTransmission() {
      digitalWrite(SENSOR_PIN, HIGH);
    }

    void postTransmission() {
      digitalWrite(SENSOR_PIN, LOW);
    }
    
    void initial_modbus() {
      Serial1.begin(4800, SERIAL_8N1, RX_PIN, TX_PIN);
      node.begin(1, Serial1);
      node.preTransmission(preTransmission);
      node.postTransmission(postTransmission);
    }

  // -> блок modbus

  // -> блок сервера
    // TODO: добавить html-страницы + сделать запись данных на sd-карту(добавить кнопку записи на sd-карту)
    AsyncWebServer server(80);
    IPAddress ip(192, 168, 2, 1);
    IPAddress geteway(192, 168, 2, 1);
    IPAddress subnet(255, 255, 255, 0);

    WiFiClient client;

    String default_ssid_wifi = "Torex";
    String default_password_wifi = "Torex123";

    const char* url_ntp = "pool.ntp.org";
    const long gmt_offset = 5 * 3600;
    const int day_light_offset = 0;

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
          }
          getDataFile();

      </script>
      </body>
      </html>
    )rawliteral";
    
    const char index_settings_html[] PROGMEM = R"HTML_PAGE(
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

            <div style="margin-top:14px;">
              <button type="button" class="btn btn-secondary" id="gen-gas">
                ⚙️ Сгенерировать Google Script
              </button>
            </div>

            <div id="gas_container" class="panel hidden" style="margin-top:10px;">
              <label>Google Apps Script</label>
              <textarea id="gas_script" class="input" rows="10" readonly style="resize:vertical;"></textarea>
              
              <div class="actions" style="margin-top:10px;">
                <button type="button" class="btn btn-primary" id="copy-gas">
                  📋 Скопировать
                </button>
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

          <!-- SD-карта -->
          <div class="option">
            <div class="option-header" onclick="document.getElementById('use_sd').click()">
              <input type="checkbox" id="use_sd" name="use_sd" style="margin-top:2px;">
              <div>
                <div class="option-title">SD-карта</div>
                <div class="option-desc">Запись данных на SD-карту</div>
              </div>
            </div>
            
            <div id="panel_sd" class="panel hidden">
              <div style="margin-bottom:10px;">
                <label for="sd_period_s">Скорость записи</label>
                <div class="input-group">
                  <select class="input save-period" id="sd_period_s" name="sd_period_s">
                    <option value="2" selected>2</option>
                    <option value="5">5</option>
                    <option value="10">10</option>
                    <option value="15">15</option>
                    <option value="30">30</option>
                  </select>
                  <span class="unit">сек</span>
                </div>
              </div>
              
              <div class="grid">
                <div>
                  <label for="sd_filename">Имя файла</label>
                  <input class="input" id="sd_filename" name="sd_filename" value="sd_log.txt">
                </div>
                <div>
                  <label for="sd_format">Формат</label>
                  <input class="input" id="sd_format" name="sd_format" value="txt">
                </div>
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
                let valueCheckBoxSd = document.getElementById('use_sd').checked;

                let valueSsidWifi = document.getElementById('wifi_ssid').value;
                let valuePasswordWifi = document.getElementById('wifi_pass').value;
                let valueUrl = document.getElementById('gs_url').value;
                let valueSecretKey = document.getElementById('gs_key').value;
                let valueWriteInGoogle = document.getElementById('gs_period_s').value;

                let valueWriteInExcel = document.getElementById('excel_period_s').value;

                let valueWriteFs = document.getElementById('fs_period_s').value;
                let valueFileName = document.getElementById('fs_filename').value;

                let valueWriteSd = document.getElementById('sd_period_s').value;
                let valueSdFileName = document.getElementById('sd_filename').value;

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
                    },
                    "sd-card": {
                      "flag-sd": valueCheckBoxSd,
                      "interval-write": valueWriteSd,
                      "file-name": valueSdFileName
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
          ['gs', 'excel', 'fs', 'sd'].forEach(id => {
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

          document.getElementById("gen-gas").addEventListener("click", async () => {
            try {
              const key = document.getElementById("gs_key").value;

              if (!key) {
                alert("Введите секретный ключ!");
                return;
              }

              const response = await fetch('/get_gas_script?key=' + encodeURIComponent(key));;
              const text = await response.text();

              const container = document.getElementById("gas_container");
              const textarea = document.getElementById("gas_script");

              textarea.value = text;
              container.classList.remove("hidden");

            } catch (err) {
              console.error(err);
              alert("Ошибка получения скрипта");
            }
          });

          // Копирование
          document.getElementById("copy-gas").addEventListener("click", () => {
            const textarea = document.getElementById("gas_script");
            textarea.select();
            textarea.setSelectionRange(0, 99999);

            document.execCommand("copy");
            alert("Скрипт скопирован!");
          });

        // loadGasScript();
      </script>
      </body>
      </html>
    )HTML_PAGE";

    void start_server() {
      server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request -> send_P(200, "text/html", index_html);
      });

      server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* request) {
        request -> send_P(200, "text/html", index_settings_html);
      });

      server.on("/temp", HTTP_GET, [](AsyncWebServerRequest* request) {
        float copy_temp_variable = 0;
        if (xSemaphoreTake(mutex_esp_work, pdMS_TO_TICKS(100))) {
          copy_temp_variable = temperature;
          xSemaphoreGive(mutex_esp_work);
        }
        request -> send(200, "text/plain", String(copy_temp_variable));
      });

      server.on("/hum", HTTP_GET, [](AsyncWebServerRequest* request) {
        float copy_hum_variable = 0;
        if (xSemaphoreTake(mutex_esp_work, pdMS_TO_TICKS(100))) {
          copy_hum_variable = humidity;
          xSemaphoreGive(mutex_esp_work);
        }
        request -> send(200, "text/plain", String(copy_hum_variable));
      });

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

            if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(100))) {
              rtc.setTime(sec, minutes, (hour - 1), day, month, year); // -> синхронизация времени с модулем rtc
              xSemaphoreGive(time_mutex);
            }

            request -> send(200, "text/plain", "ok");
            return;
        } 
        request -> send(400, "text/plain", "error");
      });

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

          // sd card
          JsonObject sd_card = root["sd-card"];
          bool flag_sd_card = sd_card["flag-sd"] | false;
          preferences.putBool("flag_sd", flag_sd_card);

          int interval_write_sd = atoi(sd_card["interval-write"] | "2");
          preferences.putInt("interval_write_sd", interval_write_sd);

          String file_name_sd = sd_card["file-name"] | "log.txt";
          preferences.putString("file_name_sd", file_name_sd);

          request -> send(200, "text/plain", "ok");
          ESP.restart();
      });

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

      server.on("/get_gas_script", HTTP_GET, [](AsyncWebServerRequest* request) {
        String secret_key = "";

        if (request -> hasParam("key")) {
          secret_key = request->getParam("key")->value();
        }

        String gas_script = String(R"rawliteral(
        function doPost(e) {
          var SECRET = ")rawliteral") + secret_key + R"rawliteral("; // -> секретный ключ

          if (!e.postData || !e.postData.contents) {
            return badResponse("no_post_data");
          }

          var payload;
          try {
            payload = JSON.parse(e.postData.contents);
          } catch (err) {
            return badResponse("invalid_json");
          }

          if (!payload.key || payload.key !== SECRET) {
            return badResponse("invalid_key");
          }

          var today = getTodaySheetName();
          var ss = SpreadsheetApp.getActiveSpreadsheet();
          var sheet = ss.getSheetByName(today);

          if (!sheet) {
            sheet = ss.insertSheet(today);
            sheet.appendRow(["Timestamp", "Temperature", "Humidity", "Info"]);
          }

          var temp = payload.temperature == null ? "" : payload.temperature;
          var hum  = payload.humidity == null ? "" : payload.humidity;
          var info = payload.info || "";

          try {
            sheet.appendRow([ new Date(), temp, hum, info ]);
          } catch (err) {
            return badResponse("append_failed: " + err);
          }

          return ContentService
            .createTextOutput(JSON.stringify({ result: "ok" }))
            .setMimeType(ContentService.MimeType.JSON);
        }

        function getTodaySheetName() {
          var d = new Date();
          var year  = d.getFullYear();
          var month = ("0" + (d.getMonth() + 1)).slice(-2);
          var day   = ("0" + d.getDate()).slice(-2);
          return year + "-" + month + "-" + day;
        }

        function badResponse(code) {
          return ContentService
            .createTextOutput(JSON.stringify({ result: "error", code: code }))
            .setMimeType(ContentService.MimeType.JSON);
        }
        )rawliteral";

        request -> send(200, "text/plain", gas_script);
      });

      server.begin();
    }

    void get_connection_wifi() {
      String ssid_wifi = preferences.getString("ssid_wifi", "None");
      String password_wifi = preferences.getString("password_wifi", "None");
      
      WiFi.begin(ssid_wifi, password_wifi);
      for (int i = 0; i < 30; i++) {
        if (WL_CONNECTED == WiFi.status()) {
          return;
        }
        delay(1000);
      }
    }

    void get_data_ntp(const char* url, long gmt_offset, int day_light_offset) {
      configTime(gmt_offset, day_light_offset, url);
      struct tm time_info;

      if (!getLocalTime(&time_info)) {
        return;
      }

      if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(100))) {
          rtc.setTime(
          time_info.tm_sec,
          time_info.tm_min,
          (time_info.tm_hour - 1),
          time_info.tm_mday,
          (time_info.tm_mon + 1),
          (time_info.tm_year + 1900)
        );
        xSemaphoreGive(time_mutex);
      }
    }
  // -> блок сервера

  // -> блок записи на sd-карту
    bool sd_initial = false; // -> флаг инициализации sd-карты
    hw_timer_t* timer_sd = NULL;
    volatile bool flag_write_sd = false;

    void IRAM_ATTR change_sd_flag() {
      flag_write_sd = true;
    }

    void timer_sd_start() {
      int interval_write_to_sd = preferences.getInt("interval_write_sd", 2);
      timer_sd = timerBegin(1000000);
      timerAttachInterrupt(timer_sd, &change_sd_flag);
      timerAlarm(timer_sd, (interval_write_to_sd * 1000000), true, 0);
      timerStart(timer_sd);
    }

    void initial_sd() {
      if (!SD_MMC.begin()) {
        sd_initial = false;
      } else {
        sd_initial = true;
      }
    }

    void write_file_sd(String file_name, String data) { // -> функция для записи на sd-карту
      if (!sd_initial) {
        return;
      }
      
      File file = SD_MMC.open(file_name, FILE_APPEND);
      if (!file) {
        return;
      }

      file.println(data);
      file.close();
    }
  // -> блок запиаи на sd карту

  // -> блок отправки в google-таблицы
    hw_timer_t* timer_google = NULL;
    volatile bool flag_write_to_google_f = false;

    void IRAM_ATTR change_google_flag() {
      flag_write_to_google_f = true;
    }

    void timer_write_to_google() {
      int data_interval = preferences.getInt("interval_google", 2);
      timer_google = timerBegin(1000000);
      timerAttachInterrupt(timer_google, &change_google_flag);
      timerAlarm(timer_google, (data_interval * 1000000), true, 0);
      timerStart(timer_google);
    }

    bool post_to_google(float temp, float hum, const char* info = "") {
      HTTPClient http;
      String url = preferences.getString("url_google", "None");
      http.begin(url);
      http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      http.addHeader("Content-Type", "application/json");

      DynamicJsonDocument document(512);
      String secret_key = preferences.getString("secret_key", "None");

      document["key"] = secret_key;
      document["temperature"] = temp;
      document["humidity"] = hum;
      document["info"] = info;

      String payload;
      serializeJson(document, payload);

      int http_response_code = http.POST(payload);

      if (http_response_code > 0) {
        String response = http.getString();
        http.end();
        return response.indexOf("\"result\":\"ok\"") != -1;
      } else {
        http.end();
        return false;
      }
    }
  // -> блок отправки в google-таблицы

  // -> блок файловой системы
    hw_timer_t* timer_file_system = NULL;
    volatile bool flag_write_file_system = false;

    void IRAM_ATTR change_file_system_flag() {
      flag_write_file_system = true;
    }

    void timer_write_to_file_system() {
      int data_interval = preferences.getInt("interval_file_system", 2);
      timer_file_system = timerBegin(1000000);
      timerAttachInterrupt(timer_file_system, &change_file_system_flag);
      timerAlarm(timer_file_system, (data_interval * 1000000), true, 0);
      timerStart(timer_file_system);
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
  // -> блок файловой системы

  // -> блок data streamer
    hw_timer_t* timer_excel = NULL;
    volatile bool flag_write_to_excel_f = false;

    void IRAM_ATTR change_excel_flag() {
      flag_write_to_excel_f = true;
    }

    void timer_write_to_excel() {
      int data_interval = preferences.getInt("interval_excel", 2);
      timer_excel = timerBegin(1000000);
      timerAttachInterrupt(timer_excel, &change_excel_flag);
      timerAlarm(timer_excel, (data_interval * 1000000), true, 0);
      timerStart(timer_excel);
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
  // -> блок data streamer

  void esp_work_function(void* parameter) {
    initial_modbus();
    timer_modbus_start();
    start_server();

    bool flag_write_to_google_pref = preferences.getBool("flag_google", false);
    if (flag_write_to_google_pref) {
      timer_write_to_google();
      get_connection_wifi();
      get_data_ntp(url_ntp, gmt_offset, day_light_offset);
    }

    bool flag_write_to_file_system = preferences.getBool("flag_file_system", false);
    if (flag_write_to_file_system) {
      timer_write_to_file_system();
    }

    bool flag_write_to_excel = preferences.getBool("flag_excel", false);
    if (flag_write_to_excel) {
      timer_write_to_excel();
    }

    bool flag_write_to_sd = preferences.getBool("flag_sd", false);
    if (flag_write_to_sd) {
      initial_sd();
      timer_sd_start();
    }

    for (;;) {

      if (modbus_flag) {
        modbus_flag = false;

        uint8_t result;
        uint16_t data_modbus[2];

        result = node.readInputRegisters(0x0000, 2);
        if (result == node.ku8MBSuccess) {
          data_modbus[0] = node.getResponseBuffer(0x00);
          data_modbus[1] = node.getResponseBuffer(0x01);

          if (xSemaphoreTake(mutex_esp_work, pdMS_TO_TICKS(100))) {
            humidity = data_modbus[0] / 10.0;
            temperature = data_modbus[1] / 10.0;

            xSemaphoreGive(mutex_esp_work);
          } 
        }
      }

      if (flag_write_to_google_f) {
        flag_write_to_google_f = false;

        float copy_variable_temp = 0;
        float copy_variable_hum = 0;
        
        if (xSemaphoreTake(mutex_esp_work, pdMS_TO_TICKS(100))) {
          copy_variable_temp = temperature;
          copy_variable_hum = humidity;
          xSemaphoreGive(mutex_esp_work);
        }

        post_to_google(copy_variable_temp, copy_variable_hum, "sensor_get");
      }

      if (flag_write_file_system) {
        flag_write_file_system = false;
        
        String path_file = preferences.getString("file_name", "log.txt");
        if (!path_file.startsWith("/")) {
          path_file = "/" + path_file;
        }
        String time_date;
        String sensor_date;

        float copy_variable_tem = 0;
        float copy_variable_hum = 0;

        if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(100))) {
          time_date = String(rtc.getTime()) + "\n";
          xSemaphoreGive(time_mutex);
        }

        if (xSemaphoreTake(mutex_esp_work, pdMS_TO_TICKS(100))) {
          copy_variable_tem = temperature;
          copy_variable_hum = humidity;

          xSemaphoreGive(mutex_esp_work);
        }
        sensor_date = "Humidity: " + String(copy_variable_hum) + "\t" + "Temperature: " + String(copy_variable_tem) + "\n";

        writeFile(LittleFS, path_file, time_date);
        writeFile(LittleFS, path_file, sensor_date);
      }

      if (flag_write_to_excel_f) {
        flag_write_to_excel_f = false;

        float copy_variable_temp = 0;
        float copy_variable_hum = 0;
        if (xSemaphoreTake(mutex_esp_work, pdMS_TO_TICKS(100))) {
          copy_variable_temp = temperature;
          copy_variable_hum = humidity;
          xSemaphoreGive(mutex_esp_work);
        }
        printDataStreamer(copy_variable_hum, copy_variable_temp);
      }

      if (flag_write_sd) {
        flag_write_sd = false;

        String path_file = preferences.getString("file_name_sd", "log.txt");
        if (!path_file.startsWith("/")) {
          path_file = "/" + path_file;
        }

        float temp_copy_variable = 0;
        float hum_copy_variable = 0;
        String data_sensor;
        if (xSemaphoreTake(mutex_esp_work, pdMS_TO_TICKS(100))) {
          temp_copy_variable = temperature;
          hum_copy_variable = humidity;
          data_sensor = "Humidity: " + String(hum_copy_variable) + "\t" + "Temperature: " + String(temp_copy_variable) + "\n";
          xSemaphoreGive(mutex_esp_work);
        }

        String time_date;
        if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(100))) {
          time_date = String(rtc.getTime() + "\n");
          xSemaphoreGive(time_mutex);
        }
        write_file_sd(path_file, data_sensor);
        write_file_sd(path_file, time_date);
      }

      vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL); // -> выключение таймера задачи
  }
// -> esp_work

// -> display  
  // TODO: сделать график(температура и влажность относительно времени)
  HWCDC USBSerial;

  static lv_disp_draw_buf_t draw_buf;
  static lv_color_t buf[LCD_WIDTH * LCD_HEIGHT / 10];

  lv_obj_t* time_label_value;
  lv_obj_t* date_label_value;

  lv_obj_t* humidity_label_value;
  lv_obj_t* temperature_label_value;

  Arduino_DataBus* bus = new Arduino_ESP32QSPI(
    LCD_CS,
    LCD_SCLK,
    LCD_SDIO0,
    LCD_SDIO1,
    LCD_SDIO2,
    LCD_SDIO3
  );

  Arduino_SH8601* gfx = new Arduino_SH8601(
    bus, 
    GFX_NOT_DEFINED,
    0,
    LCD_WIDTH,
    LCD_HEIGHT
  );

  std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus = 
    std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
  
  void Arduino_IIC_Touch_Interrupt(void);

  std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(
    IIC_Bus,
    FT3168_DEVICE_ADDRESS,
    DRIVEBUS_DEFAULT_VALUE,
    TP_INT,
    Arduino_IIC_Touch_Interrupt
  ));

  void Arduino_IIC_Touch_Interrupt(void) {
    FT3168 -> IIC_Interrupt_Flag = true;
  }

  void my_disp_flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t width = (area -> x2 - area -> x1 + 1);
    uint32_t height = (area -> y2 - area -> y1 + 1);

    gfx -> draw16bitBeRGBBitmap(area -> x1, area -> y1, (uint16_t*)color_p, width, height);
    lv_disp_flush_ready(disp);
  }

  void toggle_backlight(); // -> прототип функции

  void my_touch_read(lv_indev_drv_t* indev_driver, lv_indev_data_t* data) {
    uint32_t touch_X = FT3168 -> IIC_Read_Device_Value(FT3168 -> Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    uint32_t touch_Y = FT3168 -> IIC_Read_Device_Value(FT3168 -> Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);

    if (FT3168 -> IIC_Interrupt_Flag == true) {
      FT3168 -> IIC_Interrupt_Flag = false;
      
      if (!touch_enabled) {
        toggle_backlight();
        data -> state = LV_INDEV_STATE_REL;
        return;
      }

      data -> state = LV_INDEV_STATE_PR;
      data -> point.x = touch_X;
      data -> point.y = touch_Y;
    } else {
      data -> state = LV_INDEV_STATE_REL;
    }
  }

  void setFlag(void) {
    pmu_flag = true;
  }

  void toggle_backlight() {
    if (backlight_on) {
      for (int i = 255; i >= 0; i--) {
        gfx -> setBrightness(i);
        delay(3);
      }

      touch_enabled = false;
      // vTaskSuspend(task_esp); // -> остановка задачи

      // -> остановка таймеров
      if (timer_modbus) {
        timerStop(timer_modbus);
      }
      if (timer_google) {
        timerStop(timer_google);
      }
      if (timer_file_system) {
        timerStop(timer_file_system);
      }
      if (timer_excel) {
        timerStop(timer_excel);
      }

      // server.end();
      WiFi.softAPdisconnect(true);
      WiFi.disconnect(true);
      // WiFi.mode(WIFI_OFF);
      WiFi.setSleep(true);
    } else {
      for (int i = 0; i <= 255; i++) {
        gfx -> setBrightness(i);
        delay(3);
      }

      touch_enabled = true;
      // vTaskResume(task_esp); // -> перезапуск задачи

      // start_server();
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAPConfig(ip, geteway, subnet);
      WiFi.softAP("Torex", "Torex123");
      delay(100);

      // -> запуск таймеров
      if(preferences.getBool("flag_google", false)) {
        timer_write_to_google();
      }
      if(preferences.getBool("flag_file_system", false)) {
        timer_write_to_file_system();
      }
      if(preferences.getBool("flag_excel", false)) {
        timer_write_to_excel();
      }
      timer_modbus_start();
    }
    backlight_on = !backlight_on;
  }

  void create_label(lv_obj_t* scr, const char* text, int position_X, int position_Y) {
    lv_obj_t* label = lv_label_create(scr);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, position_X, position_Y);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_22, 0);
  }

  void create_svipe(lv_obj_t* scr) {
    lv_obj_t* tileview = lv_tileview_create(scr);

    lv_obj_t* title_1 = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_HOR);
    lv_obj_t* title_2 = lv_tileview_add_tile(tileview, 1, 0, LV_DIR_HOR);
    // lv_obj_t* title_3 = lv_tileview_add_tile(tileview, 2, 0, LV_DIR_HOR);

    /*
      lv_obj_t* humidity_label_value;
      lv_obj_t* temperature_label_value;
    */

    create_label(title_1, "Sensor humidity-temperature", 22, 50);
    create_label(title_1, "indications", 120, 70);
    create_label(title_1, "Humidity:", 22, 150);
    humidity_label_value = lv_label_create(title_1);
    lv_label_set_text(humidity_label_value, "--.-°C");
    lv_obj_align(humidity_label_value, LV_ALIGN_TOP_LEFT, 150, 150);
    lv_obj_set_style_text_font(humidity_label_value, &lv_font_montserrat_22, 0);

    create_label(title_1, "Temperature:", 22, 250);
    temperature_label_value = lv_label_create(title_1);
    lv_label_set_text(temperature_label_value, "--.-%");
    lv_obj_align(temperature_label_value, LV_ALIGN_TOP_LEFT, 200, 250);
    lv_obj_set_style_text_font(temperature_label_value, &lv_font_montserrat_22, 0);
    
    create_label(title_2, "Time and Date", 105, 50);
    create_label(title_2, "Time:", 22, 150);
    time_label_value = lv_label_create(title_2);
    lv_label_set_text(time_label_value, "--:--:--");
    lv_obj_align(time_label_value, LV_ALIGN_TOP_LEFT, 100, 150);
    lv_obj_set_style_text_font(time_label_value, &lv_font_montserrat_22, 0);

    create_label(title_2, "Date:", 22, 250);
    date_label_value = lv_label_create(title_2);
    lv_label_set_text(date_label_value, "--:--:--");
    lv_obj_align(date_label_value, LV_ALIGN_TOP_LEFT, 100, 250);
    lv_obj_set_style_text_font(date_label_value, &lv_font_montserrat_22, 0);

    // create_label(title_3, "Settings", 130, 50);
    // create_btn(title_3, "OFF", 150, 50, 50, 50);
  }

  void update_display_time() {
    char time_str[9]; // -> 8 символов + конец строки "\0" => 9 символов на время
    char date_str[11]; // -> 10 символов + конец строки => 11 символов

    if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(100))) {
      snprintf(
        time_str, sizeof(time_str), "%02d:%02d:%02d", 
        rtc.getHour(), rtc.getMinute(), rtc.getSecond()
      );

      snprintf(
        date_str, sizeof(date_str), "%02d.%02d.%04d",
        rtc.getDay(), rtc.getMonth() + 1, rtc.getYear()
      );
      xSemaphoreGive(time_mutex);
    }

    float local_humidity_variable = 0;
    float local_temperature_variable = 0; 
    char hum_str[10];
    char temp_str[10];
    if (xSemaphoreTake(mutex_esp_work, pdMS_TO_TICKS(100))) {
      local_humidity_variable = humidity;
      local_temperature_variable = temperature;

      snprintf(hum_str, sizeof(hum_str), "%.1f%%", local_humidity_variable);
      snprintf(temp_str, sizeof(temp_str), "%.1f°C", local_temperature_variable);

      xSemaphoreGive(mutex_esp_work);
    }

    lv_label_set_text(time_label_value, time_str);
    lv_label_set_text(date_label_value, date_str);

    lv_label_set_text(humidity_label_value, hum_str);
    lv_label_set_text(temperature_label_value, temp_str);
  }
// -> display

void setup() {
  mutex_esp_work = xSemaphoreCreateMutex();
  time_mutex = xSemaphoreCreateMutex();
  // sleep_mutex = xSemaphoreCreateMutex();

  preferences.begin("interval", false);
  LittleFS.begin();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(ip, geteway, subnet);
  WiFi.softAP("Torex", "Torex123");

  xTaskCreatePinnedToCore(
    esp_work_function,
    "EspTask",
    10000,
    NULL,
    1,
    &task_esp,
    0
  );

  // -> display_setup
    USBSerial.begin(115200);
    Wire.begin(IIC_SDA, IIC_SCL);

    if (!expander.begin(0x20)) {
      while(1)
        ;
    }

    bool result = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
    if (result == false) {
      while(1) {
        delay(50);
      }
    }
    expander.pinMode(5, INPUT);

    int pmu_irq = expander.digitalRead(5);
    if (pmu_irq == 1) {
      setFlag();
    }

    power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    power.setChargeTargetVoltage(3);
    power.clearIrqStatus();
    power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);

    USBSerial.print("Ядро задействовано: ");
    USBSerial.println(xPortGetCoreID());

    gfx -> begin();
    gfx -> setBrightness(204);

    while (FT3168 -> begin() == false) {
      delay(2000);
    }

    FT3168 -> IIC_Write_Device_State(
      FT3168 -> Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
      FT3168 -> Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR
    );

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, LCD_WIDTH * LCD_HEIGHT / 10);

    static lv_disp_drv_t disp_drv;
      lv_disp_drv_init(&disp_drv);
      disp_drv.hor_res = LCD_WIDTH;
      disp_drv.ver_res = LCD_HEIGHT;
      disp_drv.flush_cb = my_disp_flush;
      disp_drv.draw_buf = &draw_buf;
      lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
      lv_indev_drv_init(&indev_drv);
      indev_drv.type = LV_INDEV_TYPE_POINTER;
      indev_drv.read_cb = my_touch_read;
      lv_indev_drv_register(&indev_drv);

    lv_obj_t* scr = lv_scr_act();
    create_svipe(scr);
  // -> display_setup
}

void loop() {
  lv_tick_inc(5);
  lv_timer_handler();

  if (expander.digitalRead(5) == LOW) {
    power.getIrqStatus();
    if (power.isPekeyShortPressIrq()) {
      toggle_backlight();
    }
    power.clearIrqStatus();
  }

  update_display_time(); // -> ключевая ошибка
  delay(5);
}
