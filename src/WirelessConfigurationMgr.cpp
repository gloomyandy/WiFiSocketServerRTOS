#include "WirelessConfigurationMgr.h"

#include <cstring>

WirelessConfigurationMgr* WirelessConfigurationMgr::instance = nullptr;

void WirelessConfigurationMgr::Init()
{
	/**
	 * This class manages two partitions: a credential scratch partition and
	 * the key-value store partition.
	 *
	 * The scratch partition is a raw partition that provides the required contiguous memory
	 * for enterprise network credentials, which can get huge. These credentials are loaded and assembled
	 * from the key-value store in a wear-leveled fashion.
	 *
	 * The key-value store uses the SDK's NVS mechanism. They are used to store wireless configuration data,
	 * the credential chunks, and some other bits and pieces. There are three classes of namespace:
	 * 		- ssids - stores wireless configuration data
	 * 		- creds_xx - stores credential for a particular wireless config data stored in 'ssids', where
	 * 					xx is the index/slot number
	 * 		- scratch - stores some values related to the scratch partition.
	 **/

	nvs_flash_init_partition(SSIDS_STORAGE_NAME);
	nvs_open_from_partition(SSIDS_STORAGE_NAME, SSIDS_STORAGE_NAME, NVS_READWRITE, &ssidsStorage);

#if SUPPORT_WPA2_ENTERPRISE
	nvs_open_from_partition(SSIDS_STORAGE_NAME, SCRATCH_STORAGE_NAME, NVS_READWRITE, &scratchStorage);
	credsScratch = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, SCRATCH_STORAGE_NAME);
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

#if SUPPORT_WPA2_ENTERPRISE
	// Memory map the partition. The base pointer will be returned.
	spi_flash_mmap_handle_t  mapHandle;
	esp_partition_mmap(credsScratch, 0, credsScratch->size, SPI_FLASH_MMAP_DATA, reinterpret_cast<const void**>(&scratchBase), &mapHandle);

	// Storing an enterprise SSID might not have gone all the way.
	// The SSID information is written last, after all of the credentials;
	// so there might be orphaned credentials taking up space. Clear them here.
	WirelessConfigurationData data;

	for(int i = 1; i < MaxRememberedNetworks; i++)
	{
		GetSsid(i, data);
		if (data.ssid[0] == 0xFF)
		{
			EraseCredentials(i);
		}
	}
#endif
}

void WirelessConfigurationMgr::Clear()
{
	/**
	 * Clear storage and reset values to default.
	 * 	- the credentials scratch partition must be erased
	 * 	- the SSID key-value storage namespace must be cleared, and each slot must be reset to a blank value
	 *  - the scratch key-value storage namespace must be cleared
	 *  - the credential key-value storage namespace for each SSID must be cleared
	 **/
	nvs_erase_all(ssidsStorage);

#if SUPPORT_WPA2_ENTERPRISE
	esp_partition_erase_range(credsScratch, 0, credsScratch->size);
	nvs_erase_all(scratchStorage);
#endif

	for (int i = MaxRememberedNetworks; i >= 0; i--)
	{
		EraseSsid(i);
	}
}


std::string WirelessConfigurationMgr::GetSsidKey(int ssid)
{
	return std::to_string(ssid).c_str();
}

int WirelessConfigurationMgr::FindEmptySsidEntry()
{
	for (int i = MaxRememberedNetworks; i >= 0; i--)
	{
		WirelessConfigurationData d;
		if (GetSsid(i, d) && d.ssid[0] == 0xFF)
		{
			return i;
		}
	}

	return -1;
}

