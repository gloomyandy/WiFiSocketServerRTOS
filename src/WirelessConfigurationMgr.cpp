#include "WirelessConfigurationMgr.h"

#include <cstring>

WirelessConfigurationMgr* WirelessConfigurationMgr::instance = nullptr;

void WirelessConfigurationMgr::Init()
{
	nvs_flash_init_partition(SSIDS_STORAGE_NAME);
	nvs_open_from_partition(SSIDS_STORAGE_NAME, SSIDS_STORAGE_NAME, NVS_READWRITE, &ssidsStorage);

#if ESP32C3
	credsScratch = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
		ESP_PARTITION_SUBTYPE_DATA_NVS, SCRATCH_STORAGE_NAME);
#endif

	nvs_iterator_t savedSsids = nvs_entry_find(SSIDS_STORAGE_NAME, SSIDS_STORAGE_NAME, NVS_TYPE_ANY);
	if (!savedSsids) {
		Clear();

		// Restore ap info from old firmware
		const esp_partition_t* oldSsids = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
			ESP_PARTITION_SUBTYPE_DATA_NVS, "ssids_old");

		if (oldSsids) {
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
		}
	}
	nvs_release_iterator(savedSsids);

#if ESP32C3
	nvs_open_from_partition(SSIDS_STORAGE_NAME, SCRATCH_STORAGE_NAME, NVS_READWRITE, &scratchStorage);

	spi_flash_mmap_handle_t  mapHandle;
	// Memory map the partition. The base pointer will be returned.
	esp_partition_mmap(credsScratch, 0, credsScratch->size, SPI_FLASH_MMAP_DATA, reinterpret_cast<const void**>(&scratchBase), &mapHandle);

    // Storing an enterprise SSID might not have gone all the way.
    // The SSID information is written last, after all of the credentials;
    // so there might be orphaned credentials taking up space. Clear them here.
    WirelessConfigurationData data;

    for(int i = 1; i < MaxRememberedNetworks; i++)
    {
        GetSsidDataByIndex(i, data);
        if (data.ssid[0] == 0xFF)
        {
            EraseCredentials(i);
        }
    }
#endif
}

// Reset to default settings
void WirelessConfigurationMgr::Clear()
{
	esp_partition_erase_range(credsScratch, 0, credsScratch->size);

	nvs_erase_all(ssidsStorage);
	nvs_erase_all(scratchStorage);

	for (int i = MaxRememberedNetworks; i >= 0; i--)
	{
		EraseSsidData(i);
#if ESP32C3
		EraseCredentials(i);
#endif
	}
}

#if ESP32C3
nvs_handle_t WirelessConfigurationMgr::OpenCredentialStore(int ssid, bool write)
{
	char ssidCredsNs[NVS_KEY_NAME_MAX_SIZE] = { 0 };
	snprintf(ssidCredsNs, sizeof(ssidCredsNs), CREDS_STORAGE_NAME, ssid);

	nvs_handle_t ssidCreds;
	nvs_open_from_partition(SSIDS_STORAGE_NAME, ssidCredsNs, write ? NVS_READWRITE : NVS_READONLY, &ssidCreds);

	return ssidCreds;
}

std::string WirelessConfigurationMgr::GetCredentialKey(int cred, int chunk)
{
	std::string key = std::to_string(cred);
	key.append("_");
	key.append(std::to_string(chunk));
	return key;
}

bool WirelessConfigurationMgr::SetCredential(int ssid, int cred, int chunk, const void* buff, size_t sz)
{
	ResetLoadedSsid(ssid);

	nvs_handle_t creds = OpenCredentialStore(ssid, true);
	esp_err_t err = nvs_set_blob(creds, GetCredentialKey(cred, chunk).c_str(), buff, sz);
	if (err == ESP_OK) {
		err = nvs_commit(ssidsStorage);
	}
	nvs_close(creds);

	return err == ESP_OK;
}

size_t WirelessConfigurationMgr::GetCredential(int ssid, int cred, int chunk, void* buff, size_t sz)
{
	nvs_handle_t creds = OpenCredentialStore(ssid, false);
	nvs_get_blob(creds, GetCredentialKey(cred, chunk).c_str(), buff, &sz);
	nvs_close(creds);
	return sz;
}

void WirelessConfigurationMgr::ResetLoadedSsid(int ssid)
{
	uint32_t loadedSsid = 0;
	nvs_get_u32(scratchStorage, LOADED_SSID_KEY, &loadedSsid);

	if (loadedSsid == ssid)
	{
		nvs_set_u32(scratchStorage, LOADED_SSID_KEY, 0);
		nvs_commit(ssidsStorage);
	}
}

