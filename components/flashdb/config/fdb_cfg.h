#pragma once

#include_next <fdb_cfg.h>

#include <esp_log.h>
#define FDB_PRINT(...) ESP_LOGI("fdb", __VA_ARGS__)

#undef FDB_USING_TSDB

/* print debug information */
#undef FDB_DEBUG_ENABLE
