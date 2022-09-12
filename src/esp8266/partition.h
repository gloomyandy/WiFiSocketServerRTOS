#pragma once

#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPI_FLASH_MMAP_DATA,
} spi_flash_mmap_memory_t;

typedef uint32_t spi_flash_mmap_handle_t;

esp_err_t esp_partition_mmap(const esp_partition_t* partition, size_t offset, size_t size,
                             spi_flash_mmap_memory_t memory,
                             const void** out_ptr, spi_flash_mmap_handle_t* out_handle);

#ifdef __cplusplus
}
#endif