#include "WirelessConfigurationMgr.h"

#include <cstring>

#include "esp_system.h"
#include "rom/ets_sys.h"

#include "Config.h"

#if ESP8266
#include "esp8266/partition.h"
#endif

WirelessConfigurationMgr* WirelessConfigurationMgr::instance = nullptr;
SemaphoreHandle_t WirelessConfigurationMgr::kvsLock = nullptr;

void WirelessConfigurationMgr::Init()
{
	InitKVS();

	// Memory map the partition. The base pointer will be returned.
	spi_flash_mmap_handle_t mapHandle;
	const esp_partition_t* scratch = GetScratchPartition();
	esp_partition_mmap(scratch, 0, scratch->size, SPI_FLASH_MMAP_DATA, reinterpret_cast<const void**>(&scratchBase), &mapHandle);

	WirelessConfigurationData temp;

	// Check if first time and the storage should be initialized
	if (!GetKV(GetSsidKey(0), &temp, sizeof(temp)))
	{
		debugPrintf("initializing SSID storage...");
		Reset();

		// Restore SSID info from old firmware
		const esp_partition_t* oldSsids = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
			ESP_PARTITION_SUBTYPE_DATA_NVS, "ssids_old");

		if (oldSsids) {
			debugPrintf("restoring old SSID...");
			const size_t eepromSizeNeeded = (MaxRememberedNetworks + 1) * sizeof(WirelessConfigurationData);

			uint8_t *buff = reinterpret_cast<uint8_t*>(malloc(eepromSizeNeeded));
			memset(buff, 0xFF, eepromSizeNeeded);
			esp_partition_read(oldSsids, 0, buff, eepromSizeNeeded);

			WirelessConfigurationData *data = reinterpret_cast<WirelessConfigurationData*>(buff);
			for(int i = 0; i <= MaxRememberedNetworks; i++) {
				WirelessConfigurationData *d = &(data[i]);
				if (d->ssid[0] != 0xFF) {
					SetSsidData(i, *d);
				}
			}

			free(buff);
			debugPrintf("done!\n");
		}
	}

	// Check that the scratch partition is ready for writing. All bytes after the current
	// offset must be 0xFF.
	uint32_t offset = 0;
	GetKV(GetScratchKey(SCRATCH_OFFSET_ID), &offset, sizeof(offset));

	for(const uint8_t* current = scratchBase + offset; current < scratchBase + scratch->size; current++)
	{
		if (*current != 0xFF)
		{
			debugPrintf("scratch partition inconsistent at position %u, erasing...", current - scratchBase);
			EraseScratch();
			debugPrintf("done!\n", current - scratchBase);
			break;
		}
	}

	// Storing an enterprise SSID might not have gone all the way.
	// The SSID information is written last, after all of the credentials;
	// so there might be orphaned credentials taking up space. Clear them here.
	CredentialsInfo sizes;

	for(int i = 1; i < MaxRememberedNetworks; i++)
	{
		if ((GetSsid(i, temp) && IsSsidBlank(temp)) && GetCredentialSizes(i, sizes))
		{
			EraseCredentials(i);
		}
	}
}

void WirelessConfigurationMgr::Reset()
{
	EraseScratch();

	// Reset storage and reset values to default.
	// 	- the SSID slot must be reset to a blank value, and credentials for
	// 		each slot must be cleared
	//  - the scratch key-value storage namespace must be cleared
	//
	// Work down to SSID 0, since it is used to detect whether
	// the KVS has been initialized for the first time.
	for (int i = MaxRememberedNetworks; i >= 0; i--)
	{
		EraseSsid(i); // this clears the associated credentials as well
	}
}

