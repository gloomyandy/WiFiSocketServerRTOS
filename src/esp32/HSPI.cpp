/*
 SPI.cpp - SPI library for esp8266

 Copyright (c) 2015 Hristo Gochkov. All rights reserved.
 This file is part of the esp8266 core for Arduino environment.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include <cmath>
#include <string.h>

#include "esp_attr.h"

#include "esp32/spi.h"

#include "esp_system.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "HSPI.h"
#include "Config.h"

static spi_device_handle_t spi;

HSPIClass::HSPIClass()
{
}

void HSPIClass::InitMaster(uint8_t mode, uint32_t clockReg, bool msbFirst)
{
	spi_bus_config_t buscfg;
	memset(&buscfg, 0, sizeof(buscfg));
	buscfg.mosi_io_num = MOSI;
	buscfg.miso_io_num = MISO;
	buscfg.sclk_io_num = SCK;
	buscfg.quadwp_io_num = -1;
	buscfg.quadhd_io_num = -1;
	buscfg.max_transfer_sz = 0; // use default val
	buscfg.flags = SPICOMMON_BUSFLAG_MASTER;
	buscfg.intr_flags = ESP_INTR_FLAG_IRAM;

	spi_device_interface_config_t devcfg;
	memset(&devcfg, 0, sizeof(devcfg));
	devcfg.mode = mode;
	devcfg.clock_speed_hz = clockReg;
	devcfg.spics_io_num = -1;
	devcfg.flags = SPI_DEVICE_NO_DUMMY | (!msbFirst ? SPI_DEVICE_BIT_LSBFIRST : 0);
	devcfg.queue_size = 4;

	spi_bus_initialize(MSPI, &buscfg, SPI_DMA_CH_AUTO);
	spi_bus_add_device(MSPI, &devcfg, &spi);

	spi_device_acquire_bus(spi, portMAX_DELAY);
}

void HSPIClass::end()
{
	spi_device_release_bus(spi);
}

// Begin a transaction without changing settings
void IRAM_ATTR HSPIClass::beginTransaction()
{
}

void IRAM_ATTR HSPIClass::endTransaction()
{
}

void HSPIClass::setClockDivider(uint32_t clockDiv)
{
}

void HSPIClass::setDataBits(uint16_t bits)
{
}

uint32_t IRAM_ATTR HSPIClass::transfer32(uint32_t data)
{
	spi_transaction_t trans;
	memset(&trans, 0, sizeof(trans));
	trans.length = 32;
	*((uint32_t*)trans.tx_data) = data;
	trans.flags |= SPI_TRANS_USE_TXDATA;
	esp_err_t err = spi_device_polling_transmit(spi, &trans);
	return err == ESP_OK ? data : 0;
}

/**
 * @param out uint32_t *
 * @param in  uint32_t *
 * @param size uint32_t
 */
void IRAM_ATTR HSPIClass::transferDwords(const uint32_t * out, uint32_t * in, uint32_t size)
{
	if (size == 0) {
		return;
	}

	spi_transaction_t trans;
	memset(&trans, 0, sizeof(trans));
	trans.length = 8 * 4 * size;

	if (out) {
		trans.tx_buffer = out;
	}

	if (in)
	{
		trans.rx_buffer = in;
		trans.rxlength = trans.length;
	}

	spi_device_polling_transmit(spi, &trans);
}

void IRAM_ATTR HSPIClass::transferDwords_(const uint32_t * out, uint32_t * in, uint8_t size)
{
}

// End
