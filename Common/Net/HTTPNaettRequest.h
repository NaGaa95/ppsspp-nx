#pragma once

#include "ppsspp_config.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>

#include "Common/Net/HTTPRequest.h"

#ifndef HTTPS_NOT_AVAILABLE

#if PPSSPP_PLATFORM(SWITCH)
#include <curl/curl.h>
#else
#include "ext/naett-lib/naett.h"
#endif

namespace http {

struct NaettBodySink;

// Really an asynchronous request.
class HTTPSRequest : public Request {
public:
	HTTPSRequest(RequestMethod method, std::string_view url, std::string_view postData, std::string_view postMime, const Path &outfile, RequestFlags flags = RequestFlags::ProgressBar | RequestFlags::ProgressBarDelayed, std::string_view name = "");
	~HTTPSRequest();

	void Start() override;
	void Join() override;

	// Also acts as a Poll.
	bool Done() override;
	bool Failed() const override { return failed_; }

	// Cancelling has to reach naett, or it only changes the code we report once the transfer ends
	// on its own. See the .cpp.
	void Cancel() override;

private:
#if PPSSPP_PLATFORM(SWITCH)
	void Do();
	static size_t WriteCallback(char *data, size_t size, size_t count, void *userdata);
	static int ProgressCallback(void *userdata, curl_off_t downloadTotal, curl_off_t downloaded, curl_off_t uploadTotal, curl_off_t uploaded);

	std::thread thread_;
	std::atomic_bool completed_{false};
#else
	static int WriteBodyThunk(const void *source, int bytes, void *userData);

	bool completed_ = false;

	// Where the response body lands. Deliberately not part of this object: naett writes into it
	// from its own transfer thread, and that can outlive us if we're torn down before the request
	// finishes, so ownership has to be able to move elsewhere. See NaettBodySink in the .cpp.
	std::unique_ptr<NaettBodySink> sink_;

	// Naett state
	naettReq *req_ = nullptr;
	naettRes *res_ = nullptr;
#endif
	std::string postData_;
	std::string postMime_;
	bool failed_ = false;
};

}  // namespace http

#endif  // HTTPS_NOT_AVAILABLE