int WirelessConfigurationMgr::SetSsid(const WirelessConfigurationData& data, bool ap = false)
{
	WirelessConfigurationData d;

	int ssid = WirelessConfigurationMgr::AP;

	if (!ap)
	{
		ssid = GetSsid(data.ssid, d);

		if (ssid < 0)
		{
			ssid = FindEmptySsidEntry();
			if (ssid == WirelessConfigurationMgr::AP) { // reserved for AP details
				ssid = -1;
			}
		}
	}

	if (ssid >= 0)
	{
		if (EraseSsid(ssid))
		{
			if (SetSsidData(ssid, data))
			{
				return ssid;
			}
		}

		ssid = -1;
	}

	return ssid;
}

bool WirelessConfigurationMgr::EraseSsid(int ssid)
{
	bool res = false;

	if (ssid >= 0)
	{
		// Erase the SSID first, then the credentials.
		// There are two reasons for this:
		// 		- SSID is stored in one operation. If it is interrupted by power loss,
		//			it is either still valid or erased at next reboot
		//		- Credentials are stored in multiple chunks. If it is interrupted by power loss,
		//			the SSID will have already been cleared and thus erasure will
		//			continue at reboot inside WirelessConfigurationMgr::Init()
		res = EraseSsidData(ssid);
		if (res)
		{
			res = EraseCredentials(ssid);
		}
	}

	return res;
}

bool WirelessConfigurationMgr::EraseSsid(const char *ssid)
{
	WirelessConfigurationData temp;
	return EraseSsid(GetSsid(ssid, temp));
}

bool WirelessConfigurationMgr::GetSsid(int ssid, WirelessConfigurationData& data)
{
	if (ssid <= MaxRememberedNetworks)
	{
		return GetKV(GetSsidKey(ssid), &data, sizeof(data));
	}

	return false;
}

int WirelessConfigurationMgr::GetSsid(const char *ssid, WirelessConfigurationData& data)
{
	for (int i = MaxRememberedNetworks; i >= 0; i--)
	{
		WirelessConfigurationData temp;
		if (GetSsid(i, temp) && strncmp(ssid, temp.ssid, sizeof(temp.ssid)) == 0)
		{
			data = temp;
			return i;
		}
	}
	return -1;
}

bool WirelessConfigurationMgr::BeginEnterpriseSsid(const WirelessConfigurationData &data)
{
	// Personal network assumed unless otherwise stated. PSK is indicated by WirelessConfigurationData::eap.protocol == 1,
	// which is the null terminator for the pre-shared key. Enforce that here.
	static_assert(offsetof(WirelessConfigurationData, eap.protocol) ==
					offsetof(WirelessConfigurationData,
							password[sizeof(data.password) - sizeof(data.eap.protocol)]));

	WirelessConfigurationData temp;
	int newSsid = GetSsid(data.ssid, temp);

	if (newSsid < 0)
	{
		newSsid = FindEmptySsidEntry();
	}

	if (newSsid > 0)
	{
		if (EraseSsid(newSsid))
		{
			pendingSsid = static_cast<PendingEnterpriseSsid*>(calloc(1, sizeof(PendingEnterpriseSsid)));
			if (pendingSsid)
			{
				pendingSsid->data = data;
				pendingSsid->ssid = newSsid;
				return true;
			}
		}
	}

	return false;
}

bool WirelessConfigurationMgr::SetEnterpriseCredential(int cred, const void* buff, size_t size)
{
	if (pendingSsid)
	{
		uint32_t *credsSizes = reinterpret_cast<uint32_t*>(&(pendingSsid->sizes));
		if (SetCredential(pendingSsid->ssid, cred, credsSizes[cred]/MaxCredentialChunkSize, buff, size))
		{
			credsSizes[cred] += size;
			return true;
		}
	}

	return false;
}

bool WirelessConfigurationMgr::EndEnterpriseSsid(bool cancel = true)
{
	bool res = true;

	if (pendingSsid)
	{
		if (!cancel)
		{
			res = SetCredential(pendingSsid->ssid, CREDS_SIZES_IDX, 0,
				&(pendingSsid->sizes), sizeof(pendingSsid->sizes)) &&
				SetSsidData(pendingSsid->ssid, pendingSsid->data);
		}

		if (cancel || !res)
		{
			// Delete the credentials written so far, since a reboot might
			// be far off. This is a best-effort cleanup, so do not factor
			// in the result.
			EraseCredentials(pendingSsid->ssid);
		}

		free(pendingSsid);
		pendingSsid = nullptr;
	}

	return res;
}

