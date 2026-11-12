// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(SWITCH)

#include "UI/SwitchSteamGridDB.h"

#include <curl/curl.h>

#include <algorithm>
#include <limits>
#include <unordered_set>

#include "Common/Data/Format/JSONReader.h"
#include "Common/Render/ManagedTexture.h"

#ifndef PPSSPP_SWITCH_RELEASE_VERSION
#define PPSSPP_SWITCH_RELEASE_VERSION "1.1.1"
#endif

namespace {

constexpr size_t MAX_RESPONSE_SIZE = 24 * 1024 * 1024;
constexpr char API_ROOT[] = "https://www.steamgriddb.com/api/v2/";

struct DownloadBuffer {
	std::string data;
	const std::atomic<bool> *cancel = nullptr;
};

size_t WriteResponse(void *pointer, size_t size, size_t count, void *userdata) {
	if (count != 0 && size > std::numeric_limits<size_t>::max() / count)
		return 0;
	size_t bytes = size * count;
	auto *buffer = (DownloadBuffer *)userdata;
	if ((buffer->cancel && buffer->cancel->load(std::memory_order_relaxed)) || bytes > MAX_RESPONSE_SIZE - buffer->data.size()) {
		return 0;
	}
	buffer->data.append((const char *)pointer, bytes);
	return bytes;
}

int TransferProgress(void *userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	const auto *cancel = (const std::atomic<bool> *)userdata;
	return cancel && cancel->load(std::memory_order_relaxed) ? 1 : 0;
}

SteamGridDBResult Request(const std::string &url, const std::string &apiKey, const std::atomic<bool> *cancel, std::string *response) {
	if (cancel && cancel->load(std::memory_order_relaxed))
		return SteamGridDBResult::Cancelled;
	CURL *curl = curl_easy_init();
	if (!curl)
		return SteamGridDBResult::NetworkError;
	DownloadBuffer buffer{{}, cancel};
	curl_slist *headers = nullptr;
	if (!apiKey.empty()) {
		std::string authorization = "Authorization: Bearer " + apiKey;
		headers = curl_slist_append(headers, authorization.c_str());
	}
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)MAX_RESPONSE_SIZE);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "PPSSPP-nx/" PPSSPP_SWITCH_RELEASE_VERSION);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, TransferProgress);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
	CURLcode curlResult = curl_easy_perform(curl);
	long responseCode = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (cancel && cancel->load(std::memory_order_relaxed))
		return SteamGridDBResult::Cancelled;
	if (curlResult != CURLE_OK)
		return SteamGridDBResult::NetworkError;
	if (responseCode == 401 || responseCode == 403)
		return SteamGridDBResult::Unauthorized;
	if (responseCode == 404)
		return SteamGridDBResult::NotFound;
	if (responseCode < 200 || responseCode >= 300)
		return SteamGridDBResult::NetworkError;
	*response = std::move(buffer.data);
	return SteamGridDBResult::OK;
}

std::string UrlEncode(const std::string &value) {
	CURL *curl = curl_easy_init();
	if (!curl)
		return {};
	char *encoded = curl_easy_escape(curl, value.c_str(), (int)value.size());
	std::string result = encoded ? encoded : "";
	if (encoded)
		curl_free(encoded);
	curl_easy_cleanup(curl);
	return result;
}

SteamGridDBResult ParseGames(const std::string &response, std::vector<SteamGridDBGame> *games) {
	json::JsonReader reader(response.data(), response.size());
	json::JsonGet root = reader.root();
	const JsonNode *data = root.getArray("data");
	if (!reader.ok() || !data)
		return SteamGridDBResult::InvalidResponse;
	std::unordered_set<int64_t> seen;
	for (const JsonNode *node : data->value) {
		json::JsonGet item = node->value;
		int64_t id = item.getInt("id", 0);
		std::string name;
		if (id <= 0 || !item.getString("name", &name) || name.empty() || !seen.insert(id).second)
			continue;
		games->push_back({id, std::move(name)});
		if (games->size() == 32)
			break;
	}
	return games->empty() ? SteamGridDBResult::NotFound : SteamGridDBResult::OK;
}

void AppendArtworks(const std::string &response, std::vector<SteamGridDBArtwork> *artworks, std::unordered_set<std::string> *seen) {
	json::JsonReader reader(response.data(), response.size());
	json::JsonGet root = reader.root();
	const JsonNode *data = root.getArray("data");
	if (!reader.ok() || !data)
		return;
	for (const JsonNode *node : data->value) {
		json::JsonGet item = node->value;
		SteamGridDBArtwork artwork;
		if (!item.getString("url", &artwork.url) || artwork.url.rfind("https://", 0) != 0 || !seen->insert(artwork.url).second) {
			continue;
		}
		item.getString("thumb", &artwork.thumbnailUrl);
		if (artwork.thumbnailUrl.rfind("https://", 0) != 0)
			artwork.thumbnailUrl = artwork.url;
		artwork.width = item.getInt("width", 0);
		artwork.height = item.getInt("height", 0);
		artworks->push_back(std::move(artwork));
		if (artworks->size() == 64)
			break;
	}
}

}  // namespace

