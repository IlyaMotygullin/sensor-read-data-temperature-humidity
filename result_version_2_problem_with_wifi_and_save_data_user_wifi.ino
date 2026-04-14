#include <SPI.h>
#include <SD.h>
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
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Arduino.h>


TaskHandle_t task_esp;

volatile float temperature = 0;
volatile float humidity = 0;

SemaphoreHandle_t esp_work;
SemaphoreHandle_t time_mutex;

Preferences preferences;

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

  lv_obj_t* temperature_label_value;
  lv_obj_t* humidity_label_value;

  lv_obj_t* time_label_value;
  lv_obj_t* date_label_value;

  uint32_t screen_width;
  uint32_t screen_height;
  uint32_t buf_size;

  volatile bool button_pressed_flag = false; // -> флаг для определения нажатия кнопки
  volatile bool button_is_pressed = false;
  bool current_screen_flag = true; // -> флаг для отрисовки определенного сигнала

  // lv_obj_t *key_obj;
  enum Mode_ESP { // -> режимы работы esp32: настройки и показания
    MODE_VIEW_SENSOR_AND_TIME,
    MODE_VIEW_SETTINGS
  };
  Mode_ESP mode_esp = MODE_VIEW_SENSOR_AND_TIME;

  enum Screen_Sensor_Time_Date { // -> экраны показаний и времени для режима работы MODE_VIEW_SENSOR_AND_TIME
    SCREEN_SENSOR_VALUE,
    SCREEN_DATE_TIME_VALUE,
    SCREEN_CHART_SENSOR_VALUE,
    SCREEN_CHART_TEMP_SENSOR_VALUE,
    SCREEN_CHART_HUM_SENSOR_VALUE
  };
  Screen_Sensor_Time_Date screen_sensor_time_date = SCREEN_SENSOR_VALUE;

  enum Screen_Settings { // -> экраны настроек для режима работы MODE_VIEW_SETTINGS
    SCREEN_INFO,
    SCREEN_MODE_LOG_ESP32, // -> выбор хранилища данных
    SCREEN_BRIGHTNES
  };
  Screen_Settings current_screen_settings = SCREEN_INFO;

  // -> new block
    struct AppSettings {
      bool modbus_enabled;
      bool fs_enabled;
      bool excel_enabled;
      bool google_enabled;
      bool sd_enabled;
      int interval_modbus;
      int brightness;

      String fs_file_name;
      String fs_format;

      String sd_file_name;
      String sd_format;
    };

    AppSettings app_settings;
    SemaphoreHandle_t settings_mutex;

    volatile bool settings_ui_dirty = false;
    volatile bool modbus_time_need_restart = false;

    enum LogMenu {
      LOG_ITEM_MODBUS,
      LOG_ITEM_FS,
      LOG_ITEM_EXCEL,
      LOG_ITEM_GOOGLE,
      LOG_ITEM_SD,
      LOG_ITEM_BACK,
      LOG_ITEM_COUNT
    };
    LogMenu log_menu = LOG_ITEM_MODBUS;
    lv_obj_t* log_menu_labels[LOG_ITEM_COUNT] = {nullptr};

    void normalize_settings_locked() {
      if (!app_settings.modbus_enabled) {
        app_settings.fs_enabled = false;
        app_settings.excel_enabled = false;
        app_settings.google_enabled = false;
        app_settings.sd_enabled = false;
      }
    }

    void load_settings() {
      if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(100))) {
        app_settings.modbus_enabled = preferences.getBool("modbus_en", true);
        app_settings.fs_enabled = preferences.getBool("fs_en", false);
        app_settings.excel_enabled = preferences.getBool("excel_en", false);
        app_settings.google_enabled = preferences.getBool("google_en", false);
        app_settings.interval_modbus = preferences.getInt("interval_mb", 2);
        app_settings.brightness = preferences.getInt("brightness", 80);
        app_settings.fs_file_name = preferences.getString("fs_file_name", "log");
        app_settings.fs_format = preferences.getString("fs_format", "txt");
        app_settings.sd_file_name = preferences.getString("sd_file_name", "sd_log");
        app_settings.sd_format = preferences.getString("sd_format", "txt");

        normalize_settings_locked();
        xSemaphoreGive(settings_mutex);
      }
    }

    void save_settings() {
      if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(100))) {
        preferences.putBool("modbus_en", app_settings.modbus_enabled);
        preferences.putBool("fs_en", app_settings.fs_enabled);
        preferences.putBool("excel_en", app_settings.excel_enabled);
        preferences.putBool("google_en", app_settings.google_enabled);
        preferences.putInt("interval_mb", app_settings.interval_modbus);
        preferences.putInt("brightness", app_settings.brightness);
        preferences.putString("fs_file_name", app_settings.fs_file_name);
        preferences.putString("fs_format", app_settings.fs_format);
        preferences.putString("sd_file_name", app_settings.sd_file_name);
        preferences.putString("sd_format", app_settings.sd_format);
        xSemaphoreGive(settings_mutex);
      }
    }

    void mark_settings_changed(bool restart_modbus_timer = false) {
      save_settings();
      settings_ui_dirty = true;
      if (restart_modbus_timer) {
        modbus_time_need_restart = true;
      }
    }
  // -> new block

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
      lv_obj_set_style_bg_color(rect, lv_color_hex(0x3c78d8), LV_PART_MAIN); // -> код красного: 0x3c78d8
    } else {
      lv_obj_set_style_bg_color(rect, lv_color_hex(0xe69138), LV_PART_MAIN);
    }
  }

  void reset_screen_pointer() {
    temperature_label_value = nullptr;
    humidity_label_value = nullptr;
    time_label_value = nullptr;
    date_label_value = nullptr;
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
    reset_screen_pointer();
    create_label(scr, "Sensor humidity/temperature", 40, 10);
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

  void screen_date() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);
    reset_screen_pointer();
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
    reset_screen_pointer();
    create_label(scr, "INFORMATION SCREEN SETTINGS", 20, 10);
    create_label(scr, "To log in to the esp32-s3 server you", 10, 60);
    create_label(scr, " need to log in to the local host at:", 10, 90);
    create_label(scr, "192.168.2.1 in your browser, and also", 10, 120);
    create_label(scr, " connect to the ESP32-S3 wifi network.", 10, 150);
  }

  lv_obj_t* chart;
  lv_chart_series_t* temp_value_series; // -> серия температуры
  lv_chart_series_t* hum_value_series; // -> серия влажности

  volatile bool new_chart_data_ready = false; // -> флаг о поступлении новых данных
  volatile float last_temp_data = 0.0f;
  volatile float last_hum_data = 0.0f; 

  #define CHART_POINT_COUNT 20 // -> количесвто точек графика

  void create_chart(lv_obj_t* scr) {
    chart = lv_chart_create(scr);
    lv_obj_set_size(chart, 230, 140);
    // lv_obj_center(chart);
    lv_obj_align(chart, LV_ALIGN_CENTER, 0, 10);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);  
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_point_count(chart, CHART_POINT_COUNT); // -> количество точек 
    lv_chart_set_div_line_count(chart, 5, 7); // -> деление сетки

    lv_obj_set_style_radius(chart, 0, LV_PART_MAIN); // -> без скруглений
    lv_obj_set_style_border_width(chart, 1, LV_PART_MAIN);

    temp_value_series = lv_chart_add_series( // -> серия для температуры
      chart,
      lv_palette_main(LV_PALETTE_RED),
      LV_CHART_AXIS_PRIMARY_Y
    );
    hum_value_series = lv_chart_add_series(
      chart,
      lv_palette_main(LV_PALETTE_BLUE),
      LV_CHART_AXIS_PRIMARY_Y
    );

    for (int i = 0; i < CHART_POINT_COUNT; i++) {
      lv_chart_set_next_value(chart, temp_value_series, 0);
      lv_chart_set_next_value(chart, hum_value_series, 0);
    }
    lv_chart_refresh(chart);
  }

  void update_chart(float temp, float hum) {
    if (!chart || !temp_value_series || !hum_value_series) {
      return;
    }
    lv_chart_set_next_value(chart, temp_value_series, (lv_coord_t)temp);
    lv_chart_set_next_value(chart, hum_value_series, (lv_coord_t)hum);
    lv_chart_refresh(chart);
  }

  void screen_chart_all_value() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);
    reset_screen_pointer();
    create_label(scr, "SCREEN CHART TEMP/HUM VALUE", 15, 10);
    create_chart(scr);
  }

  void screen_chart_temp_value() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);
    reset_screen_pointer();
    create_label(scr, "SCREEN CHART TEMPERATURE", 30, 10);
    create_chart(scr);
  }

  void screen_chart_hum_value() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);
    reset_screen_pointer();
    create_label(scr, "SCREEN CHART HUMIDITY", 50, 10);
    create_chart(scr);
  }
 
  // -> new block
    lv_obj_t* slider_brightness = NULL; // -> объект slider lvgl
    lv_obj_t* label_brightness = NULL; // -> метка для отображения яркости
    int brightness_percent = 80; // -> значениею яркости

    // void apply_brightness(int percent) { // -> функция для изменения яркости
    //   if (percent < 1) { // -> яркость не может быть равной 0
    //     percent = 1;
    //   }
    //   if (percent > 100) { // -> яркость не может быть больше 100
    //     percent = 100;
    //   }
    //   brightness_percent = percent; // -> присваение текущей яркости новое значения
      
    //   uint32_t duty = ((1 << LEDC_TIMER_10_BIT) * brightness_percent) / 100; // -> перевод в проценты
    //   ledcWrite(EXAMPLE_PIN_NUM_LCD_BL, duty);

    //   if (slider_brightness && lv_slider_get_value(slider_brightness) != brightness_percent) {
    //     lv_slider_set_value(slider_brightness, brightness_percent, LV_ANIM_OFF);
    //   }
    //   if (label_brightness) {
    //     lv_label_set_text_fmt(label_brightness, "%d %%", brightness_percent);
    //   }
    // }

    // -> new block 2
      void apply_brightness(int percent) {
        if (percent < 1) percent = 1;
        if (percent > 100) percent = 100;

        brightness_percent = percent;

        uint32_t duty = ((1 << LEDC_TIMER_10_BIT) * brightness_percent) / 100;
        ledcWrite(EXAMPLE_PIN_NUM_LCD_BL, duty);

        if (slider_brightness && lv_slider_get_value(slider_brightness) != brightness_percent) {
          lv_slider_set_value(slider_brightness, brightness_percent, LV_ANIM_OFF);
        }
        if (label_brightness) {
          lv_label_set_text_fmt(label_brightness, "%d %%", brightness_percent);
        }
      }
    // -> new block 2
  // -> new block

  // void slider_event_cb(lv_event_t* event) { // -> по сути он данный callback не нужен, но в дальнейшем если будет touch то lvgl будет дергать данную функцию
  //   lv_event_code_t code = lv_event_get_code(event);
  //   if (code == LV_EVENT_VALUE_CHANGED) {
  //     lv_obj_t* slider = lv_event_get_target(event);
  //     int value = lv_slider_get_value(slider);
  //     apply_brightness(value);
  //   }
  // }

  void lvgl_brightness_ui_init(lv_obj_t* scr) { // -> создание фрейма со slider
    lv_obj_t* obj = lv_obj_create(scr);
    lv_obj_set_size(obj, lv_pct(90), lv_pct(50));
    lv_obj_align(obj, LV_ALIGN_CENTER, 0, 0);

    slider_brightness = lv_slider_create(obj);
    lv_slider_set_range(slider_brightness, 1, 100);
    lv_slider_set_value(slider_brightness, brightness_percent, LV_ANIM_OFF);
    lv_obj_set_size(slider_brightness, lv_pct(90), 20);
    lv_obj_align(slider_brightness, LV_ALIGN_CENTER, 0, 0);

    lv_obj_set_style_pad_top(obj, 20, 0);
    lv_obj_set_style_pad_bottom(obj, 20, 0);

    // lv_obj_add_event_cb(slider_brightness, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    label_brightness = lv_label_create(obj);
    lv_label_set_text_fmt(label_brightness, "%d %%", brightness_percent);
    lv_obj_align(label_brightness, LV_ALIGN_TOP_MID, 0, 0);
  }

  void screen_settings_brightness() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);
    create_label(scr, "BRIGHTNESS SCREEN SETTINGS", 25, 10);
    lvgl_brightness_ui_init(scr);
  }

  // void brightness_step_up() { // -> логика управления яркостью
  //   int next = brightness_percent + 10; // -> логика измения яркости по кругу
  //   if (next > 100) { // если текущее значение больше 100, то я обнуляю счетчик до 10 и снова увеличиваю до 100 
  //     next = 10;
  //   }
  //   apply_brightness(next);
  // }

  // -> new block 2
    void brightness_step_up() {
      int next = app_settings.brightness + 10;
      if (next > 100) {
        next = 10;
      }

      if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(100))) {
        app_settings.brightness = next;
        xSemaphoreGive(settings_mutex);
      }

      apply_brightness(next);
      mark_settings_changed(false);
    }
  // -> new block 2

  // -> new block 2
    // void update_log_menu_ui() {
    //   if (!log_menu_labels[0]) {
    //     return;
    //   }

    //   bool modbus_en = false;
    //   bool fs_en = false;
    //   bool excel_en = false;
    //   bool google_en = false;

    //   if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(100))) {
    //     modbus_en = app_settings.modbus_enabled;
    //     fs_en = app_settings.fs_enabled;
    //     excel_en = app_settings.excel_enabled;
    //     google_en = app_settings.google_enabled;
    //     xSemaphoreGive(settings_mutex);
    //   }

    //   char line0[64];
    //   char line1[64];
    //   char line2[64];
    //   char line3[64];
    //   char line4[32];

    //   snprintf(line0, sizeof(line0), "%s Sensor read   [%s]",
    //        log_menu == LOG_ITEM_MODBUS ? ">" : " ",
    //        modbus_en ? "ON" : "OFF");

    //   snprintf(line1, sizeof(line1), "%s LittleFS      [%s]",
    //        log_menu == LOG_ITEM_FS ? ">" : " ",
    //        modbus_en ? (fs_en ? "ON" : "OFF") : "LOCK");

    //   snprintf(line2, sizeof(line2), "%s Excel         [%s]",
    //        log_menu == LOG_ITEM_EXCEL ? ">" : " ",
    //        modbus_en ? (excel_en ? "ON" : "OFF") : "LOCK");

    //   snprintf(line3, sizeof(line3), "%s Google        [%s]",
    //        log_menu == LOG_ITEM_GOOGLE ? ">" : " ",
    //        modbus_en ? (google_en ? "ON" : "OFF") : "LOCK");

    //   snprintf(line4, sizeof(line4), "%s Back",
    //        log_menu == LOG_ITEM_BACK ? ">" : " ");

    //   lv_label_set_text(log_menu_labels[0], line0);
    //   lv_label_set_text(log_menu_labels[1], line1);
    //   lv_label_set_text(log_menu_labels[2], line2);
    //   lv_label_set_text(log_menu_labels[3], line3);
    //   lv_label_set_text(log_menu_labels[4], line4);
    // }

    // void update_log_menu_ui() {
    //   if (!log_menu_labels[0]) {
    //    return;
    //   }

    //   bool modbus_en = false;
    //   bool fs_en = false;
    //   bool excel_en = false;
    //   bool google_en = false;

    //   if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(100))) {
    //     modbus_en = app_settings.modbus_enabled;
    //     fs_en = app_settings.fs_enabled;
    //     excel_en = app_settings.excel_enabled;
    //     google_en = app_settings.google_enabled;
    //     xSemaphoreGive(settings_mutex);
    //   }

    //   char line0[64];
    //   char line1[64];
    //   char line2[64];
    //   char line3[64];
    //   char line4[64];

    //   snprintf(line0, sizeof(line0), "%c %-16s [%s]",
    //        log_menu == LOG_ITEM_MODBUS ? '>' : ' ',
    //        "Sensor read",
    //        modbus_en ? "ON" : "OFF");

    //   snprintf(line1, sizeof(line1), "%c %-16s [%s]",
    //        log_menu == LOG_ITEM_FS ? '>' : ' ',
    //        "LittleFS",
    //        modbus_en ? (fs_en ? "ON" : "OFF") : "LOCK");

    //   snprintf(line2, sizeof(line2), "%c %-16s [%s]",
    //        log_menu == LOG_ITEM_EXCEL ? '>' : ' ',
    //        "Excel",
    //        modbus_en ? (excel_en ? "ON" : "OFF") : "LOCK");

    //   snprintf(line3, sizeof(line3), "%c %-16s [%s]",
    //        log_menu == LOG_ITEM_GOOGLE ? '>' : ' ',
    //        "Google",
    //        modbus_en ? (google_en ? "ON" : "OFF") : "LOCK");

    //   snprintf(line4, sizeof(line4), "%c %s",
    //        log_menu == LOG_ITEM_BACK ? '>' : ' ',
    //        "Back");

    //   lv_label_set_text(log_menu_labels[0], line0);
    //   lv_label_set_text(log_menu_labels[1], line1);
    //   lv_label_set_text(log_menu_labels[2], line2);
    //   lv_label_set_text(log_menu_labels[3], line3);
    //   lv_label_set_text(log_menu_labels[4], line4);
    // }

    // -> new block 3
      void update_log_menu_ui() {
        if (!log_menu_labels[0]) {
          return;
        }

        bool modbus_en = false;
        bool fs_en = false;
        bool excel_en = false;
        bool google_en = false;
        bool sd_en = false;

        if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(100))) {
          modbus_en = app_settings.modbus_enabled;
          fs_en = app_settings.fs_enabled;
          excel_en = app_settings.excel_enabled;
          google_en = app_settings.google_enabled;
          sd_en = app_settings.sd_enabled;
          xSemaphoreGive(settings_mutex);
        }

        char line0[64];
        char line1[64];
        char line2[64];
        char line3[64];
        char line4[64];
        char line5[64];

        snprintf(line0, sizeof(line0), "%c %-16s [%s]",
          log_menu == LOG_ITEM_MODBUS ? '>' : ' ',
          "Sensor read",
          modbus_en ? "ON" : "OFF");

        snprintf(line1, sizeof(line1), "%c %-16s [%s]",
          log_menu == LOG_ITEM_FS ? '>' : ' ',
          "LittleFS",
          modbus_en ? (fs_en ? "ON" : "OFF") : "LOCK");

        snprintf(line2, sizeof(line2), "%c %-16s [%s]",
          log_menu == LOG_ITEM_EXCEL ? '>' : ' ',
          "Excel",
          modbus_en ? (excel_en ? "ON" : "OFF") : "LOCK");

        snprintf(line3, sizeof(line3), "%c %-16s [%s]",
          log_menu == LOG_ITEM_GOOGLE ? '>' : ' ',
          "Google",
          modbus_en ? (google_en ? "ON" : "OFF") : "LOCK");

        snprintf(line4, sizeof(line4), "%c %-16s [%s]",
          log_menu == LOG_ITEM_SD ? '>' : ' ',
          "SD card",
          modbus_en ? (sd_en ? "ON" : "OFF") : "LOCK");

        snprintf(line5, sizeof(line5), "%c %s",
          log_menu == LOG_ITEM_BACK ? '>' : ' ',
          "Back");

        lv_label_set_text(log_menu_labels[0], line0);
        lv_label_set_text(log_menu_labels[1], line1);
        lv_label_set_text(log_menu_labels[2], line2);
        lv_label_set_text(log_menu_labels[3], line3);
        lv_label_set_text(log_menu_labels[4], line4);
        lv_label_set_text(log_menu_labels[5], line5);
      }
    // -> new block 3
  // -> new block 2

  // -> TODO: сделать экран в режиме настройки, где у меня есть выбор: sensor -> off/on, FS -> off/on, Excel -> off/on
  /*
    Мне нужно синхронизировать показания сервера с показаниями экрана и занести эти измения в Flash memory - Preferences
    Сам сервер должен уметь запрашивать данные из Flasg memory, для того чтобы сервер мог граммотно отобразить данные, которые пользователь может изменить на экране настроек
    Можно создать единую структуру настроек, где у меня будут константы отвечающие за работу modbus, FS, Excel, Data Streamer
    Так как у меня две задачи крутяться в разныых потоках, то нужно работать с симафорами и памятью для того чтобы избежать race condition
  */

  // void screen_setting_log_mode() {
  //   lv_obj_t* scr = lv_scr_act();
  //   lv_obj_clean(scr);
  //   create_label(scr, "MODE ESP32-S3-LOGGER", 55, 10);
  //   create_screen_log_table(scr, 320, 50, 0, 50);
  //   create_screen_log_table(scr, 320, 50, 0, 120);
  //   create_screen_log_table(scr, 320, 50, 0, 190);
  // }

  // -> new block 2
    void screen_setting_log_mode() {
      lv_obj_t* scr = lv_scr_act();
      lv_obj_clean(scr);
      reset_screen_pointer();

      create_label(scr, "MODE ESP32-S3 LOGGER", 60, 10);

      for (int i = 0; i < LOG_ITEM_COUNT; i++) {
        log_menu_labels[i] = lv_label_create(scr);
        lv_obj_set_style_text_font(log_menu_labels[i], &lv_font_montserrat_16, 0);
      }

      lv_obj_align(log_menu_labels[0], LV_ALIGN_TOP_LEFT, 10, 50);
      lv_obj_align(log_menu_labels[1], LV_ALIGN_TOP_LEFT, 10, 80);
      lv_obj_align(log_menu_labels[2], LV_ALIGN_TOP_LEFT, 10, 110);
      lv_obj_align(log_menu_labels[3], LV_ALIGN_TOP_LEFT, 10, 140);
      lv_obj_align(log_menu_labels[4], LV_ALIGN_TOP_LEFT, 10, 170); // flag
      lv_obj_align(log_menu_labels[5], LV_ALIGN_TOP_LEFT, 10, 210);

      update_log_menu_ui();
    }

    void handle_log_menu_next() {
      log_menu = (LogMenu)((log_menu + 1) % LOG_ITEM_COUNT);
      update_log_menu_ui();
    }

    void handle_log_menu_select() {
      bool changed = false;

      if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(100))) {
        switch (log_menu) {
          case LOG_ITEM_MODBUS:
            app_settings.modbus_enabled = !app_settings.modbus_enabled;
            normalize_settings_locked();
            changed = true;
            break;

          case LOG_ITEM_FS:
            if (app_settings.modbus_enabled) {
              app_settings.fs_enabled = !app_settings.fs_enabled;
              changed = true;
            }
            break;

          case LOG_ITEM_EXCEL:
            if (app_settings.modbus_enabled) {
              app_settings.excel_enabled = !app_settings.excel_enabled;
              changed = true;
            }
            break;

          case LOG_ITEM_GOOGLE:
            if (app_settings.modbus_enabled) {
              app_settings.google_enabled = !app_settings.google_enabled;
              changed = true;
            }
            break;
          
          case LOG_ITEM_SD:
            if (app_settings.modbus_enabled) {
              app_settings.sd_enabled = !app_settings.sd_enabled;
              changed = true;
            }
            break;

          case LOG_ITEM_BACK:
            xSemaphoreGive(settings_mutex);
            current_screen_settings = SCREEN_BRIGHTNES;
            screen_settings_brightness();
            return;

          default:
            break;
        }
        xSemaphoreGive(settings_mutex);
      }

      if (changed) {
        mark_settings_changed(false);
      }
      update_log_menu_ui();
    }
  // -> new block 2

  // void process_button_event() { // -> в данной реализации функции у меня SCREEN_BRIGHTNESS является последним экраном режима настроек
  //   uint32_t event = bsp_button_read();

  //   if (event == 0) {
  //     return;
  //   }

  //   if (event == 1) {
  //     if (mode_esp == MODE_VIEW_SENSOR_AND_TIME) {
  //       if (screen_sensor_time_date == SCREEN_SENSOR_VALUE) {
  //         screen_sensor_time_date = SCREEN_DATE_TIME_VALUE;
  //         screen_date();
  //       } else if (screen_sensor_time_date == SCREEN_DATE_TIME_VALUE) {
  //         screen_sensor_time_date = SCREEN_CHART_SENSOR_VALUE;
  //         screen_chart_all_value();
  //         // screen_sensor();
  //       } else if (screen_sensor_time_date == SCREEN_CHART_SENSOR_VALUE) {
  //         screen_sensor_time_date = SCREEN_CHART_TEMP_SENSOR_VALUE;
  //         screen_chart_temp_value();
  //         // screen_chart_all_value();
  //       } else if (screen_sensor_time_date == SCREEN_CHART_TEMP_SENSOR_VALUE) {
  //         screen_sensor_time_date = SCREEN_CHART_HUM_SENSOR_VALUE;
  //         screen_chart_hum_value();
  //         // screen_chart_temp_value();
  //       } else if (screen_sensor_time_date == SCREEN_CHART_HUM_SENSOR_VALUE) {
  //         screen_sensor_time_date = SCREEN_SENSOR_VALUE;
  //         screen_sensor();
  //         // screen_chart_hum_value();
  //       }
  //     } else if (mode_esp == MODE_VIEW_SETTINGS) {
  //       if (current_screen_settings == SCREEN_INFO) {
  //         current_screen_settings = SCREEN_MODE_LOG_ESP32;
  //         screen_setting_log_mode();
  //       } 
  //       else if (current_screen_settings == SCREEN_MODE_LOG_ESP32) {
  //         current_screen_settings = SCREEN_BRIGHTNES;
  //         screen_settings_brightness();
  //       } else if (current_screen_settings == SCREEN_BRIGHTNES) {
  //         brightness_step_up();
  //       }
  //     }
  //   } else if (event == 2) {
  //     if (mode_esp == MODE_VIEW_SENSOR_AND_TIME) {
  //       mode_esp = MODE_VIEW_SETTINGS;
  //       current_screen_settings = SCREEN_INFO;
  //       screen_settings();
  //     } else {
  //       mode_esp = MODE_VIEW_SENSOR_AND_TIME;
  //       if (screen_sensor_time_date == SCREEN_SENSOR_VALUE) {
  //         screen_sensor();
  //       } else if (screen_sensor_time_date == SCREEN_DATE_TIME_VALUE) {
  //         screen_date();
  //       } else if (screen_sensor_time_date == SCREEN_CHART_SENSOR_VALUE) {
  //         screen_chart_all_value();
  //       } else if (screen_sensor_time_date == SCREEN_CHART_TEMP_SENSOR_VALUE) {
  //         screen_chart_temp_value();
  //       } else if (screen_sensor_time_date == SCREEN_CHART_HUM_SENSOR_VALUE) {
  //         screen_chart_hum_value();
  //       }
  //     }
  //   }
  // }

  // -> new block 2
    void process_button_event() {
      uint32_t event = bsp_button_read();

      if (event == 0) {
        return;
      }

      // -> короткое нажатие
      if (event == 1) {
        if (mode_esp == MODE_VIEW_SENSOR_AND_TIME) {
          if (screen_sensor_time_date == SCREEN_SENSOR_VALUE) {
            screen_sensor_time_date = SCREEN_DATE_TIME_VALUE;
            screen_date();
          } else if (screen_sensor_time_date == SCREEN_DATE_TIME_VALUE) {
            screen_sensor_time_date = SCREEN_CHART_SENSOR_VALUE;
            screen_chart_all_value();
          } else if (screen_sensor_time_date == SCREEN_CHART_SENSOR_VALUE) {
            screen_sensor_time_date = SCREEN_SENSOR_VALUE;
            screen_sensor();
          }
      } else if (mode_esp == MODE_VIEW_SETTINGS) {
        if (current_screen_settings == SCREEN_INFO) {
          current_screen_settings = SCREEN_MODE_LOG_ESP32;
          log_menu = LOG_ITEM_MODBUS;
          screen_setting_log_mode();
        } else if (current_screen_settings == SCREEN_MODE_LOG_ESP32) {
          handle_log_menu_next();
        } else if (current_screen_settings == SCREEN_BRIGHTNES) {
          brightness_step_up();
        }
      }
      } else if (event == 2) {
        if (mode_esp == MODE_VIEW_SENSOR_AND_TIME) {
          mode_esp = MODE_VIEW_SETTINGS;
          current_screen_settings = SCREEN_INFO;
          screen_settings();
        } else if (mode_esp == MODE_VIEW_SETTINGS) {
          if (current_screen_settings == SCREEN_INFO) {
            mode_esp = MODE_VIEW_SENSOR_AND_TIME;

            if (screen_sensor_time_date == SCREEN_SENSOR_VALUE) {
              screen_sensor();
            } else if (screen_sensor_time_date == SCREEN_DATE_TIME_VALUE) {
              screen_date();
            } else if (screen_sensor_time_date == SCREEN_CHART_SENSOR_VALUE) {
              screen_chart_all_value();
            } 
          } else if (current_screen_settings == SCREEN_MODE_LOG_ESP32) {
            handle_log_menu_select();
          } else if (current_screen_settings == SCREEN_BRIGHTNES) {
            mode_esp = MODE_VIEW_SENSOR_AND_TIME;

            if (screen_sensor_time_date == SCREEN_SENSOR_VALUE) {
              screen_sensor();
            } else if (screen_sensor_time_date == SCREEN_DATE_TIME_VALUE) {
              screen_date();
            } else if (screen_sensor_time_date == SCREEN_CHART_SENSOR_VALUE) {
              screen_chart_all_value();
            } 
          }
        }
      }
    }
  // -> new block 2

  void my_disp_flush(lv_disp_drv_t* disp_drv, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t width = area -> x2 - area -> x1 + 1;
    uint32_t height = area -> y2 - area -> y1 + 1;
    gfx -> draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)color_p, width, height);
    lv_disp_flush_ready(disp_drv);
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
                .textContent = await responseTemp.text();

              document
                .getElementById('humidity')
                .textContent = await responseHum.text();
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

    // const char settings_html[] PROGMEM = R"rawliteral(
    //   <!DOCTYPE html>
    //   <html lang="ru">
    //   <head>
    //   <meta charset="UTF-8" />
    //   <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    //   <meta name="color-scheme" content="dark" />
    //   <title>Настройки ESP32</title>

    //   <style>
    //     :root{
    //     --bg0:#060f22; --bg1:#071a33; --stroke:rgba(255,255,255,.10);
    //     --text:#e6eef3; --muted:#9aa6b2; --accent1:#22c1c3; --accent2:#4ee0a8;
    //     --shadow: 0 14px 40px rgba(0,0,0,.45); --r: 18px; --max: 780px;
    //   }

    //     *{ box-sizing:border-box; margin:0; }

    //     body{
    //     color:var(--text);
    //     font-family: system-ui, -apple-system, sans-serif;
    //     background: radial-gradient(1100px 700px at 15% 0%, rgba(34,193,195,.18), transparent 60%),
    //                 radial-gradient(900px 600px at 95% 20%, rgba(78,224,168,.14), transparent 55%),
    //                 linear-gradient(180deg, var(--bg0) 0%, var(--bg1) 65%, #060f22 100%);
    //     min-height:100vh;
    //     padding:16px;
    //     }

    //     .wrap{
    //       max-width:var(--max); margin:0 auto;
    //     }

    //     header{
    //       text-align:center; padding:12px 8px 6px;
    //     }
    //     h1{ font-size:clamp(18px, 3.8vw, 26px); margin:0; }
    //     .subtitle{ color:var(--muted); font-size:clamp(13px, 2.5vw, 15px); margin-top:6px; }

    //     .card{
    //     background: linear-gradient(180deg, rgba(255,255,255,.06), rgba(255,255,255,.03));
    //     border:1px solid var(--stroke); border-radius:var(--r);
    //     backdrop-filter:blur(10px); box-shadow:var(--shadow); margin-top:14px;
    //     }
    //     .card-inner{ padding:16px; }
    //     @media (min-width:420px){ .card-inner{ padding:18px; } }

    //     .section{
    //     padding:12px 14px; border-radius:14px;
    //     background:rgba(15,23,36,.55); border:1px solid rgba(255,255,255,.07);
    //     margin-bottom:14px;
    //     }
    //     .section-title{ font-weight:800; font-size:14px; }
    //     .section-desc{ color:var(--muted); font-size:12px; margin-top:2px; }

    //     .field{ margin-bottom:12px; }
    //     label{ color:var(--muted); font-size:12px; display:block; margin-bottom:6px; }
      
    //     .input-group{ display:flex; gap:10px; }
    //     .input, select{
    //       width:100%; padding:12px; border-radius:12px; border:1px solid rgba(255,255,255,.10);
    //       background:rgba(6,15,34,.55); color:var(--text); font-size:14px;
    //     }
    //     .input:focus, select:focus{
    //     border-color:rgba(78,224,168,.35); box-shadow:0 0 0 3px rgba(78,224,168,.12);
    //     outline:none;
    //     }
      
    //     .unit{
    //     color:var(--muted); font-size:12px; white-space:nowrap;
    //     padding:10px; border-radius:12px; border:1px solid rgba(255,255,255,.10);
    //     background:rgba(6,15,34,.35);
    //     }

    //     .option{
    //     padding:14px; border-radius:14px; background:rgba(15,23,36,.72);
    //     border:1px solid rgba(255,255,255,.07); margin-bottom:12px;
    //     }
    //     .option-header{
    //     display:flex; align-items:flex-start; gap:10px; cursor:pointer;
    //     }
    //     .option-title{ font-weight:800; font-size:14px; }
    //     .option-desc{ color:var(--muted); font-size:12px; margin-top:4px; }
      
    //     .panel{
    //     margin-top:10px; padding:12px; border-radius:14px;
    //     background:rgba(255,255,255,.03); border:1px dashed rgba(255,255,255,.12);
    //     }
    //     .hidden{ display:none; }

    //     .grid{ display:grid; gap:10px; }
    //     @media (min-width:720px){ .grid{ grid-template-columns:1fr 1fr; } }

    //     .actions{
    //     display:flex; flex-direction:column; gap:10px; margin-top:20px;
    //     }
    //     @media (min-width:520px){ .actions{ flex-direction:row; } }
      
    //     .btn{
    //     padding:12px 14px; border-radius:14px; border:none; font-size:14px;
    //     font-weight:800; cursor:pointer; flex:1; text-align:center;
    //     text-decoration:none; display:inline-block;
    //     }
    //     .btn-primary{
    //     background:linear-gradient(90deg, var(--accent1), var(--accent2));
    //     color:#062017; box-shadow:0 14px 30px rgba(34,193,195,.18);
    //     }
    //     .btn-secondary{
    //     background:rgba(15,23,36,.55); color:var(--text);
    //     border:1px solid rgba(255,255,255,.12);
    //     }

    //     .hint{ color:rgba(154,166,178,.85); font-size:12px; margin-top:6px; }
    //   </style>
    //   </head>

    //   <body>
    //   <div class="wrap">
    //   <header>
    //     <h1>Настройки ESP32</h1>
    //     <p class="subtitle">Частота опроса и сохранение данных</p>
    //   </header>

    //   <section class="card">
    //     <form class="card-inner" action="/" method="GET">

    //       <div class="section">
    //         <div class="section-title">Частота получения данных</div>
    //         <div class="section-desc">Интервал чтения датчика (в секундах).</div>
    //       </div>

    //       <div class="field">
    //         <label for="poll_s">Интервал опроса</label>
    //         <div class="input-group">
    //           <select class="input" id="poll_s" name="poll_s">
    //             <option value="2" selected>2</option><option value="5">5</option>
    //             <option value="10">10</option><option value="15">15</option>
    //             <option value="30">30</option>
    //           </select>
    //           <span class="unit">сек</span>
    //         </div>
    //         <div class="hint">Минимальный интервал записи для всех способов сохранения.</div>
    //       </div>

    //       <div class="section">
    //         <div class="section-title">Сохранение данных</div>
    //         <div class="section-desc">Выбери один или несколько способов</div>
    //       </div>

    //       <!-- Google Sheets -->
    //       <div class="option">
    //         <div class="option-header" onclick="document.getElementById('use_gs').click()">
    //           <input type="checkbox" id="use_gs" name="use_gs" style="margin-top:2px;">
    //           <div>
    //             <div class="option-title">Google Таблицы</div>
    //             <div class="option-desc">Отправка данных в Google Sheets</div>
    //           </div>
    //         </div>
            
    //         <div id="panel_gs" class="panel hidden">
    //           <div class="grid">
    //             <div><label for="wifi_ssid">Wi-Fi SSID</label>
    //               <input class="input" id="wifi_ssid" name="wifi_ssid" placeholder="Имя сети"></div>
    //             <div><label for="wifi_pass">Wi-Fi пароль</label>
    //               <input class="input" id="wifi_pass" name="wifi_pass" placeholder="Пароль" type="password"></div>
    //           </div>
              
    //           <div class="grid">
    //             <div><label for="gs_url">URL Google Sheets</label>
    //               <input class="input" id="gs_url" name="gs_url" placeholder="https://script.google.com/..."></div>
    //             <div><label for="gs_key">Секретный ключ</label>
    //               <input class="input" id="gs_key" name="gs_key" placeholder="Ваш ключ"></div>
    //           </div>
              
    //           <div style="margin-top:10px;">
    //             <label for="gs_period_s">Скорость отправки</label>
    //             <div class="input-group">
    //               <select class="input save-period" id="gs_period_s" name="gs_period_s">
    //                 <option value="2">2</option><option value="5" selected>5</option>
    //                 <option value="10">10</option><option value="15">15</option>
    //                 <option value="30">30</option>
    //               </select>
    //               <span class="unit">сек</span>
    //             </div>
    //           </div>
    //         </div>

    //         <div style="margin-top:14px;">
    //           <button type="button" class="btn btn-secondary" id="gen-gas">
    //             ⚙️ Сгенерировать Google Script
    //           </button>
    //         </div>

    //         <div id="gas_container" class="panel hidden" style="margin-top:10px;">
    //           <label>Google Apps Script</label>
    //           <textarea id="gas_script" class="input" rows="10" readonly style="resize:vertical;"></textarea>
              
    //           <div class="actions" style="margin-top:10px;">
    //             <button type="button" class="btn btn-primary" id="copy-gas">
    //               📋 Скопировать
    //             </button>
    //           </div>
    //         </div>
    //       </div>

    //       <!-- Excel -->
    //       <div class="option">
    //         <div class="option-header" onclick="document.getElementById('use_excel').click()">
    //           <input type="checkbox" id="use_excel" name="use_excel" style="margin-top:2px;">
    //           <div>
    //             <div class="option-title">Excel (Data Streamer)</div>
    //             <div class="option-desc">Передача данных в Excel</div>
    //           </div>
    //         </div>
            
    //         <div id="panel_excel" class="panel hidden">
    //           <label for="excel_period_s">Скорость передачи</label>
    //           <div class="input-group">
    //             <select class="input save-period" id="excel_period_s" name="excel_period_s">
    //               <option value="2" selected>2</option><option value="5">5</option>
    //               <option value="10">10</option><option value="15">15</option>
    //               <option value="30">30</option>
    //             </select>
    //             <span class="unit">сек</span>
    //           </div>
    //         </div>
    //       </div>

    //       <!-- Файловая система -->
    //       <div class="option">
    //         <div class="option-header" onclick="document.getElementById('use_fs').click()">
    //           <input type="checkbox" id="use_fs" name="use_fs" style="margin-top:2px;">
    //           <div>
    //             <div class="option-title">Файловая система ESP32</div>
    //             <div class="option-desc">Локальное логирование</div>
    //           </div>
    //         </div>
            
    //         <div id="panel_fs" class="panel hidden">
    //           <div style="margin-bottom:10px;">
    //             <label for="fs_period_s">Скорость логирования</label>
    //             <div class="input-group">
    //               <select class="input save-period" id="fs_period_s" name="fs_period_s">
    //                 <option value="2" selected>2</option><option value="5">5</option>
    //                 <option value="10">10</option><option value="15">15</option>
    //                 <option value="30">30</option>
    //               </select>
    //               <span class="unit">сек</span>
    //             </div>
    //           </div>
              
    //           <div class="grid">
    //             <div><label for="fs_filename">Имя файла</label>
    //               <input class="input" id="fs_filename" name="fs_filename" value="log.txt"></div>
    //             <div><label for="fs_format">Формат</label>
    //               <input class="input" id="fs_format" name="fs_format" value="txt"></div>
    //           </div>
    //         </div>
    //       </div>

    //       <!-- SD-карта -->
    //       <div class="option">
    //         <div class="option-header" onclick="document.getElementById('use_sd').click()">
    //           <input type="checkbox" id="use_sd" name="use_sd" style="margin-top:2px;">
    //           <div>
    //             <div class="option-title">SD-карта</div>
    //             <div class="option-desc">Запись данных на SD-карту</div>
    //           </div>
    //         </div>
            
    //         <div id="panel_sd" class="panel hidden">
    //           <div style="margin-bottom:10px;">
    //             <label for="sd_period_s">Скорость записи</label>
    //             <div class="input-group">
    //               <select class="input save-period" id="sd_period_s" name="sd_period_s">
    //                 <option value="2" selected>2</option>
    //                 <option value="5">5</option>
    //                 <option value="10">10</option>
    //                 <option value="15">15</option>
    //                 <option value="30">30</option>
    //               </select>
    //               <span class="unit">сек</span>
    //             </div>
    //           </div>
              
    //           <div class="grid">
    //             <div>
    //               <label for="sd_filename">Имя файла</label>
    //               <input class="input" id="sd_filename" name="sd_filename" value="sd_log.txt">
    //             </div>
    //             <div>
    //               <label for="sd_format">Формат</label>
    //               <input class="input" id="sd_format" name="sd_format" value="txt">
    //             </div>
    //           </div>
    //         </div>
    //       </div>

    //       <div class="actions">
    //         <button class="btn btn-primary" type="button" id="send-server">✅ Применить</button>
    //         <a class="btn btn-secondary" href="/">← Назад к показаниям</a>
    //       </div>

    //       <div class="hint" style="margin-top:14px;">
    //         Сейчас форма отправляет параметры через URL (GET). Для пароля/ключа лучше сделать POST.
    //       </div>
    //     </form>
    //   </section>
    //   </div>

    //   <script>

    //     function sendSettingsOnServer() { // -> привязка к кнопке и отправка на сервер настроек
    //       document
    //         .getElementById("send-server")
    //         .addEventListener('click', async () => {
    //           try {
    //             let valueInterval = document.getElementById('poll_s').value; // -> первый input - интервал чтения modbus
    //             let valueCheckBoxGoogle = document.getElementById('use_gs').checked;
    //             let valueCheckBoxExcel = document.getElementById('use_excel').checked;
    //             let valueCheckBoxFileSystem = document.getElementById('use_fs').checked;
    //             let valueCheckBoxSd = document.getElementById('use_sd').checked;

    //             let valueSsidWifi = document.getElementById('wifi_ssid').value;
    //             let valuePasswordWifi = document.getElementById('wifi_pass').value;
    //             let valueUrl = document.getElementById('gs_url').value;
    //             let valueSecretKey = document.getElementById('gs_key').value;
    //             let valueWriteInGoogle = document.getElementById('gs_period_s').value;

    //             let valueWriteInExcel = document.getElementById('excel_period_s').value;

    //             let valueWriteFs = document.getElementById('fs_period_s').value;
    //             let valueFileName = document.getElementById('fs_filename').value;

    //             let valueWriteSd = document.getElementById('sd_period_s').value;
    //             let valueSdFileName = document.getElementById('sd_filename').value;

    //             await fetch('/get_settings', {
    //               method: 'POST',
    //               headers: {
    //                 'Content-Type': 'application/json',
    //               },
    //               body: JSON.stringify({
    //                 "interval": valueInterval,
    //                 "google": {
    //                   "flag-google": valueCheckBoxGoogle,
    //                   "wifi-ssid": valueSsidWifi,
    //                   "wifi-pass": valuePasswordWifi,
    //                   "url-gs": valueUrl,
    //                   "secret-key": valueSecretKey,
    //                   "interval-write": valueWriteInGoogle
    //                 },
    //                 "excel": {
    //                   "flag-excel": valueCheckBoxExcel,
    //                   "interval-write": valueWriteInExcel
    //                 },
    //                 "file-system": {
    //                   "flag-fs": valueCheckBoxFileSystem,
    //                   "interval-write": valueWriteFs,
    //                   "file-name": valueFileName
    //                 },
    //                 "sd-card": {
    //                   "flag-sd": valueCheckBoxSd,
    //                   "interval-write": valueWriteSd,
    //                   "file-name": valueSdFileName
    //                 }
    //               })
    //             });
    //           } catch(error) {
    //             console.error(error);
    //           }
    //         });
    //     }

    //     sendSettingsOnServer();

    //     document.addEventListener('DOMContentLoaded', function() {
    //       // Показ/скрытие панелей настроек
    //       ['gs', 'excel', 'fs', 'sd'].forEach(id => {
    //         const chk = document.getElementById('use_' + id);
    //         const panel = document.getElementById('panel_' + id);
    //         if (chk && panel) {
    //           chk.addEventListener('change', () => panel.classList.toggle('hidden', !chk.checked));
    //         }
    //       });

    //       // Валидация периодов сохранения
    //       const pollSelect = document.getElementById('poll_s');
    //       const saveSelects = document.querySelectorAll('.save-period');
        
    //       function updateSavePeriods() {
    //         const min = parseInt(pollSelect.value);
          
    //         saveSelects.forEach(select => {
    //           Array.from(select.options).forEach(option => {
    //             option.disabled = parseInt(option.value) < min;
    //           });
            
    //           // Автоматический выбор допустимого значения
    //           if (parseInt(select.value) < min) {
    //             const validOptions = Array.from(select.options).filter(opt => !opt.disabled);
    //             if (validOptions.length > 0) {
    //               select.value = validOptions[0].value;
    //             }
    //           }
    //         });
    //       }
        
    //       if (pollSelect) {
    //         pollSelect.addEventListener('change', updateSavePeriods);
    //         updateSavePeriods();
    //       }
    //     });

    //       document.getElementById("gen-gas").addEventListener("click", async () => {
    //         try {
    //           const key = document.getElementById("gs_key").value;

    //           if (!key) {
    //             alert("Введите секретный ключ!");
    //             return;
    //           }

    //           const response = await fetch('/get_gas_script?key=' + encodeURIComponent(key));;
    //           const text = await response.text();

    //           const container = document.getElementById("gas_container");
    //           const textarea = document.getElementById("gas_script");

    //           textarea.value = text;
    //           container.classList.remove("hidden");

    //         } catch (err) {
    //           console.error(err);
    //           alert("Ошибка получения скрипта");
    //         }
    //       });

    //       // Копирование
    //       document.getElementById("copy-gas").addEventListener("click", () => {
    //         const textarea = document.getElementById("gas_script");
    //         textarea.select();
    //         textarea.setSelectionRange(0, 99999);

    //         document.execCommand("copy");
    //         alert("Скрипт скопирован!");
    //       });

    //     // async function loadRuntimeSettings() {
    //     //   try {
    //     //     const response = await fetch('/get_runtime_settings');
    //     //     if (!response.ok) {
    //     //       return;
    //     //     }
    //     //     const data = await response.json();
    //     //     document.getElementById('use_gs').checked = !!data.google["flag-google"];
    //     //     document.getElementById('use_excel').checked = !!data.excel["flag-excel"];
    //     //     document.getElementById('use_fs').checked = !!data["file-system"]["flag-fs"];
    //     //     const panelGs = document.getElementById('panel_gs');
    //     //     const panelExcel = document.getElementById('panel_excel');
    //     //     const panelFs = document.getElementById('panel_fs');

    //     //     if (panelGs) {
    //     //       panelGs.classList.toggle('hidden', !document.getElementById('use_gs').checked);
    //     //     }
    //     //     if (panelExcel) {
    //     //       panelExcel.classList.toggle('hidden', !document.getElementById('use_excel').checked);
    //     //     }
    //     //     if (panelFs) {
    //     //       panelFs.classList.toggle('hidden', !document.getElementById('use_fs').checked);
    //     //     }
    //     //   } catch (error) {
    //     //     console.error(error);
    //     //   }
    //     // }
    //     // loadRuntimeSettings();

    //     fetch('/get_runtime_settings')
    //       .then(function(response) {
    //         if (!response.ok) {
    //           return null;
    //         }
    //         return response.json();
    //       })
    //       .then(function(data) {
    //         if (!data) {
    //           return;
    //         }

    //         document.getElementById('use_gs').checked = !!data.google["flag-google"];
    //         document.getElementById('use_excel').checked = !!data.excel["flag-excel"];
    //         document.getElementById('use_fs').checked = !!data["file-system"]["flag-fs"];

    //         const panelGs = document.getElementById('panel_gs');
    //         const panelExcel = document.getElementById('panel_excel');
    //         const panelFs = document.getElementById('panel_fs');

    //         if (panelGs) {
    //           panelGs.classList.toggle('hidden', !document.getElementById('use_gs').checked);
    //         }
    //         if (panelExcel) {
    //           panelExcel.classList.toggle('hidden', !document.getElementById('use_excel').checked);
    //         }
    //         if (panelFs) {
    //           panelFs.classList.toggle('hidden', !document.getElementById('use_fs').checked);
    //         }
    //       })
    //       .catch(function(error) {
    //         console.error(error);
    //       });

      
    //   </script>
    //   </body>
    //   </html>
    // )rawliteral";

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
            <div class="section-desc">Интервал чтения датчика Modbus (в секундах).</div>
          </div>

          <div class="field">
            <label for="poll_s">Интервал опроса Modbus</label>
            <div class="input-group">
              <select class="input" id="poll_s" name="poll_s">
                <option value="2" selected>2</option>
                <option value="5">5</option>
                <option value="10">10</option>
                <option value="15">15</option>
                <option value="30">30</option>
              </select>
              <span class="unit">сек</span>
            </div>
            <div class="hint">Этот интервал используется как общий такт чтения датчика.</div>
          </div>

          <div class="option">
              <div class="option-header" onclick="document.getElementById('modbus_enabled').click()">
                <input type="checkbox" id="modbus_enabled" name="modbus_enabled" style="margin-top:2px;" checked>
              <div>
                <div class="option-title">Modbus / Sensor read</div>
                <div class="option-desc">Включить или выключить опрос датчика</div>
              </div>
            </div>
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
                <div>
                  <label for="wifi_ssid">Wi-Fi SSID</label>
                  <input class="input" id="wifi_ssid" name="wifi_ssid" placeholder="Имя сети">
                </div>
                <div>
                  <label for="wifi_pass">Wi-Fi пароль</label>
                  <input class="input" id="wifi_pass" name="wifi_pass" placeholder="Пароль" type="password">
                </div>
              </div>

              <div class="grid">
                <div>
                  <label for="gs_url">URL Google Sheets</label>
                  <input class="input" id="gs_url" name="gs_url" placeholder="https://script.google.com/...">
                </div>
                <div>
                  <label for="gs_key">Секретный ключ</label>
                  <input class="input" id="gs_key" name="gs_key" placeholder="Ваш ключ">
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
              <div class="hint">Дополнительные параметры для Excel не требуются.</div>
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
              <div class="grid">
                <div>
                  <label for="fs_filename">Имя файла</label>
                  <input class="input" id="fs_filename" name="fs_filename" value="log.txt">
                </div>
                <div>
                  <label for="fs_format">Формат</label>
                  <input class="input" id="fs_format" name="fs_format" value="txt">
                </div>
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
            Настройки отправляются на сервер в JSON через POST.
          </div>
        </form>
      </section>
      </div>

      <script>
        function sendSettingsOnServer() {
          document.getElementById("send-server").addEventListener('click', async function() {
            try {
              let valueInterval = document.getElementById('poll_s').value;
              let valueModbusEnabled = document.getElementById('modbus_enabled').checked;
              let valueCheckBoxGoogle = document.getElementById('use_gs').checked;
              let valueCheckBoxExcel = document.getElementById('use_excel').checked;
              let valueCheckBoxFileSystem = document.getElementById('use_fs').checked;
              let valueCheckBoxSd = document.getElementById('use_sd').checked;

              let valueSsidWifi = document.getElementById('wifi_ssid').value;
              let valuePasswordWifi = document.getElementById('wifi_pass').value;
              let valueUrl = document.getElementById('gs_url').value;
              let valueSecretKey = document.getElementById('gs_key').value;

              let valueFileName = document.getElementById('fs_filename').value;
              let valueFsFormat = document.getElementById('fs_format').value;

              let valueSdFileName = document.getElementById('sd_filename').value;
              let valueSdFormat = document.getElementById('sd_format').value;

              const response = await fetch('/get_settings', {
                method: 'POST',
                headers: {
                  'Content-Type': 'application/json',
                },

                body: JSON.stringify({
                  "interval": valueInterval,
                  "modbus-enabled": valueModbusEnabled,
                  "google": {
                    "flag-google": valueCheckBoxGoogle,
                    "wifi-ssid": valueSsidWifi,
                    "wifi-pass": valuePasswordWifi,
                    "url-gs": valueUrl,
                    "secret-key": valueSecretKey
                  },
                  "excel": {
                    "flag-excel": valueCheckBoxExcel
                  },
                  "file-system": {
                    "flag-fs": valueCheckBoxFileSystem,
                    "file-name": valueFileName,
                    "format": valueFsFormat
                  },
                  "sd-card": {
                    "flag-sd": valueCheckBoxSd,
                    "file-name": valueSdFileName,
                    "format": valueSdFormat
                  }
                })
              });

              if (response.ok) {
                alert("Настройки сохранены");
              } else {
                alert("Ошибка сохранения настроек");
              }
            } catch(error) {
              console.error(error);
              alert("Ошибка отправки настроек");
            }
          });
        }

        sendSettingsOnServer();

        document.addEventListener('DOMContentLoaded', function() {
          ['gs', 'excel', 'fs', 'sd'].forEach(function(id) {
            const chk = document.getElementById('use_' + id);
            const panel = document.getElementById('panel_' + id);
            if (chk && panel) {
              chk.addEventListener('change', function() {
                panel.classList.toggle('hidden', !chk.checked);
              });
            }
          });
        });

        document.getElementById("gen-gas").addEventListener("click", async function() {
          try {
            const key = document.getElementById("gs_key").value;

            if (!key) {
              alert("Введите секретный ключ!");
              return;
            }

            const response = await fetch('/get_gas_script?key=' + encodeURIComponent(key));
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

        document.getElementById("copy-gas").addEventListener("click", function() {
          const textarea = document.getElementById("gas_script");
          textarea.select();
          textarea.setSelectionRange(0, 99999);
          document.execCommand("copy");
          alert("Скрипт скопирован!");
        });

        fetch('/get_runtime_settings')
          .then(function(response) {
            if (!response.ok) {
              return null;
            }
            return response.json();
          })
          .then(function(data) {
            if (!data) {
              return;
            }

            if (data.interval !== undefined) {
              document.getElementById('poll_s').value = String(data.interval);
            }
            if (data["modbus-enabled"] !== undefined) {
              document.getElementById('modbus_enabled').checked = !!data["modbus-enabled"];
            }
            document.getElementById('use_gs').checked = !!data.google["flag-google"];
            document.getElementById('use_excel').checked = !!data.excel["flag-excel"];
            document.getElementById('use_fs').checked = !!data["file-system"]["flag-fs"];
            document.getElementById('use_sd').checked = !!data["sd-card"]["flag-sd"];

            if (data.google["wifi-ssid"] !== undefined) {
              document.getElementById('wifi_ssid').value = data.google["wifi-ssid"];
            }
            if (data.google["wifi-pass"] !== undefined) {
              document.getElementById('wifi_pass').value = data.google["wifi-pass"];
            }
            if (data.google["url-gs"] !== undefined) {
              document.getElementById('gs_url').value = data.google["url-gs"];
            }
            if (data.google["secret-key"] !== undefined) {
              document.getElementById('gs_key').value = data.google["secret-key"];
            }

            if (data["file-system"]["file-name"] !== undefined) {
              document.getElementById('fs_filename').value = data["file-system"]["file-name"];
            }
            if (data["file-system"]["format"] !== undefined) {
              document.getElementById('fs_format').value = data["file-system"]["format"];
            }

            if (data["sd-card"]["file-name"] !== undefined) {
              document.getElementById('sd_filename').value = data["sd-card"]["file-name"];
            }
            if (data["sd-card"]["format"] !== undefined) {
              document.getElementById('sd_format').value = data["sd-card"]["format"];
            }

            document.getElementById('panel_gs').classList.toggle('hidden', !document.getElementById('use_gs').checked);
            document.getElementById('panel_excel').classList.toggle('hidden', !document.getElementById('use_excel').checked);
            document.getElementById('panel_fs').classList.toggle('hidden', !document.getElementById('use_fs').checked);
            document.getElementById('panel_sd').classList.toggle('hidden', !document.getElementById('use_sd').checked);
          })
          .catch(function(error) {
            console.error(error);
          });
      </script>
      </body>
      </html>
    )rawliteral";

    String build_file_path(const String& file_name, const String& file_format);

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

      // server.on("/get_settings", 
      //   HTTP_POST, 
      //   [](AsyncWebServerRequest* request){}, 
      //   NULL,
      //   [](AsyncWebServerRequest* request, uint8_t* data_part, size_t len_part, size_t index_start_part, size_t total_size) {
      //     String* json_object = new String();
      //     json_object -> reserve(total_size);

      //     for (size_t i = 0; i < len_part; i++) {
      //       json_object -> concat((char)data_part[i]);
      //     }

      //     DynamicJsonDocument document(2048);
      //     DeserializationError deserialize_document = deserializeJson(document, *json_object);
      //     if (deserialize_document) {
      //       return;
      //     }

      //     // -> modbus
      //     JsonObject root = document.as<JsonObject>();
      //     int interval = atoi(root["interval"] | "2");
      //     preferences.putInt("interval_modbus", interval);

      //     // -> google
      //     JsonObject google = root["google"];
      //     bool flag_google = google["flag-google"] | false;
      //     preferences.putBool("flag_google", flag_google);

      //     String ssid = google["wifi-ssid"] | "nothing";
      //     preferences.putString("ssid_wifi", ssid);

      //     String password = google["wifi-pass"] | "nothing";
      //     preferences.putString("password_wifi", password);

      //     String url_google_sheet = google["url-gs"] | "nothing";
      //     preferences.putString("url_google", url_google_sheet);

      //     String secret_key = google["secret-key"] | "nothing";
      //     preferences.putString("secret_key", secret_key);

      //     // -> excel
      //     JsonObject excel = root["excel"];
      //     bool excel_flag = excel["flag_excel"] | false;
      //     preferences.putBool("flag_excel", excel_flag);

      //     // -> fs
      //     JsonObject file_system = root["file-system"];
      //     bool fs_flag = file_system["flag-fs"] | false;
      //     preferences.putBool("flag_file_system", fs_flag);

      //     request -> send(200, "text/plain", "ok");
      //     ESP.restart();
      // });

      // -> new block 2
        server.on("/get_settings",
          HTTP_POST,
          [](AsyncWebServerRequest* request) {},
          NULL,
          [](AsyncWebServerRequest* request, uint8_t* data_part, size_t len_part, size_t index_start_part, size_t total_size) {
            static String json_buffer;

            if (index_start_part == 0) {
              json_buffer = "";
              json_buffer.reserve(total_size);
            }

            for (size_t i = 0; i < len_part; i++) {
              json_buffer += (char)data_part[i];
            }

            if (index_start_part + len_part != total_size) {
              return;
            }

            DynamicJsonDocument document(2048);
            DeserializationError err = deserializeJson(document, json_buffer);
            if (err) {
              request->send(400, "text/plain", "invalid json");
              return;
            }

            bool restart_timer = false;

            if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(200))) {
              JsonObject root = document.as<JsonObject>();

              int new_interval = atoi(root["interval"] | "2");
              if (app_settings.interval_modbus != new_interval) {
                app_settings.interval_modbus = new_interval;
                restart_timer = true;
              }

              if (root.containsKey("modbus-enabled")) {
                app_settings.modbus_enabled = root["modbus-enabled"] | true;
              }

              JsonObject google = root["google"];
              app_settings.google_enabled = google["flag-google"] | false;
              String wifi_ssid = google["wifi-ssid"] | "";
              String wifi_password = google["wifi-pass"] | "";
              String url_google = google["url-gs"] | "";
              String secret_key = google["secret-key"] | "";

              preferences.putString("ssid_wifi", wifi_ssid);
              preferences.putString("password_wifi", wifi_password);
              preferences.putString("url_google", url_google);
              preferences.putString("secret_key", secret_key);

              JsonObject excel = root["excel"];
              app_settings.excel_enabled = excel["flag-excel"] | false;

              JsonObject file_system = root["file-system"];
              app_settings.fs_enabled = file_system["flag-fs"] | false;
              app_settings.fs_file_name = String((const char*)(file_system["file-name"] | "log"));
              app_settings.fs_format = String((const char*)(file_system["format"] | "txt"));

              // String fs_file_name = file_system["file-name"] | "log.txt";
              // String fs_format = file_system["format"] | "txt";
              // preferences.putString("fs_file_name", fs_file_name);
              // preferences.putString("fs_format", fs_format);

              JsonObject sd = root["sd-card"];
              app_settings.sd_enabled = sd["flag-sd"] | false;
              app_settings.sd_file_name = String((const char*)(sd["file-name"] | "sd_log"));
              app_settings.sd_format = String((const char*)(sd["format"] | "txt"));
              
              // bool sd_enabled = sd["flag-sd"] | false;
              // String sd_file_name = sd["file-name"] | "sd_log.txt";
              // String sd_format = sd["format"] | "txt";
              // preferences.putBool("sd_en", sd_enabled);
              // preferences.putString("sd_file_name", sd_file_name);
              // preferences.putString("sd_format", sd_format);

              normalize_settings_locked();
              xSemaphoreGive(settings_mutex);
            }

            mark_settings_changed(restart_timer);
            request->send(200, "text/plain", "ok");
          }
        );
      // -> new block 2

      server.on("/get_gas_script", HTTP_GET, [](AsyncWebServerRequest* request) {
          String secret_key  = "";

          if (request -> hasParam("key")) {
            secret_key = request -> getParam("key") -> value();
          }

          String gas_script = "";
          gas_script += "function doPost(e) {\n";
          gas_script += "  var SECRET = \"" + secret_key + "\";\n\n";

          gas_script += "  if (!e.postData || !e.postData.contents) {\n";
          gas_script += "    return badResponse(\"no_post_data\");\n";
          gas_script += "  }\n\n";

          gas_script += "  var payload;\n";
          gas_script += "  try {\n";
          gas_script += "    payload = JSON.parse(e.postData.contents);\n";
          gas_script += "  } catch (err) {\n";
          gas_script += "    return badResponse(\"invalid_json\");\n";
          gas_script += "  }\n\n";

          gas_script += "  if (!payload.key || payload.key !== SECRET) {\n";
          gas_script += "    return badResponse(\"invalid_key\");\n";
          gas_script += "  }\n\n";

          gas_script += "  var today = getTodaySheetName();\n";
          gas_script += "  var ss = SpreadsheetApp.getActiveSpreadsheet();\n";
          gas_script += "  var sheet = ss.getSheetByName(today);\n\n";

          gas_script += "  if (!sheet) {\n";
          gas_script += "    sheet = ss.insertSheet(today);\n";
          gas_script += "    sheet.appendRow([\"Timestamp\", \"Temperature\", \"Humidity\", \"Info\"]);\n";
          gas_script += "  }\n\n";

          gas_script += "  var temp = payload.temperature == null ? \"\" : payload.temperature;\n";
          gas_script += "  var hum  = payload.humidity == null ? \"\" : payload.humidity;\n";
          gas_script += "  var info = payload.info || \"\";\n\n";

          gas_script += "  try {\n";
          gas_script += "    sheet.appendRow([ new Date(), temp, hum, info ]);\n";
          gas_script += "  } catch (err) {\n";
          gas_script += "    return badResponse(\"append_failed: \" + err);\n";
          gas_script += "  }\n\n";

          gas_script += "  return ContentService\n";
          gas_script += "    .createTextOutput(JSON.stringify({ result: \"ok\" }))\n";
          gas_script += "    .setMimeType(ContentService.MimeType.JSON);\n";
          gas_script += "}\n\n";

          gas_script += "function getTodaySheetName() {\n";
          gas_script += "  var d = new Date();\n";
          gas_script += "  var year  = d.getFullYear();\n";
          gas_script += "  var month = (\"0\" + (d.getMonth() + 1)).slice(-2);\n";
          gas_script += "  var day   = (\"0\" + d.getDate()).slice(-2);\n";
          gas_script += "  return year + \"-\" + month + \"-\" + day;\n";
          gas_script += "}\n\n";

          gas_script += "function badResponse(code) {\n";
          gas_script += "  return ContentService\n";
          gas_script += "    .createTextOutput(JSON.stringify({ result: \"error\", code: code }))\n";
          gas_script += "    .setMimeType(ContentService.MimeType.JSON);\n";
          gas_script += "}\n";

          request -> send(200, "text/plain", gas_script);
      });

      // server.on("/getFile", HTTP_GET, [](AsyncWebServerRequest* request) {
      //   String path_file = "/log.txt";
      //   String file_name = "log.txt";
      //   if (!LittleFS.exists(path_file)) {
      //     request -> send(400, "text/plain", "File not found");
      //     Serial.println("File not found");
      //     return;
      //   }
      //   AsyncWebServerResponse* response = request -> beginResponse(LittleFS, path_file, "text/plain");
      //   response -> addHeader("Content-Disposition", "attachment; filename=\"" + file_name + "\"");
      //   request -> send(response);
      // });

      // -> new block 3
        server.on("/getFile", HTTP_GET, [](AsyncWebServerRequest* request) {
          String path_file = "/log.txt";
          String file_name = "log.txt";

          if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(100))) {
            path_file = build_file_path(app_settings.fs_file_name, app_settings.fs_format);
            file_name = app_settings.fs_file_name + "." + app_settings.fs_format;
            xSemaphoreGive(settings_mutex);
          }

          if (!LittleFS.exists(path_file)) {
            request->send(400, "text/plain", "File not found");
            Serial.println("File not found");
            return;
          }

          AsyncWebServerResponse* response = request->beginResponse(LittleFS, path_file, "text/plain");
            response->addHeader("Content-Disposition", "attachment; filename=\"" + file_name + "\"");
            request->send(response);
        });
      // -> new block 3

      // -> new block 2
        // server.on("/get_runtime_settings", HTTP_GET, [](AsyncWebServerRequest* request) {
        //   DynamicJsonDocument doc(512);

        //   if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(100))) {
        //     doc["modbus-enabled"] = app_settings.modbus_enabled;
        //     doc["interval"] = app_settings.interval_modbus;
        //     doc["brightness"] = app_settings.brightness;

        //     JsonObject google = doc.createNestedObject("google");
        //     google["flag-google"] = app_settings.google_enabled;

        //     JsonObject excel = doc.createNestedObject("excel");
        //     excel["flag-excel"] = app_settings.excel_enabled;

        //     JsonObject fs = doc.createNestedObject("file-system");
        //     fs["flag-fs"] = app_settings.fs_enabled;

        //     xSemaphoreGive(settings_mutex);
        //   }

        //   String out;
        //   serializeJson(doc, out);
        //   request->send(200, "application/json", out);
        // });

        // server.on("/get_runtime_settings", HTTP_GET, [](AsyncWebServerRequest* request) {
        //   DynamicJsonDocument doc(1024);

        //   if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(100))) {
        //     doc["modbus-enabled"] = app_settings.modbus_enabled;
        //     doc["interval"] = app_settings.interval_modbus;
        //     doc["brightness"] = app_settings.brightness;

        //     JsonObject google = doc.createNestedObject("google");
        //     google["flag-google"] = app_settings.google_enabled;
        //     google["wifi-ssid"] = preferences.getString("ssid_wifi", "");
        //     google["wifi-pass"] = preferences.getString("password_wifi", "");
        //     google["url-gs"] = preferences.getString("url_google", "");
        //     google["secret-key"] = preferences.getString("secret_key", "");

        //     JsonObject excel = doc.createNestedObject("excel");
        //     excel["flag-excel"] = app_settings.excel_enabled;

        //     JsonObject fs = doc.createNestedObject("file-system");
        //     fs["flag-fs"] = app_settings.fs_enabled;
        //     fs["file-name"] = preferences.getString("fs_file_name", "log.txt");
        //     fs["format"] = preferences.getString("fs_format", "txt");

        //     JsonObject sd = doc.createNestedObject("sd-card");
        //     sd["flag-sd"] = preferences.getBool("sd_en", false);
        //     sd["file-name"] = preferences.getString("sd_file_name", "sd_log.txt");
        //     sd["format"] = preferences.getString("sd_format", "txt");

        //     xSemaphoreGive(settings_mutex);
        //   }

        //   String out;
        //   serializeJson(doc, out);
        //   request->send(200, "application/json", out);
        // });

        // -> new block 3
          server.on("/get_runtime_settings", HTTP_GET, [](AsyncWebServerRequest* request) {
            DynamicJsonDocument doc(1024);

            if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(100))) {
              doc["modbus-enabled"] = app_settings.modbus_enabled;
              doc["interval"] = app_settings.interval_modbus;
              doc["brightness"] = app_settings.brightness;

              JsonObject google = doc.createNestedObject("google");
              google["flag-google"] = app_settings.google_enabled;
              google["wifi-ssid"] = preferences.getString("ssid_wifi", "");
              google["wifi-pass"] = preferences.getString("password_wifi", "");
              google["url-gs"] = preferences.getString("url_google", "");
              google["secret-key"] = preferences.getString("secret_key", "");

              JsonObject excel = doc.createNestedObject("excel");
              excel["flag-excel"] = app_settings.excel_enabled;

              JsonObject fs = doc.createNestedObject("file-system");
              fs["flag-fs"] = app_settings.fs_enabled;
              fs["file-name"] = app_settings.fs_file_name;
              fs["format"] = app_settings.fs_format;

              JsonObject sd = doc.createNestedObject("sd-card");
              sd["flag-sd"] = app_settings.sd_enabled;
              sd["file-name"] = app_settings.sd_file_name;
              sd["format"] = app_settings.sd_format;

              xSemaphoreGive(settings_mutex);
            }
            String out;
            serializeJson(doc, out);
            request->send(200, "application/json", out);
          });
        // -> new block 3
      // -> new block 2

      server.begin();
    }

    bool connect_wifi_flag() {
      String ssid_wifi = preferences.getString("ssid_wifi", "");
      String password_wifi = preferences.getString("password_wifi", "");
      if (ssid_wifi.isEmpty()) {
        return false;
      }

      WiFi.begin(ssid_wifi.c_str(), password_wifi.c_str());
      uint32_t start_time = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start_time < 15000) {
        delay(500);
      }
      if (WiFi.status() == WL_CONNECTED) {
        return true;
      }

      return false;
    }

    bool ensure_wifi_connected() {
      if (WiFi.status() == WL_CONNECTED) {
        return true;
      }
      WiFi.disconnect();
      return connect_wifi_flag();
    }
  // -> server

  // -> fs
    String build_file_path(const String& file_name, const String& file_format) { // -> сборка пути
      String name = file_name;
      String format = file_format;
      name.trim();
      format.trim();

      if (name.isEmpty()) {
        name = "log";
      }
      if (format.isEmpty()) {
        format = "txt";
      }
      if (!format.startsWith(".")) {
        format = "." + format;
      }
      if (!name.startsWith("/")) {
        name = "/" + name;
      }
      return name + format;
    }

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
    bool post_to_google(float temp, float hum, const char* info="") {
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
  // -> google

  // -> data streamer
    void print_data_streamer(float humidity, float temperature) {
      Serial.print("Humidity: ");
      Serial.print(",");
      Serial.print(humidity);
      Serial.print("Temperature: ");
      Serial.print(",");
      Serial.println(temperature);
    }
  // -> data streamer

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

    // void start_timer_modbus() {
    //   int data_interval = preferences.getInt("interval_modbus", 2);
    //   timer_modbus = timerBegin(1000000);
    //   timerAttachInterrupt(timer_modbus, &change_flag_modbus);
    //   timerAlarm(timer_modbus, (data_interval * 1000000), true, 0);
    //   timerStart(timer_modbus);
    // }

    // -> new block 2
      void start_timer_modbus() {
        int data_interval = 2;

        if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(100))) {
          data_interval = app_settings.interval_modbus;
          xSemaphoreGive(settings_mutex);
        }

        timer_modbus = timerBegin(1000000);
        timerAttachInterrupt(timer_modbus, &change_flag_modbus);
        timerAlarm(timer_modbus, (data_interval * 1000000), true, 0);
        timerStart(timer_modbus);
      }
    // -> new block 2

    void initial_modbus() {
      Serial1.begin(4800, SERIAL_8N1, PIN_RX, PIN_TX);
      node.begin(1, Serial1);
    }
  // -> modbus

  // void esp_work_function(void* parameter) {
  //   WiFi.mode(WIFI_AP_STA);
  //   WiFi.softAP(ssid, password);
  //   WiFi.softAPConfig(ip, geteway, subnet);

  //   if (!LittleFS.begin(true)) {
  //     Serial.println("LittleFS mount failed");
  //   } else {
  //     Serial.println("LittleFS mounted successfully");
  //   }

  //   start_server();
    
  //   Serial.println("LOGGER ESP_TASK: Task is start.");

  //   start_timer_modbus(); // -> запуск таймера
  //   initial_modbus();

  //   bool flag_google_p = preferences.getBool("flag_google", false);
  //   bool flag_fs_p = preferences.getBool("flag_file_system", false);
  //   bool flag_excel_p = preferences.getBool("flag_excel", false);

  //   for (; ;) {
  //     if (flag_modbus) {
  //       flag_modbus = false;
        
  //       uint8_t result_operation;
  //       uint16_t data_modbus[2];

  //       result_operation = node.readInputRegisters(0x0000, 2);
  //       if (result_operation == node.ku8MBSuccess) {
  //         data_modbus[0] = node.getResponseBuffer(0x00);
  //         data_modbus[1] = node.getResponseBuffer(0x01);

  //         if (xSemaphoreTake(esp_work, pdMS_TO_TICKS(100))) {
  //           humidity = data_modbus[0] / 10.0;
  //           temperature = data_modbus[1] / 10.0;

  //           xSemaphoreGive(esp_work);
  //         }

  //         String sensor_data;
  //         String time_date_data;

  //         sensor_data = "Humidity: " + String(humidity) + " " + "Temperature: " + String(temperature) + "\n";
  //         time_date_data = "Date: " + String(rtc.getDate()) + " " + "Time: " + String(rtc.getTime()) + "\n\n";

  //         if (flag_fs_p) {
  //           write_file(LittleFS, "/log.txt", sensor_data);
  //           write_file(LittleFS, "/log.txt", time_date_data);
  //         }

  //         if (flag_google_p) {
  //           post_to_google(temperature, humidity, "sensor_get");
  //         }

  //         if (flag_excel_p) {
  //           print_data_streamer(humidity, temperature);
  //         }
  //       } else {
  //         Serial.println(result_operation);
  //       }
  //     }

  //     vTaskDelay(pdMS_TO_TICKS(100));
  //   }
  // }

  #define SD_CS_PIN 4 // -> пин для sd карты 

  // -> new block 2
    void esp_work_function(void* parameter) {
        WiFi.mode(WIFI_AP_STA);

        WiFi.softAP(ssid, password);
        WiFi.softAPConfig(ip, geteway, subnet);

        bool wifi_connected = connect_wifi_flag();
        if (wifi_connected) {
          get_data_ntp(url_ntp, gmt_offset, day_light_offset);
        }

        if (!LittleFS.begin(true)) {
          Serial.println("LittleFS mount failed");
        } else {
          Serial.println("LittleFS mounted successfully");
        }

        if (!SD.begin(SD_CS_PIN)) {
          Serial.println("SD mount failed");
        } else {
          Serial.println("SD mounted successfully");
        }

        start_server();

        Serial.println("LOGGER ESP_TASK: Task is start.");

        start_timer_modbus();
        initial_modbus();

        for (;;) {
          if (modbus_time_need_restart) {
            modbus_time_need_restart = false;

          if (timer_modbus) {
            timerEnd(timer_modbus);
            timer_modbus = NULL;
          }

          start_timer_modbus();
        }

        bool modbus_en = false;
        bool fs_en = false;
        bool excel_en = false;
        bool google_en = false;
        bool sd_en = false;

        String fs_path = "/log.txt";
        String sd_path = "/sd_log.txt";

        if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(100))) {
          modbus_en = app_settings.modbus_enabled;
          fs_en = app_settings.fs_enabled;
          excel_en = app_settings.excel_enabled;
          google_en = app_settings.google_enabled;
          sd_en = app_settings.sd_enabled;
          fs_path = build_file_path(app_settings.fs_file_name, app_settings.fs_format);
          sd_path = build_file_path(app_settings.sd_file_name, app_settings.sd_format);
          xSemaphoreGive(settings_mutex);
        }

        if (flag_modbus && modbus_en) {
          flag_modbus = false;

          uint8_t result_operation;
          uint16_t data_modbus[2];

          result_operation = node.readInputRegisters(0x0000, 2);
          if (result_operation == node.ku8MBSuccess) {
            data_modbus[0] = node.getResponseBuffer(0x00);
            data_modbus[1] = node.getResponseBuffer(0x01);

            // if (xSemaphoreTake(esp_work, pdMS_TO_TICKS(100))) {
            //   humidity = data_modbus[0] / 10.0;
            //   temperature = data_modbus[1] / 10.0;
            //   xSemaphoreGive(esp_work);
            // }

            if (xSemaphoreTake(esp_work, pdMS_TO_TICKS(100))) {
              humidity = data_modbus[0] / 10.0;
              temperature = data_modbus[1] / 10.0;

              last_temp_data = temperature;
              last_hum_data = humidity;
              new_chart_data_ready = true;

              xSemaphoreGive(esp_work);
            }

            String sensor_data = "Humidity: " + String(humidity) + " Temperature: " + String(temperature) + "\n";
            String time_date_data = "Date: " + String(rtc.getDate()) + " Time: " + String(rtc.getTime()) + "\n\n";

            if (fs_en) {
              write_file(LittleFS, fs_path, sensor_data);
              write_file(LittleFS, fs_path, time_date_data);
            }

            if (google_en) {
              if (ensure_wifi_connected()) {
                post_to_google(temperature, humidity, "sensor_get");
              }
            }

            if (excel_en) {
              print_data_streamer(humidity, temperature);
            }

            if (sd_en) {
              write_file(SD, sd_path, sensor_data);
              write_file(SD, sd_path, time_date_data);
            }
          } else {
            Serial.println(result_operation);
          }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
      } 
    }
  // -> new block 2