bool WirelessConfigurationMgr::SetSsidData(int ssid, const WirelessConfigurationData& data)
{
	if (ssid <= MaxRememberedNetworks) {
		esp_err_t res = nvs_set_blob(ssidsStorage, GetSsidKey(ssid).c_str(), &data, sizeof(data));

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

int WirelessConfigurationMgr::SetSsid(const WirelessConfigurationData& data, bool ap = false)
{
	WirelessConfigurationData d;
	int ssid = GetSsid(data.ssid, d);

	if (ssid < 0)
	{
		ssid = FindEmptySsidEntry();
		if (ssid == 0 && !ap) { // reserved for AP details
			ssid = -1;
		}
	}

	if (ssid >= 0)
	{
		SetSsidData(ssid, data);
	}

	return ssid;
}

bool WirelessConfigurationMgr::EraseSsid(int ssid)
{
	if (ssid >= 0)
	{
		EraseSsidData(ssid);

#if SUPPORT_WPA2_ENTERPRISE
		EraseCredentials(ssid);
#endif

		return true;
	}

	return false;
}

bool WirelessConfigurationMgr::EraseSsid(const char *ssid)
{
	WirelessConfigurationData temp;
	return EraseSsid(GetSsid(ssid, temp));
}

bool WirelessConfigurationMgr::GetSsid(int ssid, WirelessConfigurationData& data)
{
	if (ssid <= MaxRememberedNetworks) {
		size_t sz = sizeof(data);
		esp_err_t res = nvs_get_blob(ssidsStorage, GetSsidKey(ssid).c_str(), &data, &sz);
		return (res == ESP_OK) && (sz == sizeof(data));
	}

	return false;
}

int WirelessConfigurationMgr::GetSsid(const char *ssid, WirelessConfigurationData& data)
{
	for (int i = MaxRememberedNetworks; i >= 0; i--)
	{
		WirelessConfigurationData d;
		if (GetSsid(i, d) && strncmp(ssid, d.ssid, sizeof(d.ssid)) == 0)
		{
			data = d;
			return i;
		}
	}
	return -1;
}

#if SUPPORT_WPA2_ENTERPRISE
nvs_handle_t WirelessConfigurationMgr::OpenCredentialStorage(int ssid, bool write)
{
	char ssidCredsNs[NVS_KEY_NAME_MAX_SIZE] = { 0 };
	snprintf(ssidCredsNs, sizeof(ssidCredsNs), CREDS_STORAGE_NAME, ssid);

	nvs_handle_t ssidCreds;
	nvs_open_from_partition(SSIDS_STORAGE_NAME, ssidCredsNs, write ? NVS_READWRITE : NVS_READONLY, &ssidCreds);

	return ssidCreds;
}

std::string WirelessConfigurationMgr::GetCredentialKey(int cred, int chunk)
{
	// Key is in the form "xx_yy" where x is the credential id
	// and yy is the chunk no.
	std::string key = std::to_string(cred);
	key.append("_");
	key.append(std::to_string(chunk));
	return key;
}

bool WirelessConfigurationMgr::SetCredential(int ssid, int cred, int chunk, const void* buff, size_t sz)
{
	ResetIfCredentialsLoaded(ssid);

	nvs_handle_t creds = OpenCredentialStorage(ssid, true);
	esp_err_t err = nvs_set_blob(creds, GetCredentialKey(cred, chunk).c_str(), buff, sz);
	if (err == ESP_OK) {
		err = nvs_commit(ssidsStorage);
	}
	nvs_close(creds);

	return err == ESP_OK;
}

size_t WirelessConfigurationMgr::GetCredential(int ssid, int cred, int chunk, void* buff, size_t sz)
{
	nvs_handle_t creds = OpenCredentialStorage(ssid, false);
	nvs_get_blob(creds, GetCredentialKey(cred, chunk).c_str(), buff, &sz);
	nvs_close(creds);
	return sz;
}

void WirelessConfigurationMgr::ResetIfCredentialsLoaded(int ssid)
{
	uint32_t loadedSsid = 0;
	nvs_get_u32(scratchStorage, LOADED_SSID_KEY, &loadedSsid);

	if (loadedSsid == ssid)
	{
		nvs_set_u32(scratchStorage, LOADED_SSID_KEY, 0);
		nvs_commit(scratchStorage);
	}
}

const uint8_t* WirelessConfigurationMgr::GetEnterpriseCredentials(int ssid, const CredentialsInfo& sizes, CredentialsInfo& offsets)
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
					esp_partition_write(credsScratch, baseOffset + offset, buff, sz);

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
	ResetIfCredentialsLoaded(ssid);
	nvs_handle_t creds = OpenCredentialStorage(ssid, true);
	nvs_erase_all(creds);
	nvs_commit(creds);
	nvs_close(creds);
}

bool WirelessConfigurationMgr::BeginEnterpriseSsid(const WirelessConfigurationData &data)
{
	pendingEnterpriseSsidData = static_cast<WirelessConfigurationData*>(calloc(1, sizeof(data)));
	memcpy(pendingEnterpriseSsidData, &data, sizeof(data));

	// Personal network assumed unless otherwise stated. PSK is indicated by WirelessConfigurationData::eap.protocol == 1,
	// which is the null terminator for the pre-shared key. Enforce that here.
	static_assert(offsetof(WirelessConfigurationData, eap.protocol) ==
					offsetof(WirelessConfigurationData,
							password[sizeof(pendingEnterpriseSsidData->password) - sizeof(pendingEnterpriseSsidData->eap.protocol)]));

	WirelessConfigurationData stored;
	pendingEnterpriseSsid = GetSsid(pendingEnterpriseSsidData->ssid, stored);

	if (pendingEnterpriseSsid < 0)
	{
		pendingEnterpriseSsid = FindEmptySsidEntry();
	}

	if (pendingEnterpriseSsid > 0)
	{
		EraseCredentials(pendingEnterpriseSsid);
		return true;
	}

	return false;
}

bool WirelessConfigurationMgr::SetEnterpriseCredential(int cred, const void* buff, size_t size)
{
	uint32_t *credsSizes = reinterpret_cast<uint32_t*>(&(pendingEnterpriseSsidData->eap.credsSizes));

	if (SetCredential(pendingEnterpriseSsid, cred, credsSizes[cred]/MaxCredentialChunkSize, buff, size))
	{
		credsSizes[cred] += size;
		return true;
	}

	return false;
}

bool WirelessConfigurationMgr::EndEnterpriseSsid()
{
	bool res = SetSsidData(pendingEnterpriseSsid, *pendingEnterpriseSsidData);

	free(pendingEnterpriseSsidData);
	pendingEnterpriseSsidData = nullptr;
	pendingEnterpriseSsid = -1;

	return res;
}
#endif