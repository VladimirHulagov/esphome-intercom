#pragma once
#include "esphome.h"
#include <driver/gpio.h>

class I2CDiagnostic : public esphome::Component {
 public:
  float get_setup_priority() const override { return esphome::setup_priority::HARDWARE + 1; }

  void setup() override {
    ESP_LOGI("i2c_diag", "=== RAW I2C DIAGNOSTIC ===");

    test_gpio_pair(47, 48, "GPIO47/48 (community code)");
    test_gpio_pair(39, 40, "GPIO39/40 (AGENTS.md original)");
    test_gpio_pair(41, 42, "GPIO41/42");
    test_gpio_pair(38, 45, "GPIO38/45");
    test_gpio_pair(8, 9, "GPIO8/9");
    test_gpio_pair(18, 17, "GPIO18/17");
    test_gpio_pair(10, 11, "GPIO10/11");

    ESP_LOGI("i2c_diag", "=== DIAGNOSTIC COMPLETE ===");
  }

 private:
  void test_gpio_pair(int sda_num, int scl_num, const char* label) {
    ESP_LOGI("i2c_diag", "--- Testing %s (SDA=%d, SCL=%d) ---", label, sda_num, scl_num);

    gpio_num_t sda = (gpio_num_t)sda_num;
    gpio_num_t scl = (gpio_num_t)scl_num;

    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
    esphome::delay(1);

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT_OD;
    io_conf.pin_bit_mask = (1ULL << sda_num) | (1ULL << scl_num);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

    esphome::delay(1);

    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    esphome::delay(1);
    int sda_h = gpio_get_level(sda);
    int scl_h = gpio_get_level(scl);
    ESP_LOGI("i2c_diag", "  Pins released: SDA=%d SCL=%d (expect 1/1)", sda_h, scl_h);

    gpio_set_level(sda, 0);
    esphome::delay(1);
    int sda_l = gpio_get_level(sda);
    gpio_set_level(sda, 1);
    esphome::delay(1);
    ESP_LOGI("i2c_diag", "  SDA driven low: read=%d (expect 0)", sda_l);

    gpio_set_level(scl, 0);
    esphome::delay(1);
    int scl_l = gpio_get_level(scl);
    gpio_set_level(scl, 1);
    esphome::delay(1);
    ESP_LOGI("i2c_diag", "  SCL driven low: read=%d (expect 0)", scl_l);

    if (sda_h == 1 && scl_h == 1 && sda_l == 0 && scl_l == 0) {
      int found = 0;
      for (int addr = 0x08; addr < 0x78; addr++) {
        if (bitbang_probe(sda, scl, addr)) {
          ESP_LOGI("i2c_diag", "  *** FOUND device at 0x%02X ***", addr);
          found++;
        }
      }
      if (found == 0) {
        ESP_LOGI("i2c_diag", "  No I2C devices found");
      }
    } else {
      ESP_LOGW("i2c_diag", "  Skipping scan - pins not usable (SDA: hi=%d lo=%d, SCL: hi=%d lo=%d)",
               sda_h, sda_l, scl_h, scl_l);
    }

    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
  }

  bool bitbang_probe(gpio_num_t sda, gpio_num_t scl, uint8_t addr) {
    bool ack = false;

    gpio_set_level(scl, 1);
    gpio_set_level(sda, 1);
    esphome::delayMicroseconds(5);

    gpio_set_level(sda, 0);
    esphome::delayMicroseconds(5);
    gpio_set_level(scl, 0);
    esphome::delayMicroseconds(5);

    for (int i = 7; i >= 0; i--) {
      if ((addr >> i) & 1)
        gpio_set_level(sda, 1);
      else
        gpio_set_level(sda, 0);
      esphome::delayMicroseconds(5);
      gpio_set_level(scl, 1);
      esphome::delayMicroseconds(5);
      gpio_set_level(scl, 0);
      esphome::delayMicroseconds(5);
    }

    gpio_set_level(sda, 1);
    esphome::delayMicroseconds(5);
    gpio_set_level(scl, 1);
    esphome::delayMicroseconds(5);
    ack = (gpio_get_level(sda) == 0);

    gpio_set_level(scl, 0);
    esphome::delayMicroseconds(5);

    gpio_set_level(sda, 0);
    esphome::delayMicroseconds(5);
    gpio_set_level(scl, 1);
    esphome::delayMicroseconds(5);
    gpio_set_level(sda, 1);
    esphome::delayMicroseconds(5);

    return ack;
  }
};