// -> esp_work

void setup() {
  Serial.begin(115200);
  delay(1000);

  esp_work = xSemaphoreCreateMutex();
  time_mutex = xSemaphoreCreateMutex();
  settings_mutex = xSemaphoreCreateMutex();

  preferences.begin("interval", false);
  load_settings();
  // apply_brightness(app_settings.brightness);

  // -> disp_setup
    if (!gfx -> begin()) {
      return;
    }
    gfx -> fillScreen(BLACK);
    ledcAttach(EXAMPLE_PIN_NUM_LCD_BL , LEDC_FREQ, LEDC_TIMER_10_BIT);
    apply_brightness(app_settings.brightness);
    // ledcWrite(EXAMPLE_PIN_NUM_LCD_BL , (1 << LEDC_TIMER_10_BIT) / 100 * 80);
    screen_width = gfx -> width();
    screen_height = gfx -> height();
    buf_size = screen_width * 40;

    lv_init();
    bsp_button_init();
    lv_disp_drv_init(&disp_drv);

    disp_draw_buf = (lv_color_t*) heap_caps_malloc(buf_size * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!disp_draw_buf) {
      disp_draw_buf = (lv_color_t*) heap_caps_malloc(buf_size * 2, MALLOC_CAP_8BIT);
    } 
    if (disp_draw_buf) {
      lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, buf_size);
      disp_drv.hor_res = screen_width;
      disp_drv.ver_res = screen_height;
      disp_drv.flush_cb = my_disp_flush;
      disp_drv.draw_buf = &draw_buf;
      disp_drv.direct_mode = false;
      lv_disp_drv_register(&disp_drv);
    } else {
      Serial.println("LVGL buffer allocation failed");
      return;
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

  process_button_event();
  if (settings_ui_dirty) {
    settings_ui_dirty = false;

    if (mode_esp == MODE_VIEW_SETTINGS) {
      if (current_screen_settings == SCREEN_MODE_LOG_ESP32) {
        update_log_menu_ui();
      } else if (current_screen_settings == SCREEN_BRIGHTNES) {
        if (xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(50))) {
          apply_brightness(app_settings.brightness);
          xSemaphoreGive(settings_mutex);
        }
      }
    }
  }

  // if (mode_esp == MODE_VIEW_SENSOR_AND_TIME) {
  //   if (screen_sensor_time_date == SCREEN_SENSOR_VALUE) {
  //     update_sensor_display();
  //   } else if (screen_sensor_time_date == SCREEN_DATE_TIME_VALUE) {
  //     update_time_display();
  //   }
  // } 

  if (mode_esp == MODE_VIEW_SENSOR_AND_TIME) {
    if (screen_sensor_time_date == SCREEN_SENSOR_VALUE) {
      update_sensor_display();
    } else if (screen_sensor_time_date == SCREEN_DATE_TIME_VALUE) {
      update_time_display();
    } else if (screen_sensor_time_date == SCREEN_CHART_SENSOR_VALUE) {
      if (new_chart_data_ready) {
        float temp_copy = 0.0f;
        float hum_copy = 0.0f;

        if (xSemaphoreTake(esp_work, pdMS_TO_TICKS(50))) {
          temp_copy = last_temp_data;
          hum_copy = last_hum_data;
          new_chart_data_ready = false;
          xSemaphoreGive(esp_work);
        }
        update_chart(temp_copy, hum_copy);
      }
    }
  }
  delay(10);
}
