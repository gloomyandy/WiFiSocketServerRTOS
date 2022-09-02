#include "WirelessConfigurationMgr.h"

#include <cstdlib>
#include <cstring>
#include <sys/fcntl.h>
#include <sys/unistd.h>

#include "esp_spiffs.h"

#include "Config.h"

#if ESP8266
#include "esp8266/partition.h"
#endif

WirelessConfigurationMgr* WirelessConfigurationMgr::instance = nullptr;

static inline uint32_t round2SecSz(uint32_t val)
{
	static_assert(SPI_FLASH_SEC_SIZE && ((SPI_FLASH_SEC_SIZE & (SPI_FLASH_SEC_SIZE - 1)) == 0));
	// If val is already a multiple of SPI_FLASH_SEC_SIZE, return itself;
	// else return next multiple.
	return (val + (SPI_FLASH_SEC_SIZE - 1)) & ~(SPI_FLASH_SEC_SIZE - 1);
}

void WirelessConfigurationMgr::Init()
{
	// This class manages two partitions: a credential scratch partition and
	// the key-value storage (KVS) partition.
	//
	// The scratch partition is a raw partition that provides the required contiguous memory
	// for enterprise network credentials. Credentials stored in the KVS are copied to this
	// partition before being passed to ESP WPA2 enterprise APIs.
	//
	// The key-value storage partition uses SPIFFS. It is used to store wireless configuration data,
	// the credentials, and some other bits and pieces. There are three main 'directories':
	// 		- ssids - stores wireless configuration data, with key/path 'ssids/xx' where xx is the ssid slot
	// 		- creds - stores credential for a particular wireless config data stored in 'ssids', with key/path
	// 					'creds/xx/yy', where xx is the ssid slot, yy is the credential index
	// 		- scratch - stores some values related to the scratch partition, with key/path 'scratch/ss' where
	// 					ss is the string id
	//
	esp_vfs_spiffs_conf_t conf = {
		.base_path = KVS_PATH,
		.partition_label = NULL,
		.max_files = 1,
		.format_if_mount_failed = true
	};

	esp_vfs_spiffs_register(&conf);

	// Memory map the partition, remembering the base pointer for the lifetime of the app.
	spi_flash_mmap_handle_t mapHandle;
	scratchPartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, SCRATCH_DIR);
	esp_partition_mmap(scratchPartition, 0, scratchPartition->size, SPI_FLASH_MMAP_DATA,
						reinterpret_cast<const void**>(&scratchBase), &mapHandle);

	char key[MAX_KEY_LEN] = { 0 };

	// Check if first time and the storage should be initialized. The marker here is SSID slot 0,
	// since WirelessConfigurationMgr::Reset works it's way backwards to it.
	if (!GetKV(GetSsidKey(key, 0), nullptr, 0))
	{
		debugPrintf("initializing SSID storage...");
		Reset();

		// Restore SSID info from old, 1.x firmware, if the partition exists
		const esp_partition_t* oldSsids = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
			ESP_PARTITION_SUBTYPE_DATA_NVS, "ssids_old");

		if (oldSsids) {
			debugPrintf("restoring old SSID...");

			for (int ssid = MaxRememberedNetworks; ssid >= 0; ssid--) {
				WirelessConfigurationData temp;
				esp_partition_read(oldSsids, ssid * sizeof(temp), &temp, sizeof(temp));

				if (temp.ssid[0] != 0xFF) {
					SetSsidData(ssid, temp);
				}
			}
		}
		debugPrintf("done!\n");
	}

#if ESP32C3
	esp_spiffs_check(NULL);
#endif

	// Storing an enterprise SSID and its credentials might not have
	// gone all the way. Since credentials are stored first before the
	// SSID data, if credentials are incompletely stored due to a power loss,
	// we can detect and clean up those orphaned credentials here.
	for (int ssid = MaxRememberedNetworks; ssid > 0; ssid--)
	{
		WirelessConfigurationData temp;
		if ((GetSsid(ssid, temp) && IsSsidBlank(temp)))
		{
			DeleteCredentials(ssid);
		}
	}
}


