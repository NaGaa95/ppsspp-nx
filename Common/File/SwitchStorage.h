#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace SwitchStorage
{
struct SmbShare
{
	std::string id;
	std::string name;
	std::string server;
	std::string share;
	std::string path;
	std::string user;
	std::string password;
	std::string domain;
	bool autoMount = true;
};

struct Location
{
	std::string id;
	std::string path;
	std::string label;
	std::string mountAlias;
	std::string physicalId;
	std::string serialNumber;
	std::uint16_t vendorId{};
	std::uint16_t productId{};
	std::uint8_t lun{};
	std::uint32_t partition{};
	std::uint8_t filesystemType{};
	std::uint64_t capacity{};
};

enum class SmbConnectionState
{
	Disconnected,
	Connecting,
	Connected,
	Reconnecting,
	Failed,
};

struct UsbSnapshot
{
	std::uint64_t generation{};
	std::vector<Location> locations;
};

using UsbStatusCallback = void (*)(void* userData);

bool InitializeUsb(std::string* error = nullptr);
uint64_t UsbStatusGeneration();
void SetUsbStatusCallback(UsbStatusCallback callback, void* userData = nullptr);
UsbSnapshot GetUsbSnapshot();
std::string ResolveUsbPath(const std::string& id);
bool SafelyEjectUsb(const std::string& id, std::string* error = nullptr);
bool GetCachedSmbStat(const std::string& path, struct stat* output);
bool MountSmb(const SmbShare& share, std::string* error = nullptr,
              const std::atomic_bool* cancel = nullptr);
bool UnmountSmb(const std::string& id);
bool IsSmbMounted(const std::string& id);
SmbConnectionState GetSmbConnectionState(const std::string& id);
bool ReconnectSmb(const std::string& id, std::string* error = nullptr,
                  const std::atomic_bool* cancel = nullptr);

std::string SmbRootPath(const std::string& id);
std::string SmbBrowsePath(const SmbShare& share);
std::vector<Location> ListUsbLocations();
void Shutdown();
} // namespace SwitchStorage
