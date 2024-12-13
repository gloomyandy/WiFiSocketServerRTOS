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

/* STM32 Port notes:
   For some reason using the original Duet3D SPI configuration results in sperodic data corruption,
   in particular the first byte of a transfer to RRF is often 0. After much testing it seems that
   two changes fix this problem. The first change is to add a short delay just at the start of an
   spi transaction. The spi_pre_transmit_callback function is used to do this. The second change is
   to prevent the esp32 code for attempting to adjust the timing of the spi signals. We do this by
   a combination of ensuring that the "dummy byte" operation is not used and be setting the:
   spi_pre_transmit_callback value to be such that the delay compensation is not used. This setting
   needs to vary based on the spi clock speed and is set when selecting the clock.

   When using Ethernet the SPI and MAC devices share a DMA device. With the default MAC DMA burst
   setting (EMAC_LL_DMA_BURST_LENGTH_32BEAT) spi transfers would sometimes not work (no data would
   be transferred to RAM even though the operation appears to complete ok). For this reasons previous
   versions of this code used polling rather than DMA when using Ethernet. This required changes to
   esp-idf code to allow polled transfers > 64 bits. We now set the MAC DMA burst size to 16
   (EMAC_LL_DMA_BURST_LENGTH_16BEAT) which seems to fix the problem. Currently this requires a change
   to the esp-idf file emac_hal.c.
*/

static void IRAM_ATTR spi_pre_transmit_callback(spi_transaction_t* arg)
{
	ets_delay_us(2);
}

static spi_device_handle_t spi;

static void clockCtrl2Cfg(uint32_t val, spi_device_interface_config_t *devcfg)
{
	switch (val)
	{
	case 0x1001:
		devcfg->clock_speed_hz = 80000000/2;
		devcfg->input_delay_ns = 12;
		break;
	case 0x3403:
		devcfg->clock_speed_hz = 80000000/4;
		devcfg->input_delay_ns = 25;
		break;
	case 0x2001:
	case 0x2402:
	case 0x2002:
		devcfg->clock_speed_hz = 80000000/3;
		devcfg->input_delay_ns = 25;
		break;
	case 0x2003:
	default:
		devcfg->clock_speed_hz = 80000000/4;
		devcfg->input_delay_ns = 25;
		break;

	}
}

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
    buscfg.flags = SPICOMMON_BUSFLAG_MASTER|SPICOMMON_BUSFLAG_IOMUX_PINS;
	buscfg.intr_flags = ESP_INTR_FLAG_IRAM;

	spi_device_interface_config_t devcfg;
	memset(&devcfg, 0, sizeof(devcfg));
	devcfg.mode = mode;
	devcfg.spics_io_num = -1;
	devcfg.flags = SPI_DEVICE_NO_DUMMY | (!msbFirst ? SPI_DEVICE_BIT_LSBFIRST : 0);
	devcfg.queue_size = 4;
	devcfg.pre_cb = spi_pre_transmit_callback;

	clockCtrl2Cfg(clockReg, &devcfg);

	spi_bus_initialize(MSPI, &buscfg, SPI_DMA_CH_AUTO);
	spi_bus_add_device(MSPI, &devcfg, &spi);
	spi_device_acquire_bus(spi, portMAX_DELAY);
}

void HSPIClass::end()
{
	spi_device_release_bus(spi);
	spi_bus_remove_device(spi);
	spi_bus_free(MSPI);
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
	trans.flags |= SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
	spi_device_polling_transmit(spi, &trans);
	return *((uint32_t*)trans.rx_data);
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