void WirelessConfigurationMgr::Reset(bool format)
{
	if (format)
	{
		esp_spiffs_format(NULL);
	}

	// Reset storage and reset values to default.
	//  - clear the scratch partition, and the associated scratch
	//		key-value pairs
	// 	- the SSID slot must be reset to a blank value, and credentials for
	// 		each slot must be cleared
	//
	// Work down to SSID slot 0, since it is used to detect whether
	// the KVS has been initialized for the first time.
	ResetScratch();

	for (int ssid = MaxRememberedNetworks; ssid >= 0; ssid--)
	{
		// Erase the SSID first, then the credentials. This is because if
		// erasure of credentials is incomplete, if the corresponding
		// SSID has been cleared first, then it can be erased at startup
		EraseSsid(ssid);
		DeleteCredentials(ssid);
	}
}

int WirelessConfigurationMgr::SetSsid(const WirelessConfigurationData& data, bool ap = false)
{
	WirelessConfigurationData temp;
	memset(&temp, 0, sizeof(temp));

	int ssid = WirelessConfigurationMgr::AP;

	if (!ap)
	{
		ssid = GetSsid(data.ssid, temp);

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
		// This might previously be an enterprise ssid, delete
		// its credentials here.
		if (temp.eap.protocol == EAPProtocol::NONE || (EraseSsid(ssid) && DeleteCredentials(ssid)))
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
	if (ResetIfCredentialsLoaded(ssid))
	{
		if (EraseSsidData(ssid))
		{
			return true;
		}
	}

	return false;
}

bool WirelessConfigurationMgr::EraseSsid(const char *ssid)
{
	WirelessConfigurationData temp;
	return EraseSsid(GetSsid(ssid, temp));
}

bool WirelessConfigurationMgr::GetSsid(int ssid, WirelessConfigurationData& data) const
{
	char key[MAX_KEY_LEN] = { 0 };
	return GetKV(GetSsidKey(key, ssid), &data, sizeof(data));
}

int WirelessConfigurationMgr::GetSsid(const char *ssid, WirelessConfigurationData& data) const
{
	if (ssid)
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

	// Check if the credentials will fit
	size_t total = 0;

	for (uint32_t sz : data.eap.credSizes.asArr)
	{
		total += sz;
	}

	if (total < GetFree() && total < scratchPartition->size)
	{
		WirelessConfigurationData temp;
		int ssid = GetSsid(data.ssid, temp);

		if (ssid < 0)
		{
			ssid = FindEmptySsidEntry();
		}

		if (ssid > 0)
		{
			if (EraseSsid(ssid))
			{
				pendingSsid = static_cast<PendingEnterpriseSsid*>(calloc(1, sizeof(PendingEnterpriseSsid)));
				if (pendingSsid)
				{
					pendingSsid->data = data;
					pendingSsid->ssid = ssid;
					return true;
				}
			}
		}
	}

	return false;
}

bool WirelessConfigurationMgr::SetEnterpriseCredential(int cred, const void* buff, size_t size)
{
	if (pendingSsid)
	{
		size_t newSize = pendingSsid->sizes.asArr[cred] + size;

		if (newSize <= pendingSsid->data.eap.credSizes.asArr[cred])
		{
			char key[MAX_KEY_LEN] = { 0 };
			if (SetKV(GetCredentialKey(key, pendingSsid->ssid, cred), buff, size, pendingSsid->sizes.asArr[cred]))
			{
				pendingSsid->sizes.asArr[cred] = newSize;
				return true;
			}
		}
	}

	return false;
}

bool WirelessConfigurationMgr::EndEnterpriseSsid(bool cancel)
{
	bool ok = cancel;

	if (pendingSsid)
	{
		if (cancel)
		{
			DeleteCredentials(pendingSsid->ssid);
		}
		else
		{
			// Make sure that the sizes sent at the beginning matches
			// what we have received.
			ok = true;

			for (int cred = 0; ok && cred < ARRAY_SIZE(pendingSsid->sizes.asArr); cred++)
			{
				ok = ((pendingSsid->data.eap.credSizes.asArr[cred] == pendingSsid->sizes.asArr[cred]));

				if (ok && !pendingSsid->sizes.asArr[cred])
				{
					ok = DeleteCredential(pendingSsid->ssid, cred);
				}
			}

			if (ok)
			{
				ok = SetSsidData(pendingSsid->ssid, pendingSsid->data);
			}
			else
			{
				DeleteCredentials(pendingSsid->ssid);
			}
		}

		free(pendingSsid);
		pendingSsid = nullptr;
	}

	return ok;
}

const uint8_t* WirelessConfigurationMgr::GetEnterpriseCredentials(int ssid, const CredentialsInfo& sizes, CredentialsInfo& offsets)
{
	const uint8_t *res = nullptr;

	// Check that all information needed for the proceeding operations can
	// be retrieved.
	uint32_t loadedSsid = 0, baseOffset = 0;

	char key[MAX_KEY_LEN] = { 0 };

	if (GetKV(GetScratchKey(key, LOADED_SSID_ID), &loadedSsid, sizeof(loadedSsid)) &&
		GetKV(GetScratchKey(key, SCRATCH_OFFSET_ID), &baseOffset, sizeof(baseOffset)))
	{
		// Get the total size of credentials
		size_t totalSize = 0;

		for (uint32_t sz: sizes.asArr)
		{
			totalSize += sz;
		}

		// Erasing the flash storage has to be in multiples of SPI_FLASH_SEC_SZ.
		size_t actualSize = round2SecSz(totalSize);

		// If the SSID has already been loaded, just return the existing pointer
		// and compute offsets.asMemb. If not, load it in the scratch partition.
		if (loadedSsid == ssid)
		{
			for(int cred = 0, offset = 0; cred < ARRAY_SIZE(offsets.asArr); cred++)
			{
				offsets.asArr[cred] = offset;
				offset += sizes.asArr[cred];
			}

			res = (scratchBase + baseOffset - actualSize);
		}
		else
		{
			if (baseOffset + actualSize > scratchPartition->size)
			{
				baseOffset = 0;
			}

			uint32_t newOffset = baseOffset + actualSize;
			bool ok = SetKV(GetScratchKey(key, SCRATCH_OFFSET_ID), &newOffset, sizeof(newOffset));

			if (ok)
			{
				ok = (esp_partition_erase_range(scratchPartition, baseOffset, actualSize) == ESP_OK);

				if (ok)
				{
					// Store offsets from the base offset
					uint8_t *buff = static_cast<uint8_t*>(malloc(MaxCredentialChunkSize));

					for(int cred = 0, offset = 0; ok && cred < ARRAY_SIZE(offsets.asArr); cred++)
					{
						offsets.asArr[cred] = offset;

						for(int sz = 0, pos = 0, remain = sizes.asArr[cred];
							ok && remain > 0; remain -= sz, offset += sz, pos += sz)
						{
							memset(buff, 0, MaxCredentialChunkSize);

							sz = (remain >= MaxCredentialChunkSize) ? MaxCredentialChunkSize : remain;
							ok = GetKV(GetCredentialKey(key, ssid, cred), buff, sz, pos);

							if (ok)
							{
								ok = (esp_partition_write(scratchPartition, baseOffset + offset, buff, sz) == ESP_OK);
							}
						}
					}

					free(buff);

					if (ok)
					{
						loadedSsid = ssid;
						ok = SetKV(GetScratchKey(key, LOADED_SSID_ID), &loadedSsid, sizeof(loadedSsid));

						if (ok)
						{
							res = scratchBase + baseOffset;
						}
					}
				}
			}
		}
	}

	return res;
}

bool WirelessConfigurationMgr::DeleteKV(const char *key)
{
	if (key)
	{
		return (remove(key) == 0);
	}

	return false;
}

bool WirelessConfigurationMgr::SetKV(const char *key, const void *buff, size_t sz, bool append)
{
	if (key && buff && sz)
	{
		int f = open(key, O_WRONLY | (append ? O_APPEND : O_CREAT | O_TRUNC));

		if (f >= 0)
		{
			size_t written = write(f, buff, sz);
			close(f);
			return written == sz;
		}
	}

	return false;
}

bool WirelessConfigurationMgr::GetKV(const char *key, void* buff, size_t sz, size_t pos) const
{
	if (key)
	{
		int f = open(key, O_RDONLY);

		if (f >= 0)
		{
			bool res = true;

			// If buff == NULL or sz == 0, this command is only used to check
			// if the particular key exists. Therefore, do nothing
			// but close the opened file.
			if (buff && sz)
			{
				res = (lseek(f, pos, SEEK_SET) == pos);

				if (res)
				{
					res = (read(f, buff, sz) == sz);
				}
			}

			close(f);
			return res;
		}
	}

	return false;
}

size_t WirelessConfigurationMgr::GetFree()
{
	size_t total = 0, used = 0;
	esp_err_t ret = esp_spiffs_info(NULL, &total, &used);
	return (ret == ESP_OK && used < total) ? total - used : 0;
}

const char* WirelessConfigurationMgr::GetSsidKey(char *buff, int ssid)
{
	int res = 0;

	if (buff && ssid >= 0 && ssid <= MaxRememberedNetworks)
	{
		res = snprintf(buff, MAX_KEY_LEN, "%s/%s/%d", KVS_PATH, SSIDS_DIR, ssid);
	}

	return (res > 0 && res < MAX_KEY_LEN) ? buff : nullptr;
}

bool WirelessConfigurationMgr::SetSsidData(int ssid, const WirelessConfigurationData& data)
{
	char key[MAX_KEY_LEN] = { 0 };
	return SetKV(GetSsidKey(key, ssid), &data, sizeof(data));
}

bool WirelessConfigurationMgr::EraseSsidData(int ssid)
{
	WirelessConfigurationData clean;
	memset(&clean, 0xFF, sizeof(clean));
	return SetSsidData(ssid, clean);
}

const char* WirelessConfigurationMgr::GetScratchKey(char *buff, int id)
{
	int res = 0;

	if (buff && id >= 0)
	{
		res = snprintf(buff, MAX_KEY_LEN, "%s/%s/%d", KVS_PATH, SCRATCH_DIR, id);
	}

	return (res > 0 && res < MAX_KEY_LEN) ? buff : nullptr;
}

bool WirelessConfigurationMgr::ResetScratch()
{
	esp_err_t err = esp_partition_erase_range(scratchPartition, 0, scratchPartition->size);

	if (err == ESP_OK)
	{
		char key[MAX_KEY_LEN] = { 0 };
		uint32_t zero = 0;
		return SetKV(GetScratchKey(key, LOADED_SSID_ID), &zero, sizeof(zero)) &&
				SetKV(GetScratchKey(key, SCRATCH_OFFSET_ID), &zero, sizeof(zero));
	}

	return false;
}

const char* WirelessConfigurationMgr::GetCredentialKey(char *buff, int ssid, int cred)
{
	int res = 0;

	if (buff && (ssid >= 0 && ssid <= MaxRememberedNetworks) &&
		(cred >= 0 && cred < ARRAY_SIZE(pendingSsid->sizes.asArr)))
	{
		res = snprintf(buff, MAX_KEY_LEN, "%s/%s/%d/%d", KVS_PATH, CREDS_DIR, ssid, cred);
	}

	return (res > 0 && res < MAX_KEY_LEN) ? buff : nullptr;
}

bool WirelessConfigurationMgr::DeleteCredentials(int ssid)
{
	bool res = true;

	for (int cred = 0; res && cred < ARRAY_SIZE(pendingSsid->sizes.asArr); cred++)
	{
		res = DeleteCredential(ssid, cred);
	}

	return res;
}

bool WirelessConfigurationMgr::DeleteCredential(int ssid, int cred)
{
	char key[MAX_KEY_LEN] = { 0 };
	return !GetKV(GetCredentialKey(key, ssid, cred), nullptr, 0) || DeleteKV(key);
}

bool WirelessConfigurationMgr::ResetIfCredentialsLoaded(int ssid)
{
	bool res = false;

	if (ssid >= 0 && ssid <= MaxRememberedNetworks)
	{
		char key[MAX_KEY_LEN] = { 0 };

		uint32_t loadedSsid = 0;
		res = GetKV(GetScratchKey(key, LOADED_SSID_ID), &loadedSsid, sizeof(loadedSsid));

		if (res) // loadedSsid value has to be valid
		{
			if (loadedSsid == ssid) // if the ssid in question is not loaded, do nothing
			{
				loadedSsid = 0;
				res = SetKV(GetScratchKey(key, LOADED_SSID_ID), &loadedSsid, sizeof(loadedSsid));
			}
		}
	}

	return res;
}

bool WirelessConfigurationMgr::IsSsidBlank(const WirelessConfigurationData& data)
{
	return (data.ssid[0] == 0xFF);
}

int WirelessConfigurationMgr::FindEmptySsidEntry() const
{
	for (int ssid = MaxRememberedNetworks; ssid >= 0; ssid--)
	{
		WirelessConfigurationData data;
		if (GetSsid(ssid, data) && IsSsidBlank(data)
			&& (!pendingSsid || pendingSsid->ssid != ssid)
		)
		{
			return ssid;
		}
	}

	return -1;
}