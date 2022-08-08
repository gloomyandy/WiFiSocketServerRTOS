set(srcs "FlashDB/src/fdb.c"
         "FlashDB/src/fdb_file.c"
         "FlashDB/src/fdb_kvdb.c"
         "FlashDB/src/fdb_tsdb.c"
         "FlashDB/src/fdb_utils.c"
         "FlashDB/port/fal/src/fal.c"
         "FlashDB/port/fal/src/fal_flash.c"
         "FlashDB/port/fal/src/fal_partition.c"
         "FlashDB/port/fal/src/fal_rtt.c"
         "FlashDB/demos/esp32_spi_flash/components/FlashDB/porting/fal_flash_esp32_port.c")

set(include_dirs "config"
                 "FlashDB/demos/esp32_spi_flash/components/FlashDB/inc"
                 "FlashDB/inc"
                 "FlashDB/port/fal/inc")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "${include_dirs}"
                       PRIV_REQUIRES spi_flash)