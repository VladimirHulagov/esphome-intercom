#pragma once
#include "esphome.h"

class I2CBusRecovery : public esphome::Component {
 public:
  float get_setup_priority() const override { return 2000.0f; }

  void setup() override {
    ESP_LOGI("i2c_recovery", "Early I2C bus recovery on GPIO39/GPIO40");

    gpio_num_t sda = GPIO_NUM_39;
    gpio_num_t scl = GPIO_NUM_40;

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT_OD;
    io_conf.pin_bit_mask = (1ULL << sda) | (1ULL << scl);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    delayMicroseconds(50);

    int sda_state = gpio_get_level(sda);
    int scl_state = gpio_get_level(scl);
    ESP_LOGI("i2c_recovery", "Initial state: SDA=%d SCL=%d", sda_state, scl_state);

    if (sda_state && scl_state) {
      ESP_LOGI("i2c_recovery", "Bus is clean, no recovery needed");
      gpio_reset_pin(sda);
      gpio_reset_pin(scl);
      return;
    }

    if (!scl_state) {
      ESP_LOGW("i2c_recovery", "SCL stuck LOW - clocking to recover");
      for (int i = 0; i < 128; i++) {
        gpio_set_level(scl, 0);
        delayMicroseconds(5);
        gpio_set_level(scl, 1);
        delayMicroseconds(5);
        if (gpio_get_level(scl)) {
          ESP_LOGI("i2c_recovery", "SCL released after %d clocks", i + 1);
          break;
        }
      }
    }

    if (!gpio_get_level(sda)) {
      ESP_LOGW("i2c_recovery", "SDA stuck LOW - clocking SCL to recover");
      for (int i = 0; i < 128; i++) {
        gpio_set_level(scl, 0);
        delayMicroseconds(5);
        gpio_set_level(scl, 1);
        delayMicroseconds(5);
        if (gpio_get_level(sda)) {
          ESP_LOGI("i2c_recovery", "SDA released after %d SCL clocks", i + 1);
          break;
        }
      }
    }

    gpio_set_level(sda, 0);
    delayMicroseconds(5);
    gpio_set_level(scl, 0);
    delayMicroseconds(5);
    gpio_set_level(scl, 1);
    delayMicroseconds(5);
    gpio_set_level(sda, 1);
    delayMicroseconds(10);

    ESP_LOGI("i2c_recovery", "Final state: SDA=%d SCL=%d",
             gpio_get_level(sda), gpio_get_level(scl));

    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
  }
};
