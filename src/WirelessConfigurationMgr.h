
/*
 * Socket.h
 *
 * Class for managing WiFi access point configuration and credentials.
 */

#ifndef SRC_WIFI_CONFIGURATION_MANAGER_H_
#define SRC_WIFI_CONFIGURATION_MANAGER_H_

#include <string>
#include <stdint.h>
#include <stdlib.h>

#include "include/MessageFormats.h"

#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"

class WirelessConfigurationMgr
{
public:
	static WirelessConfigurationMgr* GetInstance()
	{
		if (!instance)
		{
			instance = new WirelessConfigurationMgr();
		}
		return instance;
	}
	void Init();
	void Clear();

	bool GetSsidDataByIndex(int ssid, WirelessConfigurationData& data);
	int GetSsidDataByName(const char* ssid, WirelessConfigurationData& data);
	const uint8_t* LoadCredentials(int ssid, const CredentialsInfo& sizes, CredentialsInfo& offsets);
	int FindEmptySsidEntry();
	bool SetSsidData(int ssid, const WirelessConfigurationData& data);
	void EraseCredentials(int ssid);
	bool SetCredential(int ssid, int cred, int chunk, const void* buff, size_t sz);
	bool EraseSsidData(int ssid);
private:
	std::string GetCredentialKey(int cred, int chunk);
	nvs_handle_t OpenCredentialStore(int ssid, bool write);
	size_t GetCredential(int ssid, int cred, int chunk, void* buff, size_t sz);

	static WirelessConfigurationMgr* instance;

	static constexpr char SSIDS_STORAGE_NAME[] = "ssids";
	static constexpr char CREDS_STORAGE_NAME[] = "creds_%d";
	static constexpr char SCRATCH_STORAGE_NAME[] = "scratch";

	static constexpr char SCRATCH_OFFSET_KEY[] = "offset";

	nvs_handle_t ssidsStorage;
	nvs_handle_t scratchStorage;

	const esp_partition_t* credsScratch;

	const uint8_t* scratchBase;
};

#endif /* SRC_WIFI_CONFIGURATION_MANAGER_H_ */