const uint8_t* WirelessConfigurationMgr::GetEnterpriseCredentials(int ssid, CredentialsInfo& sizes, CredentialsInfo& offsets)
{
	const uint8_t *res = nullptr;

	// Check that all information needed for the proceeding operations can
	// be retrieved.
	uint32_t loadedSsid = 0, baseOffset = 0;

	if (GetKV(GetScratchKey(LOADED_SSID_ID), &loadedSsid, sizeof(loadedSsid)) &&
		GetKV(GetScratchKey(SCRATCH_OFFSET_ID), &baseOffset, sizeof(baseOffset)) &&
		GetCredentialSizes(ssid, sizes))
	{
		// Get the total size of credentials
		const uint32_t *sizesArr = reinterpret_cast<const uint32_t*>(&sizes);
		size_t totalSize = 0;
		for(int cred = 0; cred < sizeof(sizes)/sizeof(sizesArr[0]); cred++)
		{
			totalSize += sizesArr[cred];
		}

		const esp_partition_t* scratch = GetScratchPartition();

		if (scratch)
		{
			// If the SSID has already been loaded, just return the existing pointer
			// and compute offsets. If not, load it in the scratch partition.
			if (loadedSsid == ssid)
			{
				uint32_t *offsetsArr = reinterpret_cast<uint32_t*>(&offsets);

				for(int cred = 0, offset = 0; cred < sizeof(offsets)/sizeof(offsetsArr[0]); cred++)
				{
					offsetsArr[cred] = offset;
					offset += sizesArr[cred];
				}

				res = (scratchBase + baseOffset - totalSize);
			}
			else
			{
				bool ok = true;

				// Increment the offset first. If it will not fit, start from the top
				// again.
				if (baseOffset + totalSize > scratch->size)
				{
					baseOffset = 0;
					esp_err_t err = esp_partition_erase_range(scratch, baseOffset, scratch->size);
					ok = (err == ESP_OK);
				}

				if (ok)
				{
					uint32_t newOffset = baseOffset + totalSize;
					ok = SetKV(GetScratchKey(SCRATCH_OFFSET_ID), &newOffset, sizeof(newOffset));

					if (ok)
					{
						// Store offsets from the base offset
						uint32_t *offsetsArr = reinterpret_cast<uint32_t*>(&offsets);
						uint8_t *buff = static_cast<uint8_t*>(malloc(MaxCredentialChunkSize));

						for(int cred = 0, offset = 0; ok && cred < sizeof(offsets)/sizeof(offsetsArr[0]); cred++)
						{
							offsetsArr[cred] = offset;

							for(int chunk = 0, sz = 0, remain = sizesArr[cred];
								ok && remain > 0; chunk++, remain -= sz, offset += sz)
							{
								memset(buff, 0, MaxCredentialChunkSize);
								sz = GetCredential(ssid, cred, chunk, buff,
										remain >= MaxCredentialChunkSize ? MaxCredentialChunkSize : remain);

								if (sz)
								{
									esp_err_t err = esp_partition_write(scratch, baseOffset + offset, buff, sz);
									ok = (err == ESP_OK);
								}
								else
								{
									ok = false;
								}
							}
						}

						free(buff);

						if (ok)
						{
							loadedSsid = ssid;
							ok = SetKV(GetScratchKey(LOADED_SSID_ID), &loadedSsid, sizeof(loadedSsid));

							if (ok)
							{
								res = scratchBase + baseOffset;
							}
						}
					}
				}
			}
		}
	}

	return res;
}

