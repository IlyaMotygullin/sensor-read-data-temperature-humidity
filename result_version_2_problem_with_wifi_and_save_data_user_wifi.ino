#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <ModbusMaster.h>
#include <ESP32Time.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "time.h"
#include "bsp_button.h"
#include <Preferences.h>
#include <LittleFS.h>

TaskHandle_t task_esp;

volatile float temperature = 0;
volatile float humidity = 0;

SemaphoreHandle_t esp_work;
SemaphoreHandle_t time_mutex;

ESP32Time rtc(3600);

// -> disp
  #define EXAMPLE_PIN_NUM_LCD_SCLK 39
  #define EXAMPLE_PIN_NUM_LCD_MOSI 38
  #define EXAMPLE_PIN_NUM_LCD_MISO 40
  #define EXAMPLE_PIN_NUM_LCD_DC 42
  #define EXAMPLE_PIN_NUM_LCD_RST -1
  #define EXAMPLE_PIN_NUM_LCD_CS 45
  #define EXAMPLE_PIN_NUM_LCD_BL 1
  #define EXAMPLE_PIN_NUM_TP_SDA 48
  #define EXAMPLE_PIN_NUM_TP_SCL 47

  #define EXAMPLE_LCD_ROTATION 1
  #define EXAMPLE_LCD_H_RES 240
  #define EXAMPLE_LCD_V_RES 320

  #define LEDC_FREQ             5000
  #define LEDC_TIMER_10_BIT     10

  Arduino_DataBus* bus = new Arduino_ESP32SPI(
    EXAMPLE_PIN_NUM_LCD_DC,
    EXAMPLE_PIN_NUM_LCD_CS,
    EXAMPLE_PIN_NUM_LCD_SCLK,
    EXAMPLE_PIN_NUM_LCD_MOSI,
    EXAMPLE_PIN_NUM_LCD_MISO
  );

  Arduino_GFX *gfx = new Arduino_ST7789(
    bus,
    EXAMPLE_PIN_NUM_LCD_RST,
    EXAMPLE_LCD_ROTATION,
    false,
    EXAMPLE_LCD_H_RES,
    EXAMPLE_LCD_V_RES
  );

  lv_disp_draw_buf_t draw_buf;
  lv_color_t* disp_draw_buf;
  lv_disp_drv_t disp_drv;
  // lv_obj_t* scr = lv_scr_act();

  lv_obj_t* temperature_label_value;
  lv_obj_t* humidity_label_value;

  lv_obj_t* time_label_value;
  lv_obj_t* date_label_value;

  uint32_t screen_width;
  uint32_t screen_height;
  uint32_t buf_size;

  lv_indev_t* indev_keypad;
  volatile bool button_pressed_flag = false; // -> флаг для определения нажатия кнопки
  volatile bool button_is_pressed = false;
  bool current_screen_flag = true; // -> флаг для отрисовки определенного сигнала

  // lv_obj_t *key_obj;
  enum Screen_Mode { // -> два экрана 
    SENSOR_DATA_VALUE_SCREEN, // -> показания с датчика
    DATE_VALUE_SCREEN, // -> экран времени
    SETTINGS_SCREEN // -> экран настроек
  };
  Screen_Mode current_mode = SENSOR_DATA_VALUE_SCREEN; 

  // -> TODO: нужно сделать определение нажатия кнопки(длительное нажатие или короткое)
  /*
    при длительном нажатии нужно переходить на настройки экрана,
    иными словами, нужно сделать "два дисплея".

    настройки экрана:
      управление яркостью дисплея
      информация об адресе локального хоста
      информация об отключении самой платы

    нужно научиться определять тип нажатия:
      Простое нажатие - событие LV_EVENT_PRESSED
      Удержание кнопки - событие LV_EVENT_PRESSING
      Удержание кнопки по времени - событие LV_EVENT_LONG_PRESSED
  */

  static void keypad_read_cb(lv_indev_drv_t* indev_drv, lv_indev_data_t* data) { // -> привязка физической кнопки к lvgl
    static bool is_pressed = false;
    
    static uint32_t last_key = 0; // -> последний код клавиши для lvgl
    static uint32_t last_hw_key = 0; // -> последний код физической кнопки

    uint32_t act_key = bsp_button_read(); // -> получение текущего кода кнопки
    uint32_t lvgl_key = 0; // -> текущий код клавиши для lvgl

    if (act_key != 0 && last_hw_key == 0) { // -> обработка только коротких нажатий
      button_pressed_flag = true;
    }
    
    if (act_key != 0) {
      data -> state = LV_INDEV_STATE_PR;
      switch (act_key) {
        case 1:
          lvgl_key = LV_KEY_LEFT;
          break;
        case 2:
          lvgl_key = LV_KEY_RIGHT;
          break;
      }

      if (lvgl_key != 0) {
        last_key = lvgl_key;      
      }
    } else {
      data -> state = LV_INDEV_STATE_REL;
    }
    data -> key = last_key;
    last_hw_key = act_key;
  } 

  void my_disp_flush(lv_disp_drv_t* disp_drv, const lv_area_t* area, lv_color_t* color_p) {
    // lv_disp_flush_ready(disp_drv);
    uint32_t width = area -> x2 - area -> x1 + 1;
    uint32_t height = area -> y2 - area -> y1 + 1;
    gfx -> draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)color_p, width, height);
    lv_disp_flush_ready(disp_drv);
  }

  void create_label(lv_obj_t* scr, const char* text, int position_x, int position_y) {
    lv_obj_t* label = lv_label_create(scr);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, position_x, position_y);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
  }

  void create_panel(lv_obj_t* scr, const char* position, const char* color, int x_position, int y_position, int width, int height) {
    lv_obj_t* rect = lv_obj_create(scr); // -> объект - прямоугольник
    lv_obj_set_size(rect, width, height);
    if (position == "Left") {
      lv_obj_align(rect, LV_ALIGN_BOTTOM_LEFT, x_position, y_position);
      create_label(rect, "Humidity", 25, 10);
      humidity_label_value = lv_label_create(rect);
      lv_label_set_text(humidity_label_value, "--.-%");
      lv_obj_align(humidity_label_value, LV_ALIGN_TOP_LEFT, 50, 50);
      lv_obj_set_style_text_font(humidity_label_value, &lv_font_montserrat_16, 0);
    } else {
      lv_obj_align(rect, LV_ALIGN_BOTTOM_RIGHT, x_position, y_position);
      create_label(rect, "Temperature", 12, 10);
      temperature_label_value = lv_label_create(rect);
      lv_label_set_text(temperature_label_value, "--.-°C");
      lv_obj_align(temperature_label_value, LV_ALIGN_TOP_LEFT, 50, 50);
      lv_obj_set_style_text_font(temperature_label_value, &lv_font_montserrat_16, 0);
    }

    if (color == "Red") {
      lv_obj_set_style_bg_color(rect, lv_color_hex(0x3c78d8), LV_PART_MAIN); // -> код красного: 0x6d9eeb
    } else {
      lv_obj_set_style_bg_color(rect, lv_color_hex(0xe69138), LV_PART_MAIN);
    }
  }

  void update_sensor_display() { // -> обновляет данные с датчика и обновляет данные о времени
    float temp_copy_variable = 0;
    float hum_copy_variable = 0;

    char temp_str[10];
    char hum_str[10];

    if (xSemaphoreTake(esp_work, pdMS_TO_TICKS(100))) { // -> захват данных с датчика
      temp_copy_variable = temperature;
      hum_copy_variable = humidity;

      snprintf(hum_str, sizeof(hum_str), "%.1f%%", hum_copy_variable);//hum_copy_variable
      snprintf(temp_str, sizeof(temp_str), "%.1f°C", temp_copy_variable);// temp_copy_variable

      lv_label_set_text(temperature_label_value, temp_str);
      lv_label_set_text(humidity_label_value, hum_str);
      xSemaphoreGive(esp_work);
    }
  }

  void screen_sensor() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);
    create_label(scr, "Sensor humidity/temperature", 40, 10);
    // create_panel(lv_obj_t* scr, const char* position, int x_position, int y_position, int width, int height)
    create_panel(scr, "Left", "Blue", 2, -2, 159, 185); // -> синий
    create_panel(scr, "Right", "Red", -2, -2, 159, 185); // -> красный
  }

  void update_time_display() {
    // -> TODO: реализовать обновление времени
    char time_str[9];
    char date_str[11];

    if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(100))) {
      snprintf(
        time_str, sizeof(time_str), "%02d:%02d:%02d", 
        rtc.getHour(), rtc.getMinute(), rtc.getSecond()
      );

      snprintf(
        date_str, sizeof(date_str), "%02d.%02d.%02d", 
        rtc.getDay(), rtc.getMonth(), rtc.getYear()
      );
      lv_label_set_text(time_label_value, time_str);
      lv_label_set_text(date_label_value, date_str);

      xSemaphoreGive(time_mutex);
    }
  }
  /*
    lv_obj_t* time_label_value;
    lv_obj_t* date_label_value;
  */
  void screen_date() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);
    create_label(scr, "Time and Date Screen", 65, 10);
    
    create_label(scr, "Time:", 30, 60);
    time_label_value = lv_label_create(scr);
    lv_label_set_text(time_label_value, "--:--:--");
    lv_obj_align(time_label_value, LV_ALIGN_TOP_LEFT, 130, 100);
    lv_obj_set_style_text_font(time_label_value, &lv_font_montserrat_16, 0);

    create_label(scr, "Date:", 30, 140);
    date_label_value = lv_label_create(scr);
    lv_label_set_text(date_label_value, "--.--.--");
    lv_obj_align(date_label_value, LV_ALIGN_TOP_LEFT, 120, 180);
    lv_obj_set_style_text_font(date_label_value, &lv_font_montserrat_16, 0);
  }

  void screen_settings() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);
    create_label(scr, "Settings", 80, 10);
  }