SteamGridDBResult SteamGridDB_Search(const std::string &apiKey, const std::string &title, std::vector<SteamGridDBGame> *games,
									 const std::atomic<bool> *cancel) {
	games->clear();
	if (apiKey.empty())
		return SteamGridDBResult::MissingKey;
	std::string encoded = UrlEncode(title);
	if (encoded.empty())
		return SteamGridDBResult::InvalidResponse;
	std::string response;
	SteamGridDBResult result = Request(std::string(API_ROOT) + "search/autocomplete/" + encoded, apiKey, cancel, &response);
	return result == SteamGridDBResult::OK ? ParseGames(response, games) : result;
}

SteamGridDBResult SteamGridDB_GetIcons(const std::string &apiKey, int64_t gameId, std::vector<SteamGridDBArtwork> *artworks,
									   const std::atomic<bool> *cancel) {
	artworks->clear();
	if (apiKey.empty())
		return SteamGridDBResult::MissingKey;
	if (gameId <= 0)
		return SteamGridDBResult::NotFound;
	std::unordered_set<std::string> seen;
	const std::string id = std::to_string(gameId);
	const std::string endpoints[] = {
		std::string(API_ROOT) + "grids/game/" + id +
			"?dimensions=1024x1024,512x512&types=static&mimes=image/png,image/"
			"jpeg",
		std::string(API_ROOT) + "icons/game/" + id + "?types=static&mimes=image/png,image/jpeg",
	};
	SteamGridDBResult lastResult = SteamGridDBResult::NotFound;
	for (const std::string &endpoint : endpoints) {
		std::string response;
		SteamGridDBResult result = Request(endpoint, apiKey, cancel, &response);
		if (result == SteamGridDBResult::Cancelled || result == SteamGridDBResult::Unauthorized)
			return result;
		if (result == SteamGridDBResult::OK) {
			AppendArtworks(response, artworks, &seen);
			lastResult = SteamGridDBResult::OK;
		} else if (lastResult != SteamGridDBResult::OK) {
			lastResult = result;
		}
	}
	if (!artworks->empty())
		return SteamGridDBResult::OK;
	return lastResult == SteamGridDBResult::OK ? SteamGridDBResult::NotFound : lastResult;
}

SteamGridDBResult SteamGridDB_GetArtwork(const std::string &apiKey, int64_t gameId, bool background,
		std::vector<SteamGridDBArtwork> *artworks, const std::atomic<bool> *cancel) {
	artworks->clear();
	if (apiKey.empty()) {
		return SteamGridDBResult::MissingKey;
	}
	if (gameId <= 0) {
		return SteamGridDBResult::NotFound;
	}
	std::string url = std::string(API_ROOT) + (background ? "heroes/game/" : "grids/game/") + std::to_string(gameId);
	url += background ? "?types=static&mimes=image/png,image/jpeg" : "?dimensions=600x900,342x482&types=static&mimes=image/png,image/jpeg";
	std::string response;
	SteamGridDBResult result = Request(url, apiKey, cancel, &response);
	if (result != SteamGridDBResult::OK) {
		return result;
	}
	std::unordered_set<std::string> seen;
	AppendArtworks(response, artworks, &seen);
	return artworks->empty() ? SteamGridDBResult::NotFound : SteamGridDBResult::OK;
}

SteamGridDBResult SteamGridDB_DownloadImage(const std::string &url, std::vector<uint8_t> *data, const std::atomic<bool> *cancel) {
	data->clear();
	if (url.rfind("https://", 0) != 0)
		return SteamGridDBResult::InvalidResponse;
	std::string response;
	SteamGridDBResult result = Request(url, "", cancel, &response);
	if (result != SteamGridDBResult::OK)
		return result;
	if (response.size() < 64 || DetectImageFileType((const uint8_t *)response.data(), response.size()) == ImageFileType::UNKNOWN)
		return SteamGridDBResult::InvalidResponse;
	data->assign(response.begin(), response.end());
	return SteamGridDBResult::OK;
}

const char *SteamGridDB_ResultMessage(SteamGridDBResult result) {
	switch (result) {
	case SteamGridDBResult::OK:
		return "Download complete.";
	case SteamGridDBResult::MissingKey:
		return "A SteamGridDB API key is required.";
	case SteamGridDBResult::Unauthorized:
		return "SteamGridDB rejected the API key.";
	case SteamGridDBResult::NetworkError:
		return "Could not connect to SteamGridDB.";
	case SteamGridDBResult::NotFound:
		return "No matching artwork was found.";
	case SteamGridDBResult::InvalidResponse:
		return "SteamGridDB returned invalid data.";
	case SteamGridDBResult::Cancelled:
		return "The SteamGridDB request was cancelled.";
	}
	return "SteamGridDB request failed.";
}

#endif
