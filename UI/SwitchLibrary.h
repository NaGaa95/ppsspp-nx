#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Common/File/Path.h"
#include "Common/File/SwitchStorage.h"

namespace SwitchLibrary {

enum class SourceType {
	Local,
	Usb,
	Smb,
};

struct Source {
	SourceType type = SourceType::Local;
	std::string id;
	std::string path;
};

void Initialize();
void Shutdown();

std::vector<Source> GetSources();
std::string ResolveSource(const Source &source, bool connectSmb = false, std::string *error = nullptr);
std::string DescribeSource(const Source &source);
bool AddSource(const Path &path);
void RemoveSource(size_t index);

std::vector<SwitchStorage::SmbShare> GetSmbShares();
bool SaveSmbShare(const SwitchStorage::SmbShare &share, std::string *error = nullptr);
void RemoveSmbShare(const std::string &id);

void RequestScan();
void RequestScan(size_t sourceIndex);
std::vector<Path> GetGames();
uint64_t Generation();
bool IsScanning();

}  // namespace SwitchLibrary
