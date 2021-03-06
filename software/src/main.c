/* brickletboot
 * Copyright (C) 2016 Olaf Lüke <olaf@tinkerforge.com>
 *
 * main.c: Bricklet bootloader
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */



/*

For Bricklets with co-processor we use the following flash memory map
(in this case for 16kb of flash):

-- BOOTLOADER --------------------------------------------------------------
| brickletboot                                                             |
| 0000-2000                                                                |
----------------------------------------------------------------------------

-- FIRMWARE ----------------------------------------------------------------
| firmware      | firmware version      | device id    | firmware crc      |
| 2000-3ff4     | 3ff4-3ff8             | 3ff8-3ffc    | 3ffc-4000         |
----------------------------------------------------------------------------

*/

#include <stdio.h>

#include "port.h"
#include "clock.h"
#include "gclk.h"
#include "crc32.h"
#include "spi.h"
#include "bootloader_spitfp.h"
#include "boot.h"
#include "tfp_common.h"

#include "bricklib2/bootloader/tinydma.h"
#include "bricklib2/bootloader/tinywdt.h"
#include "bricklib2/bootloader/tinynvm.h"
#include "bricklib2/bootloader/bootloader.h"
#include "bricklib2/logging/logging.h"
#include "configs/config.h"

const uint32_t device_identifier __attribute__ ((section(".device_identifier"))) = BOOTLOADER_DEVICE_IDENTIFIER;
const uint32_t bootloader_version __attribute__ ((section(".bootloader_version"))) = (BOOTLOADER_VERSION_MAJOR << 16) | (BOOTLOADER_VERSION_MINOR << 8) | (BOOTLOADER_VERSION_REVISION << 0);

static void configure_led(void) {
#if 0
	struct port_config pin_conf;
	port_get_config_defaults(&pin_conf);

	port_pin_set_config(BOOTLOADER_STATUS_LED_PIN, &pin_conf);
	port_pin_set_output_level(BOOTLOADER_STATUS_LED_PIN, false);
#endif

	// Do above code by hand, saves 40 bytes

	PortGroup *const port = &PORT->Group[0];
	const uint32_t pin_mask = (1 << BOOTLOADER_STATUS_LED_PIN);

	const uint32_t lower_pin_mask = (pin_mask & 0xFFFF);
	const uint32_t upper_pin_mask = (pin_mask >> 16);

	// TODO: Decide if lower or upper pin_mask is needed with pre-processor
	//       to save a few bytes

	// Configure lower 16 bits (needed if lower_pin_mask != 0)
	port->WRCONFIG.reg = (lower_pin_mask << PORT_WRCONFIG_PINMASK_Pos) | PORT_WRCONFIG_WRPMUX | PORT_WRCONFIG_WRPINCFG;

	// Configure upper 16 bits  (needed if upper_pin_mask != 0)
	port->WRCONFIG.reg = (upper_pin_mask << PORT_WRCONFIG_PINMASK_Pos) | PORT_WRCONFIG_WRPMUX | PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_HWSEL;

	// Direction to output
	port->DIRSET.reg = pin_mask;

	// Turn LED off
	port->OUTSET.reg = (1 << BOOTLOADER_STATUS_LED_PIN);
}

// Initialize everything that is needed for bootloader as well as firmware
void system_init(void) {
	system_clock_init();
	tinywdt_init();
	configure_led();

	// The following can be enabled if needed by the firmware
	// Initialize EVSYS hardware
	//_system_events_init();

	// Initialize External hardware
	//_system_extint_init();

	// Initialize DIVAS hardware
	//_system_divas_init();
}

BootloaderStatus bootloader_status;
int main() {
	// Jump to firmware if we can
	const uint8_t can_jump_to_firmware = boot_can_jump_to_firmware();
	if(can_jump_to_firmware == TFP_COMMON_SET_BOOTLOADER_MODE_STATUS_OK) {
		PORT->Group[0].OUTCLR.reg = (1 << BOOTLOADER_STATUS_LED_PIN); // Turn LED on by default for firmware
		boot_jump_to_firmware();
	}

	// TODO: Set LOCK bits during initial flashing (in AUX memory)
	//       and temporarily remove LOCK bits if in bootloader mode

#if LOGGING_LEVEL != LOGGING_NONE
	logging_init();
	logi("Starting brickletboot (version %d.%d.%d)\n\r", BOOTLOADER_VERSION_MAJOR, BOOTLOADER_VERSION_MINOR, BOOTLOADER_VERSION_REVISION);
	logi("Compiled on " __DATE__ " " __TIME__ "\n\r");
#endif

	// We can't jump to firmware, so lets enter bootloader mode
	bootloader_status.boot_mode = BOOT_MODE_BOOTLOADER;
	bootloader_status.status_led_config = 0;
	bootloader_status.st.descriptor_section = tinydma_get_descriptor_section();
	bootloader_status.st.write_back_section = tinydma_get_write_back_section();
	bootloader_status.system_timer_tick = 0;

	tinynvm_init();

	spitfp_init(&bootloader_status.st);

	uint8_t tick_counter = 0;
	while(true) {
		tick_counter++;;
		if(tick_counter >= 40) { // We call spitfp_tick aprox. 40 times per ms
			bootloader_status.system_timer_tick++;
			if(bootloader_status.system_timer_tick % 2 == 0) {
				PORT->Group[0].OUTSET.reg = (1 << BOOTLOADER_STATUS_LED_PIN);
			} else {
				PORT->Group[0].OUTCLR.reg = (1 << BOOTLOADER_STATUS_LED_PIN);
			};
			tick_counter = 0;
		}

		spitfp_tick(&bootloader_status);
	}
}
