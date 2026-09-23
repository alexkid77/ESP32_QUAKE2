// Copyright 2024 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "errno.h"
#include "hid_ev.h"

#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"


static const char *TAG = "example";

QueueHandle_t app_event_queue = NULL;

QueueHandle_t hidev_event_queue = NULL;

static void hid_mouse_parse_boot(const uint8_t *data, size_t len);

/**
 * @brief Per-device state handed to hid_host_interface_callback via callback_arg.
 *
 * dev        - generic report-descriptor parser (used for report protocol)
 * boot_mouse - true when the device was switched to HID boot protocol, so its
 *              reports have the fixed 3/4-byte boot layout parsed directly.
 */
typedef struct {
	hidev_device_t *dev;
	bool boot_mouse;
} usb_hid_device_t;

/**
 * @brief APP event group
 *
 * Application logic can be different. There is a one among other ways to distingiush the
 * event by application event group.
 * In this example we have two event groups:
 * APP_EVENT			- General event, which is APP_QUIT_PIN press event (Generally, it is IO0).
 * APP_EVENT_HID_HOST	- HID Host Driver event, such as device connection/disconnection or input report.
 */
typedef enum {
	APP_EVENT = 0,
	APP_EVENT_HID_HOST
} app_event_group_t;

/**
 * @brief APP event queue
 *
 * This event is used for delivering the HID Host event from callback to a task.
 */
typedef struct {
	app_event_group_t event_group;
	/* HID Host - Device related info */
	struct {
		hid_host_device_handle_t handle;
		hid_host_driver_event_t event;
		void *arg;
	} hid_host_device;
} app_event_queue_t;

/**
 * @brief HID Protocol string names
 */
static const char *hid_proto_name_str[] = {
	"NONE",
	"KEYBOARD",
	"MOUSE"
};

/**
 * @brief USB HID Host interface callback
 *
 * @param[in] hid_device_handle	 HID Device handle
 * @param[in] event				 HID Host interface event
 * @param[in] arg				 Pointer to arguments, does not used
 */
void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
								 const hid_host_interface_event_t event,
								 void *arg)
{
	uint8_t data[64] = { 0 };
	size_t data_length = 0;
	hid_host_dev_params_t dev_params;
	usb_hid_device_t *hid_dev=(usb_hid_device_t*)arg;
	ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

	switch (event) {
	case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
		ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle,
																  data,
																  64,
																  &data_length));
		if (hid_dev && hid_dev->boot_mouse) {
			//boot protocol reports are parsed directly by hid_mouse_parse_boot
			hid_mouse_parse_boot(data, data_length);
		} else if (hid_dev && hid_dev->dev) {
			//device is left in report protocol; parse via its report descriptor
			hidev_parse_report(hid_dev->dev, data, 0);
		} else if (dev_params.proto == HID_PROTOCOL_MOUSE) {
			//last resort: descriptor not ready yet, try the boot layout
			hid_mouse_parse_boot(data, data_length);
		}
		break;
	case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
		ESP_LOGI(TAG, "HID Device, protocol '%s' DISCONNECTED",
				 hid_proto_name_str[dev_params.proto]);
		ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
		break;
	case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
		ESP_LOGI(TAG, "HID Device, protocol '%s' TRANSFER_ERROR",
				 hid_proto_name_str[dev_params.proto]);
		break;
	default:
		ESP_LOGE(TAG, "HID Device, protocol '%s' Unhandled event",
				 hid_proto_name_str[dev_params.proto]);
		break;
	}
}

static void hidev_cb(hid_ev_t *event) {
	if (!hidev_event_queue)
		return;
	//Never block the HID host callback: if the queue is full, drop the oldest
	//event to make room. This keeps the most recent input and guarantees the
	//HID host keeps running (no 1 s stalls on a full queue).
	if (xQueueSend(hidev_event_queue, event, 0) != pdTRUE) {
		hid_ev_t discard;
		xQueueReceive(hidev_event_queue, &discard, 0);
		xQueueSend(hidev_event_queue, event, 0);
	}
}

