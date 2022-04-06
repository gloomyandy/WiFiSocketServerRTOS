#pragma once

#include "driver/gpio.h"
#include "hal/spi_types.h"
#include "soc/spi_struct.h"

#define HSPI		SPI2_HOST
#define SPI_LL_GET_HW(ID) ((ID)==0? ({abort();(spi_dev_t*)NULL;}):&GPSPI2)

static const gpio_num_t SCK = GPIO_NUM_4;
static const gpio_num_t MOSI = GPIO_NUM_6;
static const gpio_num_t MISO = GPIO_NUM_5;