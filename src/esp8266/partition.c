#include <stdlib.h>

#include "partition.h"

extern uint32_t _scratch_start;

esp_err_t esp_partition_mmap(const esp_partition_t* partition, size_t offset, size_t size,
                             spi_flash_mmap_memory_t memory,
                             const void** out_ptr, spi_flash_mmap_handle_t* out_handle)
{
    *out_ptr = NULL;
    if (spi_flash_cache2phys(&_scratch_start) == partition->address)
    {
        *out_ptr = &_scratch_start;
    }

    return ESP_ERR_NOT_SUPPORTED;
}