// -> disp

// -> esp_work
  int day = 0;
  int month = 0;
  int year = 0;
  int hour = 0;
  int minutes = 0;
  int seconds = 0;

  // -> date and time
    const char* url_ntp = "pool.ntp.org";
    const long gmt_offset = 5 * 3600;
    const int day_light_offset = 0;

    void get_data_ntp(const char* url, long gmt_offset, int day_light_offset) { // -> для данного способа нужно подключение к интернету
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
  // -> date and time

  // -> server
    String ssid = "ESP32-S3";
    String password = "1234567890";

    AsyncWebServer server(80);
    IPAddress ip(192, 168, 2, 1);
    IPAddress geteway(192, 168, 2, 1);
    IPAddress subnet(255, 255, 255, 0);

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

    const char settings_html[] PROGMEM = R"rawliteral(
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
    )rawliteral";

    void start_server() {
      server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request -> send_P(200, "text/html", index_html);
      });

      server.on("/temp", HTTP_GET, [](AsyncWebServerRequest* request) {
        float copy_temp_variable = 0;
        if (xSemaphoreTake(esp_work, pdMS_TO_TICKS(100))) {
          copy_temp_variable = temperature;
          xSemaphoreGive(esp_work);
        }
        request -> send(200, "text/plain", String(copy_temp_variable));
      });

      server.on("/hum", HTTP_GET, [](AsyncWebServerRequest* request) {
        float copy_hum_variable = 0;
        if (xSemaphoreTake(esp_work, pdMS_TO_TICKS(100))) {
          copy_hum_variable = humidity;
          xSemaphoreGive(esp_work);
        }
        request -> send(200, "text/plain", String(copy_hum_variable));
      });

      server.on("/getDate", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (
          request -> hasParam("day") &&
          request -> hasParam("month") &&
          request -> hasParam("year") &&
          request -> hasParam("hour") &&
          request -> hasParam("min") &&
          request -> hasParam("sec")
        ) {
          day = request -> getParam("day") -> value().toInt();
          month = request -> getParam("month") -> value().toInt();
          year = request -> getParam("year") -> value().toInt();

          hour = request -> getParam("hour") -> value().toInt();
          minutes = request -> getParam("min") -> value().toInt();
          seconds = request -> getParam("sec") -> value().toInt();

          if (xSemaphoreTake(time_mutex, pdMS_TO_TICKS(100))) {
            rtc.setTime(seconds, minutes, (hour - 1), day, month, year);
            xSemaphoreGive(time_mutex);
          }
          request -> send(200, "text/html", "ok");
          return;
        }
        request -> send(400, "text/plain", "error");
      });

      server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* request) {
        request -> send_P(200, "text/html", settings_html);
      });

      server.on("/getFile", HTTP_GET, [](AsyncWebServerRequest* request) {
        String path_file = "/log.txt";
        String file_name = "log.txt";
        if (!LittleFS.exists(path_file)) {
          request -> send(400, "text/plain", "File not found");
          Serial.println("File not found");
          return;
        }
        AsyncWebServerResponse* response = request -> beginResponse(LittleFS, path_file, "text/plain");
        response -> addHeader("Content-Disposition", "attachment; filename=\"" + file_name + "\"");
        request -> send(response);
      });

      server.begin();
    }
  // -> server

  // -> fs
    void write_file(fs::FS &fs, String path_file, String data) {
      File file = fs.open(path_file, FILE_APPEND);
      if (!file) {
        return;
      }
      // file.seek(file.size());
      file.print(data);
      file.close();
    }
  // -> fs

  // -> google
    
  // -> google

  // -> modbus
    #define PIN_RX 16
    #define PIN_TX 17
    // #define TRANS_PIN 4 // -> maybe

    ModbusMaster node;
    hw_timer_t* timer_modbus = NULL;
    volatile bool flag_modbus = false;

    void IRAM_ATTR change_flag_modbus() {
      flag_modbus = true;
    }

    void start_timer_modbus() {
      timer_modbus = timerBegin(1000000);
      timerAttachInterrupt(timer_modbus, &change_flag_modbus);
      timerAlarm(timer_modbus, (2 * 1000000), true, 0);
      timerStart(timer_modbus);
    }

    void initial_modbus() {
      Serial1.begin(4800, SERIAL_8N1, PIN_RX, PIN_TX);
      node.begin(1, Serial1);
    }
  // -> modbus

  void esp_work_function(void* parameter) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ssid, password);
    WiFi.softAPConfig(ip, geteway, subnet);
    LittleFS.begin();
    start_server();
    
    Serial.println("LOGGER ESP_TASK: Task is start.");

    start_timer_modbus(); // -> запуск таймера
    initial_modbus();

    for (; ;) {
      if (flag_modbus) {
        flag_modbus = false;
        
        uint8_t result_operation;
        uint16_t data_modbus[2];

        result_operation = node.readInputRegisters(0x0000, 2);
        if (result_operation == node.ku8MBSuccess) {
          data_modbus[0] = node.getResponseBuffer(0x00);
          data_modbus[1] = node.getResponseBuffer(0x01);

          if (xSemaphoreTake(esp_work, pdMS_TO_TICKS(100))) {
            humidity = data_modbus[0] / 10.0;
            temperature = data_modbus[1] / 10.0;

            xSemaphoreGive(esp_work);
          }

          String sensor_data;
          String time_date_data;

          sensor_data = "Humidity: " + String(humidity) + " " + "Temperature: " + String(temperature) + "\n";
          time_date_data = "Date: " + String(rtc.getDate()) + " " + "Time: " + String(rtc.getTime()) + "\n\n";

          write_file(LittleFS, "/log.txt", sensor_data);
          write_file(LittleFS, "/log.txt", time_date_data);
        } else {
          Serial.println(result_operation);
        }
      }

      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
// -> esp_work

void setup() {
  Serial.begin(115200);
  delay(1000);

  esp_work = xSemaphoreCreateMutex();
  time_mutex = xSemaphoreCreateMutex();

  // -> disp_setup
    if (!gfx -> begin()) {
      return;
    }
    gfx -> fillScreen(BLACK);
    ledcAttach(EXAMPLE_PIN_NUM_LCD_BL , LEDC_FREQ, LEDC_TIMER_10_BIT);
    ledcWrite(EXAMPLE_PIN_NUM_LCD_BL , (1 << LEDC_TIMER_10_BIT) / 100 * 80);
    screen_width = gfx -> width();
    screen_height = gfx -> height();
    buf_size = screen_width * 40;

    lv_init();
    bsp_button_init();
    lv_disp_drv_init(&disp_drv);

    disp_draw_buf = (lv_color_t*) heap_caps_malloc(buf_size * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!disp_draw_buf) {
      disp_draw_buf = (lv_color_t*) heap_caps_malloc(buf_size * 2, MALLOC_CAP_8BIT);
    } else {
      lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, buf_size);
      disp_drv.hor_res = screen_width;
      disp_drv.ver_res = screen_height;
      disp_drv.flush_cb = my_disp_flush;
      disp_drv.draw_buf = &draw_buf;
      disp_drv.direct_mode = false;
      lv_disp_drv_register(&disp_drv);

      static lv_indev_drv_t indev_drv;
      lv_indev_drv_init(&indev_drv);
      indev_drv.type = LV_INDEV_TYPE_KEYPAD;
      indev_drv.read_cb = keypad_read_cb;
      indev_keypad = lv_indev_drv_register(&indev_drv);
    }
    screen_sensor();
  // -> disp_setup

  // -> task_esp
    Serial.println("Start task:");
    xTaskCreatePinnedToCore(
      esp_work_function,
      "Esp Task",
      10000,
      NULL,
      1,
      &task_esp,
      0
    );
  // -> task_esp
}

void loop() {
  lv_tick_inc(5);
  lv_timer_handler();

  if (button_pressed_flag) {
    button_pressed_flag = false;
    current_screen_flag = !current_screen_flag;
    if (current_screen_flag) {
      screen_sensor();
    } else {
      screen_date();
    }
  }

  if (current_screen_flag) { // -> состояние для одного экрана - экран показаний и времени
    update_sensor_display();
  } else {
    update_time_display();
  }
  delay(10);
}
