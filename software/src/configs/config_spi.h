/* brickletboot
 * Copyright (C) 2016 Olaf Lüke <olaf@tinkerforge.com>
 *
 * config_spi.h: SPI configuration
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

#ifndef CONFIG_SPI_H
#define CONFIG_SPI_H

#if __has_include("config_custom_bootloader.h")
#include "config_custom_bootloader.h"
#else
#include "config_default_bootloader.h"
#endif


// --- SPI ---
#define CONF_SPI_SLAVE_ENABLE true

#ifdef FIRMWARE_USES_SPI_MASTER
#define CONF_SPI_MASTER_ENABLE true
#else
#define CONF_SPI_MASTER_ENABLE false
#endif

#endif
