// ESP32-P4 display pipeline for Quake 2 (software renderer)
//
// Author: Alejandro Villegas Alonso
//         https://www.linkedin.com/in/alejandro-villegas-alonso-825041b1
//
// 320x240 8bpp paletted framebuffer -> RGB565 transfer -> PPA scale
// to 1024x600 -> MIPI-DSI panel.

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_panel_ops.h"
#include "soc/mipi_dsi_bridge_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/ppa.h"

#include "display.h"
#include "debug.h"

static const char *TAG = "DISPLAY";

esp_lcd_panel_handle_t panel_handle = NULL;
esp_lcd_panel_io_handle_t io_handle = NULL;

static uint16_t pal[256];
static uint8_t *cur_pixels;
static uint16_t *lcdbuf[2] = {};
static int cur_buf = 1;
static int draw_task_quit = 0;

static TaskHandle_t draw_task_handle;
static SemaphoreHandle_t drawing_mux;

static int fps_ticks = 0;
static int64_t start_time_fps_meas;

// Per-frame pipeline timing (Paso 0 baseline instrumentation).
static int64_t accum_conv_us = 0;
static int64_t accum_ppa_us = 0;
static int64_t accum_draw_us = 0;
static int frame_count_stats = 0;

// Convert an 8bpp paletted frame to RGB565. The palette map cannot use a
// memcpy-based SIMD routine (each index becomes a looked-up 16-bit value), so
// the fast path uses the widest scalar loads/stores: one 32-bit input word
// carries 4 palette indices that map to one 64-bit output word (4 RGB565 px).
static void convert_pixels(uint16_t *dst, const uint8_t *src, int pixels)
{
	const uint32_t *in = (const uint32_t *)src;
	uint64_t *out = (uint64_t *)dst;
	int n = pixels / 4;

	for (int i = 0; i < n; i++) {
		uint32_t w = *in++;
		uint64_t d = (uint64_t)pal[w & 0xff]
			  | ((uint64_t)pal[(w >> 8) & 0xff] << 16)
			  | ((uint64_t)pal[(w >> 16) & 0xff] << 32)
			  | ((uint64_t)pal[(w >> 24) & 0xff] << 48);
		*out++ = d;
	}
}

void QG_DrawFrame(void *pixels)
{
	xSemaphoreTake(drawing_mux, portMAX_DELAY);
	cur_pixels = pixels;
	xSemaphoreGive(drawing_mux);
	xTaskNotifyGive(draw_task_handle);
	fps_ticks++;
	if (fps_ticks > 100)
	{
		int64_t newtime_us = esp_timer_get_time();
		int64_t fpstime = (newtime_us - start_time_fps_meas) / fps_ticks;
		fps_ticks = 0;
		start_time_fps_meas = newtime_us;
		Q2P4_DEBUG("Fps: %02f\n", 1000000.0 / fpstime);
	}
}

static void draw_task(void *param)
{
	ppa_client_config_t ppa_cfg = {
		.oper_type = PPA_OPERATION_SRM,
	};
	ppa_client_handle_t ppa;
	ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &ppa));

	uint16_t *rgbfb = heap_caps_calloc(QUAKE2_RES_X * QUAKE2_RES_Y,
									   sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
	assert(rgbfb);
	ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel_handle, 2,
														(void **)&lcdbuf[0], (void **)&lcdbuf[1]));

	while (!draw_task_quit)
	{
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

		int64_t t0 = esp_timer_get_time();

		xSemaphoreTake(drawing_mux, portMAX_DELAY);
		// convert 8bpp paletted pixels to RGB565 (wide loads/stores)
		convert_pixels(rgbfb, (uint8_t *)cur_pixels,
					   QUAKE2_RES_X * QUAKE2_RES_Y);

		int64_t t1 = esp_timer_get_time();

		// use PPA to scale image
		ppa_srm_oper_config_t op = {
			.in = {
				.buffer = rgbfb,
				.pic_w = QUAKE2_RES_X,
				.pic_h = QUAKE2_RES_Y,
				.block_w = QUAKE2_RES_X,
				.block_h = QUAKE2_RES_Y,
				.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
			},
			.out = {
				.buffer = lcdbuf[cur_buf],
				.buffer_size = BSP_LCD_V_RES * BSP_LCD_H_RES * sizeof(uint16_t),
				.pic_w = BSP_LCD_H_RES,
				.pic_h = BSP_LCD_V_RES,
				.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
			},
			.scale_x = (float)BSP_LCD_H_RES / (float)QUAKE2_RES_X,
			.scale_y = (float)BSP_LCD_V_RES / (float)QUAKE2_RES_Y,
			.mode = PPA_TRANS_MODE_BLOCKING,
		};
		ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(ppa, &op));

		int64_t t2 = esp_timer_get_time();

		xSemaphoreGive(drawing_mux);
		// do a draw to trigger fb flip
		esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES,
								  lcdbuf[cur_buf]);
		cur_buf = cur_buf ? 0 : 1;

		int64_t t3 = esp_timer_get_time();
		accum_conv_us += t1 - t0;
		accum_ppa_us += t2 - t1;
		accum_draw_us += t3 - t2;
		if (++frame_count_stats >= 100)
		{
			Q2P4_DEBUG("Draw: conv %llu us/frm, ppa %llu us/frm, draw %llu us/frm\n",
				   (unsigned long long)(accum_conv_us / frame_count_stats),
				   (unsigned long long)(accum_ppa_us / frame_count_stats),
				   (unsigned long long)(accum_draw_us / frame_count_stats));
			accum_conv_us = 0;
			accum_ppa_us = 0;
			accum_draw_us = 0;
			frame_count_stats = 0;
		}
	}
}

void QG_SetPalette(unsigned char palette[768])
{
	unsigned char *p = palette;
	for (int i = 0; i < 256; i++)
	{
		// convert to rgb565
		int b = (*p++) >> 3;
		int g = (*p++) >> 2;
		int r = (*p++) >> 3;
		pal[i] = r + (g << 5) + (b << 11);
	}
}

void display_quit(void)
{
	draw_task_quit = 1;
	xSemaphoreGive(drawing_mux);
	vTaskDelay(pdMS_TO_TICKS(30) + 1);
	// blank the panel
	memset(lcdbuf[cur_buf], 0, BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t));
	esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES,
							  lcdbuf[cur_buf]);
}

void display_init(void)
{
	// initialize LCD
	const bsp_display_config_t config = {
		.hdmi_resolution = BSP_HDMI_RES_NONE,
		.dsi_bus = {
			.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
			.lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
		},
	};

	bsp_display_new(&config, &panel_handle, &io_handle);
	esp_lcd_panel_disp_on_off(panel_handle, true);
	bsp_display_brightness_init();
	bsp_display_brightness_set(100);

	drawing_mux = xSemaphoreCreateMutex();
	xTaskCreatePinnedToCore(draw_task, "draw", 4096, NULL, 3, &draw_task_handle, 1);

	ESP_LOGW(TAG, "heap internal(8bit): free=%lu largest=%lu | DMA(8bit): free=%lu largest=%lu",
			 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
			 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
			 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
			 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
}