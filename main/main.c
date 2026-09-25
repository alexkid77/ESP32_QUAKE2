// Copyright 2024 Espressif Systems (Shanghai) PTE LTD
//
// Author: Alejandro Villegas Alonso
//         https://www.linkedin.com/in/alejandro-villegas-alonso-825041b1
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "bsp/esp-bsp.h"
#include "debug.h"
#include "display.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "eth_connect.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "input.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quake2.h"

extern void HandleInput(void);

int QG_Milliseconds(void) { return (int)(esp_timer_get_time() / 1000); }




#define QUAKE2_ESP32P4_FPS 1

#if QUAKE2_ESP32P4_FPS
#define Q2P4_FPS_ARG "+set", "cg_drawfps", "1"
#else
#define Q2P4_FPS_ARG "+set", "cg_drawfps", "0"
#endif



static void quake_task(void *param) {
  char *argv[] = {
      "quake2", "+set", "basedir", "/sdcard/", "+set", "freelook", "1",
      // Mouse feel: ~3x the default sensitivity/axis gains, since the
      // 320x240 software renderer + ~25fps makes the stock feel sluggish.
      "+set", "sensitivity", "6", "+set", "m_yaw", "0.033", "+set", "m_pitch",
      "0.033",
      Q2P4_FPS_ARG, NULL};

  // initialize Quake 2
  Quake2_Init(19, argv);

  int64_t oldtime_ms = Quake2_Milliseconds();

  while (1) {
    // Run the frame at the correct duration.
    HandleInput();
    int newtime_ms;
    int time;
    do {
      newtime_ms = Quake2_Milliseconds();
      time = newtime_ms - (int)oldtime_ms;
    } while (time < 1);
    oldtime_ms = newtime_ms;
    Quake2_Frame(time);
  }
}

static esp_err_t sdcard_mount_with_more_files(void) {
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 32,
      .allocation_unit_size = 64 * 1024,
  };

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

  sd_pwr_ctrl_ldo_config_t ldo_config = {
      .ldo_chan_id = 4,
  };
  sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
  esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
  if (ret != ESP_OK) {
    ESP_LOGE("main", "Failed to create a new on-chip LDO power control driver");
    return ret;
  }
  host.pwr_ctrl_handle = pwr_ctrl_handle;

  const sdmmc_slot_config_t slot_config = {
      .cd = SDMMC_SLOT_NO_CD,
      .wp = SDMMC_SLOT_NO_WP,
      .width = 4,
      .flags = 0,
  };

  return esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config,
                                 NULL);
}

void app_main(void) {
  size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

  Q2P4_DEBUG("PSRAM libre total: %.2f MB\n", free_spiram / (1024.0 * 1024.0));
  Q2P4_DEBUG("Bloque continuo más grande: %.2f MB\n",
             largest_block / (1024.0 * 1024.0));
  esp_err_t er = sdcard_mount_with_more_files();
  ESP_LOGI("main", "sdcard_mount() = %d", er);
  FILE *pak = fopen("/sdcard/baseq2/pak0.pak", "rb");
  if (pak) {
    ESP_LOGI("main", "pak0.pak present");
    int c = fgetc(pak);
    ESP_LOGI("main", "first byte: %02x", c);
    fclose(pak);
  } else {
    ESP_LOGW("main", "pak0.pak NOT found via fopen");
  }
  display_init();

  input_init();

  ethernet_connect();

  int stack_depth = 256 * 1024;

  StaticTask_t *taskbuf = heap_caps_malloc(
      sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  assert(taskbuf);
  memset(taskbuf, 0, sizeof(StaticTask_t));
  uint8_t *stackbuf = calloc(stack_depth, 1);
  xTaskCreateStaticPinnedToCore(quake_task, "quake", stack_depth, NULL, 2,
                                (StackType_t *)stackbuf, taskbuf, 0);
}
