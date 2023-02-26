#pragma once

#include "driver/gpio.h"
#include "hal/spi_types.h"
#include "soc/spi_struct.h"


#if CONFIG_IDF_TARGET_ESP32
# if SUPPORT_ETHERNET
#define MSPI		(SPI2_HOST)
#define SPI_LL_GET_HW(ID) (((ID) == MSPI) ? &SPI2 : ({abort();(spi_dev_t*)NULL;}))
# else
#define MSPI		(SPI3_HOST)
#define SPI_LL_GET_HW(ID) (((ID) == MSPI) ? &SPI3 : ({abort();(spi_dev_t*)NULL;}))
# endif
#else
#define MSPI		(SPI2_HOST)
#define SPI_LL_GET_HW(ID) (((ID) == MSPI) ? &GPSPI2 : ({abort();(spi_dev_t*)NULL;}))
#endif

#if CONFIG_IDF_TARGET_ESP32C3
static const gpio_num_t SCK = GPIO_NUM_4;
static const gpio_num_t MOSI = GPIO_NUM_6;
static const gpio_num_t MISO = GPIO_NUM_5;
#elif CONFIG_IDF_TARGET_ESP32S3
static const gpio_num_t SCK = GPIO_NUM_12;
static const gpio_num_t MOSI = GPIO_NUM_11;
static const gpio_num_t MISO = GPIO_NUM_13;
#elif CONFIG_IDF_TARGET_ESP32
# if SUPPORT_ETHERNET
// When running RMII based ethernet modules our normal SPI pins are in use
// so we switch to using SPI2 rather than SPI3
static const gpio_num_t SCK = GPIO_NUM_14;
static const gpio_num_t MOSI = GPIO_NUM_13;
static const gpio_num_t MISO = GPIO_NUM_12;
# else
static const gpio_num_t SCK = GPIO_NUM_18;
static const gpio_num_t MOSI = GPIO_NUM_23;
static const gpio_num_t MISO = GPIO_NUM_19;
# endif
#else
#error "pins not specifed for target chip"
#endif