/**
 * @brief Parse a HID mouse boot protocol report into hid_ev_t events.
 *
 * The boot report is: buttons + relative X + relative Y (and, on most
 * mice, an additional signed wheel byte at offset 3).
 */
static void hid_mouse_parse_boot(const uint8_t *data, size_t len)
{
	static uint8_t prev_buttons = 0;

	if (len < sizeof(hid_mouse_input_report_boot_t))
		return;

	const hid_mouse_input_report_boot_t *ms =
		(const hid_mouse_input_report_boot_t *)data;

	if (ms->x_displacement || ms->y_displacement)
	{
		hid_ev_t ev = {
			.type = HIDEV_EVENT_MOUSE_MOTION,
			.no = 0
		};
		ev.mouse_motion.dx = ms->x_displacement;
		ev.mouse_motion.dy = ms->y_displacement;
		hidev_cb(&ev);
	}

	const uint8_t btn = ms->buttons.val & 0x07;
	if (btn != prev_buttons)
	{
		for (int b = 0; b < 3; b++)
		{
			if ((btn & (1 << b)) && !(prev_buttons & (1 << b)))
			{
				hid_ev_t ev = { .type = HIDEV_EVENT_MOUSE_BUTTONDOWN, .no = b };
				hidev_cb(&ev);
			}
			if (!(btn & (1 << b)) && (prev_buttons & (1 << b)))
			{
				hid_ev_t ev = { .type = HIDEV_EVENT_MOUSE_BUTTONUP, .no = b };
				hidev_cb(&ev);
			}
		}
		prev_buttons = btn;
	}

	if (len >= sizeof(hid_mouse_input_report_boot_t) + 1)
	{
		int8_t wheel = (int8_t)data[sizeof(hid_mouse_input_report_boot_t)];
		if (wheel)
		{
			hid_ev_t ev = { .type = HIDEV_EVENT_MOUSE_WHEEL, .no = 0 };
			ev.mouse_wheel.d = wheel;
			hidev_cb(&ev);
		}
	}
}


int usb_hid_receive_hid_event(hid_ev_t *ev) {
	if (!hidev_event_queue)
		return 0;
	return xQueueReceive(hidev_event_queue, ev, 0);
}

/**
 * @brief USB HID Host Device event
 *
 * @param[in] hid_device_handle	 HID Device handle
 * @param[in] event				 HID Host Device event
 * @param[in] arg				 Pointer to arguments, does not used
 */
void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
						   const hid_host_driver_event_t event,
						   void *arg)
{
	hid_host_dev_params_t dev_params;
	ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

	switch (event) {
	case HID_HOST_DRIVER_EVENT_CONNECTED:
		ESP_LOGI(TAG, "HID Device, protocol '%s' CONNECTED",
				 hid_proto_name_str[dev_params.proto]);

		//Open every HID device: opening must not depend on the interface
		//descriptor protocol, since some mice report bInterfaceProtocol=0.
		usb_hid_device_t *hid_dev = calloc(1, sizeof(usb_hid_device_t));
		if (!hid_dev)
			break;

		//A device with the boot interface subclass declaring mouse protocol is
		//switched to BOOT protocol below, where reports have the fixed boot
		//layout. Everything else stays in report protocol and is parsed via
		//its report descriptor.
		hid_dev->boot_mouse = (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE &&
							   dev_params.proto == HID_PROTOCOL_MOUSE);

		const hid_host_device_config_t dev_config = {
			.callback = hid_host_interface_callback,
			.callback_arg = hid_dev
		};

		if (hid_host_device_open(hid_device_handle, &dev_config) != ESP_OK) {
			free(hid_dev);
			break;
		}

		if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
			if (dev_params.proto == HID_PROTOCOL_MOUSE) {
				ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
				ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
			} else {
				ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_REPORT));
			}
			if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
				ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
			}
		}

		if (hid_host_device_start(hid_device_handle) != ESP_OK) {
			hid_host_device_close(hid_device_handle);
			free(hid_dev);
			break;
		}

		//Only parse report descriptors for devices left in report protocol;
		//boot protocol mice send the fixed boot layout, not the report one.
		if (!hid_dev->boot_mouse) {
			size_t rep_desc_size;
			uint8_t *rep_desc = hid_host_get_report_descriptor(hid_device_handle, &rep_desc_size);
			hid_dev->dev = hidev_device_from_descriptor(rep_desc, rep_desc_size, 0, &hidev_cb);
		}

		ESP_LOGI(TAG, "HID device ready: subclass=%d proto=%s mode=%s",
				 dev_params.sub_class,
				 hid_proto_name_str[dev_params.proto],
				 hid_dev->boot_mouse ? "boot" : "report");
		break;
	default:
		break;
	}
}