void WirelessConfigurationMgr::LockKVS(fdb_db_t db)
{
	xSemaphoreTake(kvsLock, portMAX_DELAY);
}

void WirelessConfigurationMgr::UnlockKVS(fdb_db_t db)
{
	xSemaphoreGive(kvsLock);
}

void WirelessConfigurationMgr::InitKVS()
{
	/**
	 * This class manages two partitions: a credential scratch partition and
	 * the key-value storage partition.
	 *
	 * The scratch partition is a raw partition that provides the required contiguous memory
	 * for enterprise network credentials, which can get huge. These credentials are loaded and assembled
	 * from the key-value store in a wear-leveled fashion.
	 *
	 * The key-value store uses FlashDB. They are used to store wireless configuration data,
	 * the credential chunks, and some other bits and pieces. There are three 'namespaces':
	 * 		- ssids - stores wireless configuration data, with keys 'ssids/xx' where xx is the ssid slot
	 * 		- creds - stores credential for a particular wireless config data stored in 'ssids', with keys
	 * 					'creds/xx/yy_zz', where xx is the ssid slot, yy is the credential index
	 * 					and zz is the credential chunk
	 * 		- scratch - stores some values related to the scratch partition, with keys 'scratch_ss' where
	 * 					ss is the string id
	 **/

	kvsLock = xSemaphoreCreateCounting(1, 1);

	fdb_kvdb_control(&kvs, FDB_KVDB_CTRL_SET_LOCK, reinterpret_cast<void*>(LockKVS));
	fdb_kvdb_control(&kvs, FDB_KVDB_CTRL_SET_UNLOCK, reinterpret_cast<void*>(UnlockKVS));
	fdb_kvdb_init(&kvs, "env", "fdb_kvdb1", NULL, NULL);
}

bool WirelessConfigurationMgr::DeleteKV(std::string key)
{
	fdb_err_t err = fdb_kv_del(&kvs, key.c_str());
	return err == FDB_NO_ERR;
}

bool WirelessConfigurationMgr::SetKV(std::string key, const void *buff, size_t sz)
{
	struct fdb_blob blob = {
		const_cast<void*>(buff), // buf
		sz, // size
		0, 0, 0 // saved
	};

	fdb_err_t err = fdb_kv_set_blob(&kvs, key.c_str(), &blob);
	return err == FDB_NO_ERR;
}

bool WirelessConfigurationMgr::GetKV(std::string key, void* buff, size_t sz)
{
	struct fdb_blob blob = {
		const_cast<void*>(buff), // buf
		sz, // size
		0, 0, 0 // saved
	};

	fdb_kv_get_blob(&kvs, key.c_str(), &blob);
	return blob.saved.len == sz;
}

std::string WirelessConfigurationMgr::GetSsidKey(int ssid)
{
	std::string res = SSIDS_NS;
	res.append("/");
	res.append(std::to_string(ssid));
	return res;
}

bool WirelessConfigurationMgr::SetSsidData(int ssid, const WirelessConfigurationData& data)
{
	if (ssid <= MaxRememberedNetworks)
	{
		return SetKV(GetSsidKey(ssid), &data, sizeof(data));
	}

	return false;
}

bool WirelessConfigurationMgr::EraseSsidData(int ssid)
{
	WirelessConfigurationData clean;
	memset(&clean, 0xFF, sizeof(clean));
	return SetSsidData(ssid, clean);
}

const esp_partition_t* WirelessConfigurationMgr::GetScratchPartition()
{
	const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, SCRATCH_NS);
	return part;
}

std::string WirelessConfigurationMgr::GetScratchKey(const char* name)
{
	std::string res = SCRATCH_NS;
	res.append("/");
	res.append(name);
	return res;
}

