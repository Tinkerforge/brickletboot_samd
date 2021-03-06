/* brickletboot
 * Copyright (C) 2016 Olaf Lüke <olaf@tinkerforge.com>
 *
 * boot.c: Functions for transition between firmware and bootloader
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

#include "boot.h"

#include "configs/config.h"
#include "tfp_common.h"
#include "bricklib2/bootloader/bootloader.h"

#include "dsu_crc32.h"

typedef void (* boot_firmware_start_func_t)(void);

uint32_t boot_calculate_firmware_crc(void) {
	// unlock DSU
	// equivalent to: system_peripheral_unlock(SYSTEM_PERIPHERAL_ID(DSU), ~SYSTEM_PERIPHERAL_ID(DSU));
	// saves 50 bytes
	PAC1->WPCLR.reg = 1 << (ID_DSU % 32);

	// equivalent to dsu_crc32_init();
	PM->APBBMASK.reg |= PM_APBBMASK_DSU;

	uint32_t crc = 0xFFFFFFFF;
	dsu_crc32_cal((const uint32_t)BOOTLOADER_FIRMWARE_START_POS, BOOTLOADER_FIRMWARE_SIZE - BOOTLOADER_FIRMWARE_CRC_SIZE, &crc);

	return ~crc;
}

// We use the relevant TFP_COMMON return values here
uint8_t boot_can_jump_to_firmware(void) {
	// Check first 4 bytes, if in bootloader mode they will be all ones
	if(BOOTLOADER_FIRMWARE_FIRST_BYTES == BOOTLOADER_FIRMWARE_FIRST_BYTES_DEFAULT) {
		return TFP_COMMON_SET_BOOTLOADER_MODE_STATUS_ENTRY_FUNCTION_NOT_PRESENT;
	}

	if(BOOTLOADER_FIRMWARE_CONFIGURATION_POINTER->device_identifier != BOOTLOADER_DEVICE_IDENTIFIER) {
		return TFP_COMMON_SET_BOOTLOADER_MODE_STATUS_DEVICE_IDENTIFIER_INCORRECT;
	}

	if(BOOTLOADER_FIRMWARE_CONFIGURATION_POINTER->firmware_crc != boot_calculate_firmware_crc()) {
		return TFP_COMMON_SET_BOOTLOADER_MODE_STATUS_CRC_MISMATCH;
	}

	return TFP_COMMON_SET_BOOTLOADER_MODE_STATUS_OK;
}


void boot_jump_to_firmware(void) {
	register boot_firmware_start_func_t firmware_start_func;
	const uint32_t stack_pointer_address = BOOTLOADER_FIRMWARE_START_POS;
	const uint32_t reset_pointer_address = BOOTLOADER_FIRMWARE_START_POS + 4;

	// Set stack pointer with the first word of the run mode program
	// Vector table's first entry is the stack pointer value
	__set_MSP((*(uint32_t *)stack_pointer_address));

	//set the NVIC's VTOR to the beginning of main app
	SCB->VTOR = stack_pointer_address & SCB_VTOR_TBLOFF_Msk;

	// Set the program counter to the application start address
	// Vector table's second entry is the system reset value
	firmware_start_func = *((boot_firmware_start_func_t *)reset_pointer_address);
	firmware_start_func();
}
