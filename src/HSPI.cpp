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

#include "esp_attr.h"

#include "HSPI.h"

#include "esp8266/spi.h"
#include "esp8266/gpio.h"

HSPIClass::HSPIClass()
{
}

void HSPIClass::InitMaster(uint8_t mode, uint32_t clockReg, bool msbFirst)
{
}

void HSPIClass::end()
{
}

// Begin a transaction without changing settings
void IRAM_ATTR HSPIClass::beginTransaction()
{
}

void IRAM_ATTR HSPIClass::endTransaction()
{
}

// clockDiv is NOT the required division ratio, it is the value to write to the REG(SPI_CLOCK(HSPI)) register
void HSPIClass::setClockDivider(uint32_t clockDiv)
{
}

void HSPIClass::setDataBits(uint16_t bits)
{
}

uint32_t IRAM_ATTR HSPIClass::transfer32(uint32_t data)
{
	return 0;
}

/**
 * @param out uint32_t *
 * @param in  uint32_t *
 * @param size uint32_t
 */
void IRAM_ATTR HSPIClass::transferDwords(const uint32_t * out, uint32_t * in, uint32_t size)
{
}

void IRAM_ATTR HSPIClass::transferDwords_(const uint32_t * out, uint32_t * in, uint8_t size)
{
}

// End
