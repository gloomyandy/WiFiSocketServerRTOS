
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
    void FactoryReset();

    bool GetSsidDataByIndex(int ssid, WirelessConfigurationData& data);
    int GetSsidDataByName(const char* ssid, WirelessConfigurationData& data);
    const uint8_t* LoadCredentials(int ssid, CredentialsInfo& offsets);
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

    static constexpr char SSIDS_STORAGE_NS[] = "ssids";
    static constexpr char CREDS_STORAGE_NS[] = "creds_%d";

    nvs_handle_t ssidsStorage;
    const esp_partition_t* credsScratch;
};

#endif /* SRC_WIFI_CONFIGURATION_MANAGER_H_ */