/**
 * @brief Start USB Host install and handle common USB host library events while app pin not low
 *
 * @param[in] arg  Not used
 */
static void usb_lib_task(void *arg)
{
	const usb_host_config_t host_config = {
		.skip_phy_setup = false,
		.intr_flags = ESP_INTR_FLAG_LEVEL1,
	};

	ESP_ERROR_CHECK(usb_host_install(&host_config));
	xTaskNotifyGive(arg);

	while (true) {
		uint32_t event_flags;
		usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
		// In this example, there is only one client registered
		// So, once we deregister the client, this call must succeed with ESP_OK
		if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
			ESP_ERROR_CHECK(usb_host_device_free_all());
			break;
		}
	}

	ESP_LOGI(TAG, "USB shutdown");
	// Clean up USB Host
	vTaskDelay(10); // Short delay to allow clients clean-up
	ESP_ERROR_CHECK(usb_host_uninstall());
	vTaskDelete(NULL);
}


/**
 * @brief HID Host Device callback
 *
 * Puts new HID Device event to the queue
 *
 * @param[in] hid_device_handle HID Device handle
 * @param[in] event				HID Device event
 * @param[in] arg				Not used
 */
void hid_host_device_callback(hid_host_device_handle_t hid_device_handle,
							  const hid_host_driver_event_t event,
							  void *arg)
{
	const app_event_queue_t evt_queue = {
		.event_group = APP_EVENT_HID_HOST,
		// HID Host Device related info
		.hid_host_device.handle = hid_device_handle,
		.hid_host_device.event = event,
		.hid_host_device.arg = arg
	};

	if (app_event_queue) {
		xQueueSend(app_event_queue, &evt_queue, 0);
	}
}

void usb_hid_task(void)
{
	BaseType_t task_created;
	app_event_queue_t evt_queue;

	/*
	* Create usb_lib_task to:
	* - initialize USB Host library
	* - Handle USB Host events
	*/
	task_created = xTaskCreatePinnedToCore(usb_lib_task,
										   "usb_events",
										   4096,
										   xTaskGetCurrentTaskHandle(),
										   5, NULL, 0);
	assert(task_created == pdTRUE);

	// Wait for notification from usb_lib_task to proceed
	ulTaskNotifyTake(false, 1000);

	/*
	* HID host driver configuration
	* - create background task for handling low level event inside the HID driver
	* - provide the device callback to get new HID Device connection event
	*/
	const hid_host_driver_config_t hid_host_driver_config = {
		.create_background_task = true,
		.task_priority = 5,
		.stack_size = 4096,
		.core_id = 0,
		.callback = hid_host_device_callback,
		.callback_arg = NULL
	};

	ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

	// Create queue
	app_event_queue = xQueueCreate(10, sizeof(app_event_queue_t));
	hidev_event_queue = xQueueCreate(32, sizeof(hid_ev_t));

	ESP_LOGI(TAG, "Waiting for HID Device to be connected");

	while (1) {
		// Wait queue
		if (xQueueReceive(app_event_queue, &evt_queue, portMAX_DELAY)) {
			if (evt_queue.event_group == APP_EVENT_HID_HOST) {
				hid_host_device_event(evt_queue.hid_host_device.handle,
									  evt_queue.hid_host_device.event,
									  evt_queue.hid_host_device.arg);
			}
		}
	}

	ESP_LOGI(TAG, "HID Driver uninstall");
	ESP_ERROR_CHECK(hid_host_uninstall());
	xQueueReset(app_event_queue);
	vQueueDelete(app_event_queue);
}
