
/*
 * WirelessConfigurationMgr.h
 *
 * Manages the storage of Wi-Fi station/AP configuration and credentials.
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


#define SUPPORT_WPA2_ENTERPRISE			(ESP32C3)

class WirelessConfigurationMgr
{
public:
	static constexpr int AP = 0;

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

	int SetSsid(const WirelessConfigurationData& data, bool ap);
	bool EraseSsid(const char *ssid);
	bool GetSsid(int ssid, WirelessConfigurationData& data);
	int GetSsid(const char* ssid, WirelessConfigurationData& data);

#if SUPPORT_WPA2_ENTERPRISE
	bool BeginEnterpriseSsid(const WirelessConfigurationData &data);
	bool SetEnterpriseCredential(int cred, const void* buff, size_t size);
	bool EndEnterpriseSsid();
	const uint8_t* GetEnterpriseCredentials(int ssid, const CredentialsInfo& sizes, CredentialsInfo& offsets);
#endif
private:

#if SUPPORT_WPA2_ENTERPRISE
	WirelessConfigurationMgr() : pendingEnterpriseSsid(-1) {};
#endif

	static constexpr char SSIDS_STORAGE_NAME[] = "ssids";
#if SUPPORT_WPA2_ENTERPRISE
	static constexpr char CREDS_STORAGE_NAME[] = "creds_%d";
	static constexpr char SCRATCH_STORAGE_NAME[] = "scratch";

	static constexpr char SCRATCH_OFFSET_KEY[] = "offset";
	static constexpr char LOADED_SSID_KEY[] = "ssid";
#endif

	static WirelessConfigurationMgr* instance;

	nvs_handle_t ssidsStorage;
#if SUPPORT_WPA2_ENTERPRISE
	nvs_handle_t scratchStorage;
	const esp_partition_t* credsScratch;

	const uint8_t* scratchBase;
	WirelessConfigurationData *pendingEnterpriseSsidData;
	int pendingEnterpriseSsid;
#endif

	int FindEmptySsidEntry();

	bool SetSsidData(int ssid, const WirelessConfigurationData& data);
	bool SetCredential(int ssid, int cred, int chunk, const void* buff, size_t sz);
	bool EraseSsidData(int ssid);
	bool EraseSsid(int ssid);
	std::string GetSsidKey(int ssid);

#if SUPPORT_WPA2_ENTERPRISE
	std::string GetCredentialKey(int cred, int chunk);
	nvs_handle_t OpenCredentialStorage(int ssid, bool write);
	size_t GetCredential(int ssid, int cred, int chunk, void* buff, size_t sz);
	void EraseCredentials(int ssid);

	void ResetIfCredentialsLoaded(int ssid);
#endif
};

#endif /* SRC_WIFI_CONFIGURATION_MANAGER_H_ */