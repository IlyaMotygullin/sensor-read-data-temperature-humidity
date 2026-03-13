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
#include <Adafruit_XCA9554.h>

Adafruit_XCA9554 expander;
bool backlight_on = true;
bool expander_available = false;

// TODO: реализовать кнопку выключения + включения esp32s3
// TODO: на дисплее settings сделать кнопки + описание как зайти на сервер

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
    #define RX_PIN 16
    #define TX_PIN 17
    #define SENSOR_PIN 4

    ModbusMaster node;

    hw_timer_t* timer_modbus = NULL;
    volatile bool modbus_flag = false;

    void IRAM_ATTR change_flag_modbus() {
      modbus_flag = true;
    }

    // -> нужно доставать данные из Flash-памяти
    void timer_modbus_start() {
      timer_modbus = timerBegin(1000000);
      timerAttachInterrupt(timer_modbus, &change_flag_modbus);
      timerAlarm(timer_modbus, 2000000, true, 0);
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

    const char index_html[] PROGMEM = R"rawliteral()rawliteral";
    const char index_settings_html[] PROGMEM = R"rawliteral()rawliteral";

    void start_server() {
      server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request -> send_P(200, "text/html", index_html);
      });

      server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* request) {
        request -> send_P(200, "text/html", index_settings_html);
      });

      server.on("/temp", HTTP_GET, [](AsyncWebServerRequest* request) {
        request -> send(200, "text/plain", String(temperature));
      });

      server.on("/hum", HTTP_GET, [](AsyncWebServerRequest* request) {
        request -> send(200, "text/plain", String(humidity));
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

            rtc.setTime(sec, minutes, (hour - 1), day, month, year); // -> синхронизация времени с модулем rtc
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

      rtc.setTime(
        time_info.tm_sec,
        time_info.tm_min,
        (time_info.tm_hour - 1),
        time_info.tm_mday,
        (time_info.tm_mon + 1),
        (time_info.tm_year + 1900)
      );
    }
  // -> блок сервера

  // -> блок отправки в google-таблицы
    hw_timer_t* timer_google = NULL;
    volatile bool flag_write_to_google = false;

    void IRAM_ATTR change_google_flag() {
      flag_write_to_google = true;
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
      document["temperature"] = temperature;
      document["humidity"] = humidity;
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
    volatile bool flag_write_to_excel = false;

    void IRAM_ATTR change_excel_flag() {
      flag_write_to_excel = true;
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

    bool flag_write_to_google = preferences.getBool("flag_google", false);
    if (flag_write_to_google) {
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

    for (;;) {
      if (modbus_flag) {
        modbus_flag = false;

        uint8_t result;
        uint16_t data_modbus[2];

        result = node.readInputRegisters(0x0000, 2);
        if (result == node.ku8MBSuccess) {
          data_modbus[0] = node.getResponseBuffer(0x00);
          data_modbus[1] = node.getResponseBuffer(0x01);

          humidity = data_modbus[0] / 10.0;
          temperature = data_modbus[1] / 10.0;
        }
      }

      if (flag_write_to_google) {
        flag_write_to_google = false;
        post_to_google(temperature, humidity, "sensor_get");
      }

      if (flag_write_to_file_system) {
        flag_write_to_file_system = false;
        
        String path_file = preferences.getString("file_name", "log.txt");
        if (!path_file.startsWith("/")) {
          path_file = "/" + path_file;
        }
        String time_date = String(rtc.getTime()) + "\t";
        String sensor_data = "Humidity: " + String(humidity) + "\t" + "Temperature: " + String(temperature) + "\n";

        writeFile(LittleFS, path_file, time_date);
        writeFile(LittleFS, path_file, sensor_data);
      }

      if (flag_write_to_excel) {
        flag_write_to_excel = false;
        printDataStreamer(humidity, temperature);
      }

      vTaskDelay(pdMS_TO_TICKS(1000));
    }
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

  void toggle_back_light() {
    if (backlight_on) {
      for (int i = 255; i >= 0; i--) {
        gfx -> setBrightness(i);
        delay(3);
      }
    } else {
      for (int i = 0; i < 255; i++) {
        gfx -> setBrightness(i);
        delay(3);
      }
    }
    backlight_on = !backlight_on;
  }

  void Arduino_IIC_Touch_Interrupt(void) {
    FT3168 -> IIC_Interrupt_Flag = true;
  }

  void my_disp_flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t width = (area -> x2 - area -> x1 + 1);
    uint32_t height = (area -> y2 - area -> y1 + 1);

    gfx -> draw16bitBeRGBBitmap(area -> x1, area -> y1, (uint16_t*)color_p, width, height);
    lv_disp_flush_ready(disp);
  }

  void my_touch_read(lv_indev_drv_t* indev_driver, lv_indev_data_t* data) {
    uint32_t touch_X = FT3168 -> IIC_Read_Device_Value(FT3168 -> Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    uint32_t touch_Y = FT3168 -> IIC_Read_Device_Value(FT3168 -> Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);

    if (FT3168 -> IIC_Interrupt_Flag == true) {
      FT3168 -> IIC_Interrupt_Flag = false;
      data -> state = LV_INDEV_STATE_PR;

      data -> point.x = touch_X;
      data -> point.y = touch_Y;
    } else {
      data -> state = LV_INDEV_STATE_REL;
    }
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
    lv_obj_t* title_3 = lv_tileview_add_tile(tileview, 2, 0, LV_DIR_HOR);

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

    create_label(title_3, "Settings", 130, 50);
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
      while (1)
        ;
    }

    expander.pinMode(5, INPUT);
    expander.pinMode(4, INPUT);
    expander.pinMode(1, OUTPUT);
    expander.pinMode(2, OUTPUT);
    expander.digitalWrite(1, LOW);
    expander.digitalWrite(2, LOW);
    delay(20);
    expander.digitalWrite(1, HIGH);
    expander.digitalWrite(2, HIGH);

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
  update_display_time(); // -> ключевая ошибка

  int backlight_ctr = expander.digitalRead(4);
  if (backlight_ctr == HIGH) {
    while (expander.digitalRead(4) == HIGH) {
      delay(50);
    }
    toggle_back_light();
  }
  delay(5);
}