const uint8_t* WirelessConfigurationMgr::LoadCredentials(int ssid, const CredentialsInfo& sizes, CredentialsInfo& offsets)
{
	uint32_t loadedSsid = 0;
	nvs_get_u32(scratchStorage, LOADED_SSID_KEY, &loadedSsid);

	// Read the last offset
	uint32_t baseOffset = 0;
	nvs_get_u32(scratchStorage, SCRATCH_OFFSET_KEY, &baseOffset);

	// Get the total size of credentials
	const uint32_t *sizesArr = reinterpret_cast<const uint32_t*>(&sizes);
	size_t totalSize = 0;
	for(int cred = 0; cred < sizeof(sizes)/sizeof(sizesArr[0]); cred++)
	{
		totalSize += sizesArr[cred];
	}

	if (loadedSsid == ssid)
	{
		const uint32_t *sizesArr = reinterpret_cast<const uint32_t*>(&sizes);
		uint32_t *offsetsArr = reinterpret_cast<uint32_t*>(&offsets);

		for(int cred = 0, offset = 0; cred < sizeof(offsets)/sizeof(offsetsArr[0]); cred++)
		{
			offsetsArr[cred] = offset;
			offset += sizesArr[cred];
		}

		return (scratchBase + baseOffset - totalSize);
	}
	else
	{
		// Increment the offset first. If it will not fit, start from the top
		// again.
		if (baseOffset + totalSize > credsScratch->size)
		{
			baseOffset = 0;
			esp_partition_erase_range(credsScratch, baseOffset, credsScratch->size);
		}

		nvs_set_u32(scratchStorage, SCRATCH_OFFSET_KEY, baseOffset + totalSize);

		// Store offsets from the base offset
		uint32_t *offsetsArr = reinterpret_cast<uint32_t*>(&offsets);
		uint8_t *buff = static_cast<uint8_t*>(malloc(MaxCredentialChunkSize));

		for(int cred = 0, offset = 0; cred < sizeof(offsets)/sizeof(offsetsArr[0]); cred++)
		{
			if (sizesArr[cred])
			{
				offsetsArr[cred] = offset;
				int chunks = sizesArr[cred]/MaxCredentialChunkSize;
				for(int chunk = 0; chunk <= chunks; chunk++)
				{
					memset(buff, 0, MaxCredentialChunkSize);
					size_t sz = GetCredential(ssid, cred, chunk, buff, MaxCredentialChunkSize);
					esp_err_t err = esp_partition_write(credsScratch, baseOffset + offset, buff, sz);

					offset += sz;
				}
			}
		}

		free(buff);

		nvs_set_u32(scratchStorage, LOADED_SSID_KEY, ssid);

		return scratchBase + baseOffset;
	}
}

void WirelessConfigurationMgr::EraseCredentials(int ssid)
{
	// ResetLoadedSsid(ssid);

	nvs_handle_t creds = OpenCredentialStore(ssid, true);
	nvs_erase_all(creds);
	nvs_commit(creds);
	nvs_close(creds);
}
#endif

bool WirelessConfigurationMgr::GetSsidDataByIndex(int idx, WirelessConfigurationData& data)
{
	if (idx <= MaxRememberedNetworks) {
		size_t sz = sizeof(data);
		esp_err_t res = nvs_get_blob(ssidsStorage, std::to_string(idx).c_str(), &data, &sz);
		return (res == ESP_OK) && (sz == sizeof(data));
	}

	return false;
}

// Look up a SSID in our remembered network list, return pointer to it if found
int WirelessConfigurationMgr::GetSsidDataByName(const char* ssid, WirelessConfigurationData& data)
{
	for (int i = MaxRememberedNetworks; i >= 0; i--)
	{
		WirelessConfigurationData d;
		if (GetSsidDataByIndex(i, d) && strncmp(ssid, d.ssid, sizeof(d.ssid)) == 0)
		{
			data = d;
			return i;
		}
	}
	return -1;
}

int WirelessConfigurationMgr::FindEmptySsidEntry()
{
	for (int i = MaxRememberedNetworks; i >= 0; i--)
	{
		WirelessConfigurationData d;
		if (GetSsidDataByIndex(i, d) && d.ssid[0] == 0xFF)
		{
			return i;
		}
	}

	return -1;
}

bool WirelessConfigurationMgr::SetSsidData(int ssid, const WirelessConfigurationData& data)
{
	if (ssid <= MaxRememberedNetworks) {
		esp_err_t res = nvs_set_blob(ssidsStorage, std::to_string(ssid).c_str(), &data, sizeof(data));

		if (res == ESP_OK) {
			res = nvs_commit(ssidsStorage);
		}

		return res == ESP_OK;
	}

	return false;
}

bool WirelessConfigurationMgr::EraseSsidData(int ssid)
{
	uint8_t clean[sizeof(WirelessConfigurationData)];
	memset(clean, 0xFF, sizeof(clean));
	const WirelessConfigurationData& d = *(reinterpret_cast<const WirelessConfigurationData*>(clean));
	return SetSsidData(ssid, d);
}