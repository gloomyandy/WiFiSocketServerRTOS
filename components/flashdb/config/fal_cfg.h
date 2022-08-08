#pragma once

#include_next <fal_cfg.h>

#pragma once

#undef FAL_PART_TABLE
#define FAL_PART_TABLE  { {FAL_PART_MAGIC_WORD, "fdb_kvdb1", NOR_FLASH_DEV_NAME, 0, 512 * 1024, 0} }