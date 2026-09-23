// Quake 2 sound DMA driver for the ESP32-P4 ES8311 codec
//
// Author: Alejandro Villegas Alonso
//         https://www.linkedin.com/in/alejandro-villegas-alonso-825041b1
//
// The engine mixes SFX directly into a circular DMA buffer during
// S_PaintChannels(). This driver just cycles that buffer through the
// I2S codec, exposing the current play position via SNDDMA_GetDMAPos().

#include "bsp/esp-bsp.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "qcommon/qcommon.h"
#include "client/snd_loc.h"
#include "debug.h"

#define DEFAULT_VOLUME 70

static esp_codec_dev_handle_t spk_codec_dev;
int snd_inited;

// note: keep >64 and power of 2
#define BUFFER_SIZE	(32*1024)

unsigned char dma_buffer[BUFFER_SIZE];

int dma_rpos;

#define CHUNKSZ (BUFFER_SIZE/16)

static void audio_task(void *param)
{
	Q2P4_DEBUG("audio task running\n");
	int16_t *out = malloc(CHUNKSZ);
	int old_volume = -1;

	while (1)
	{
		// map the Q2 s_volume cvar (0..1) to codec volume 0..100
		if (s_volume && s_volume->value != old_volume)
		{
			int v = (int)(s_volume->value * 100.0);
			esp_codec_dev_set_out_vol(spk_codec_dev, v);
			old_volume = (int)s_volume->value;
		}

		memcpy(out, &dma_buffer[dma_rpos], CHUNKSZ);
		dma_rpos = (dma_rpos + CHUNKSZ) % BUFFER_SIZE;
		esp_codec_dev_write(spk_codec_dev, out, CHUNKSZ);
	}
	free(out);
}

qboolean SNDDMA_Init(void)
{
	int err;

	bsp_i2c_init();
	bsp_audio_init(NULL);
	spk_codec_dev = bsp_audio_codec_speaker_init();

	err = esp_codec_dev_set_out_vol(spk_codec_dev, DEFAULT_VOLUME);
	err |= esp_codec_dev_set_out_mute(spk_codec_dev, 0);

	esp_codec_dev_sample_info_t fs = {
		.sample_rate = 22050,
		.channel = 2,
		.bits_per_sample = 16,
	};
	err |= esp_codec_dev_open(spk_codec_dev, &fs);
	if (err)
	{
		Com_Printf("SNDDMA_Init: codec open failed\n");
		return false;
	}

	dma.samplebits = 16;
	dma.speed = 22050;
	dma.channels = 2;
	dma.samples = sizeof(dma_buffer) / (dma.samplebits / 8); // shorts
	dma.samplepos = 0;
	dma.submission_chunk = 1;
	dma.buffer = dma_buffer;

	snd_inited = 1;
	Com_Printf("SNDDMA_Init: 22050 Hz 16-bit stereo\n");
	xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 7, NULL, 1);
	return true;
}

int SNDDMA_GetDMAPos(void)
{
	if (!snd_inited) return 0;
	dma.samplepos = dma_rpos / (dma.samplebits / 8);
	return dma.samplepos;
}

void SNDDMA_Shutdown(void)
{
	if (snd_inited)
	{
		esp_codec_dev_close(spk_codec_dev);
		snd_inited = 0;
	}
}

void SNDDMA_BeginPainting(void)
{
}

void SNDDMA_Submit(void)
{
}