bool WirelessConfigurationMgr::EraseScratch()
{
	bool res = false;

	// Erase the scratch partition first, then the scratch key-values.
	// The reason for this is that if the scratch partition is interrupted
	// and does not reach the current offset, the memory from the current
	// offset will still be valid to write to.
	const esp_partition_t* scratch = GetScratchPartition();
	if (scratch)
	{
		esp_err_t err = esp_partition_erase_range(scratch, 0, scratch->size);

		if (err == ESP_OK)
		{
			uint32_t zero = 0;
			res = SetKV(GetScratchKey(LOADED_SSID_ID), &zero, sizeof(zero)) &&
					SetKV(GetScratchKey(SCRATCH_OFFSET_ID), &zero, sizeof(zero));
		}
	}

	return res;
}

std::string WirelessConfigurationMgr::GetCredentialKey(int ssid, int cred, int chunk = 0)
{
	std::string res = CREDS_NS;
	res.append("/");
	res.append(std::to_string(ssid));
	res.append("/");
	res.append(std::to_string(cred));
	res.append("/");
	res.append(std::to_string(chunk));
	return res;
}

bool WirelessConfigurationMgr::SetCredential(int ssid, int cred, int chunk, const void* buff, size_t sz)
{
	return ResetIfCredentialsLoaded(ssid) && SetKV(GetCredentialKey(ssid, cred, chunk), buff, sz);
}

size_t WirelessConfigurationMgr::GetCredential(int ssid, int cred, int chunk, void* buff, size_t sz)
{
	return GetKV(GetCredentialKey(ssid, cred, chunk), buff, sz) ? sz : 0;
}

bool WirelessConfigurationMgr::GetCredentialSizes(int ssid, CredentialsInfo& sizes)
{
	return GetKV(GetCredentialKey(ssid, CREDS_SIZES_IDX), &sizes, sizeof(sizes));
}

bool WirelessConfigurationMgr::EraseCredentials(int ssid)
{
	bool res = ResetIfCredentialsLoaded(ssid);

	if (res)
	{
		CredentialsInfo sizes;
		memset(&sizes, 0, sizeof(sizes));

		// The blob might not exist yet
		if (GetKV(GetCredentialKey(ssid, CREDS_SIZES_IDX), &sizes, sizeof(sizes)))
		{
			const uint32_t *sizesArr = reinterpret_cast<const uint32_t*>(&sizes);

			for(int cred = 0; cred < sizeof(CredentialsInfo)/ sizeof(sizesArr[0]); cred++)
			{
				int chunks = (sizesArr[cred] + (MaxCredentialChunkSize - 1)) / MaxCredentialChunkSize;
				for(int chunk = chunks - 1; res && chunk >= 0; chunk--)
				{
					res = DeleteKV(GetCredentialKey(ssid, cred, chunk));
				}
			}

			if (res)
			{
				// Only delete the sizes chunk once all the chunks have been deleted
				res = DeleteKV(GetCredentialKey(ssid, CREDS_SIZES_IDX));
			}
		}
	}

	return res;
}

bool WirelessConfigurationMgr::ResetIfCredentialsLoaded(int ssid)
{
	bool res = false;

	uint32_t loadedSsid = 0;
	res = GetKV(GetScratchKey(LOADED_SSID_ID), &loadedSsid, sizeof(loadedSsid));

	if (res) // loadedSsid value has to be valid
	{
		if (loadedSsid == ssid)
		{
			loadedSsid = 0;
			res = SetKV(GetScratchKey(LOADED_SSID_ID), &loadedSsid, sizeof(loadedSsid));
		}
		else
		{
			res = true;
		}
	}

	return res;
}

bool WirelessConfigurationMgr::IsSsidBlank(const WirelessConfigurationData& data)
{
	return (data.ssid[0] == 0xFF);
}

int WirelessConfigurationMgr::FindEmptySsidEntry()
{
	for (int i = MaxRememberedNetworks; i >= 0; i--)
	{
		WirelessConfigurationData d;
		if (GetSsid(i, d) && IsSsidBlank(d)
			&& (!pendingSsid || pendingSsid->ssid != i)
		)
		{
			return i;
		}
	}

	return -1;
}