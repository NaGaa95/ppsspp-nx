#include "ppsspp_config.h"

#ifndef HTTPS_NOT_AVAILABLE

#if PPSSPP_PLATFORM(SWITCH)

#include <cstring>

#include <curl/curl.h>

#include "Common/Log.h"
#include "Common/Net/HTTPNaettRequest.h"
#include "Common/Thread/ThreadUtil.h"

namespace http {

HTTPSRequest::HTTPSRequest(RequestMethod method, std::string_view url, std::string_view postData, std::string_view postMime, const Path &outfile, RequestFlags flags, std::string_view name)
	: Request(method, url, name, outfile, &cancelled_, flags), postData_(postData), postMime_(postMime) {
}

HTTPSRequest::~HTTPSRequest() {
	Join();
}

void HTTPSRequest::Start() {
	thread_ = std::thread([this] { Do(); });
}

void HTTPSRequest::Join() {
	if (thread_.joinable())
		thread_.join();
}

bool HTTPSRequest::Done() {
	return completed_.load(std::memory_order_acquire);
}

void HTTPSRequest::Cancel() {
	Request::Cancel();
}

size_t HTTPSRequest::WriteCallback(char *data, size_t size, size_t count, void *userdata) {
	if (count != 0 && size > SIZE_MAX / count)
		return 0;
	HTTPSRequest *request = static_cast<HTTPSRequest *>(userdata);
	if (request->cancelled_)
		return 0;
	size_t bytes = size * count;
	std::memcpy(request->buffer_.Append(bytes), data, bytes);
	return bytes;
}

int HTTPSRequest::ProgressCallback(void *userdata, curl_off_t downloadTotal, curl_off_t downloaded, curl_off_t, curl_off_t) {
	HTTPSRequest *request = static_cast<HTTPSRequest *>(userdata);
	request->progress_.Update(downloaded, downloadTotal, false);
	return request->cancelled_ ? 1 : 0;
}

void HTTPSRequest::Do() {
	SetCurrentThreadAffinity(ThreadAffinityRole::IO);
	SetCurrentThreadName("HTTPSDownload::Do");

	CURL *curl = curl_easy_init();
	if (!curl) {
		failed_ = true;
		resultCode_ = -1;
		progress_.Update(0, 0, true);
		completed_.store(true, std::memory_order_release);
		return;
	}

	curl_slist *headers = nullptr;
	if (acceptMime_ && std::strcmp(acceptMime_, "*/*") != 0) {
		std::string accept = std::string("Accept: ") + acceptMime_;
		headers = curl_slist_append(headers, accept.c_str());
	}
	if (!postMime_.empty()) {
		std::string contentType = std::string("Content-Type: ") + postMime_;
		headers = curl_slist_append(headers, contentType.c_str());
	}

	char error[CURL_ERROR_SIZE]{};
	curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 128L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent_.empty() ? "PPSSPP" : userAgent_.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
	if (headers)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
	if (method_ == RequestMethod::POST) {
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData_.data());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)postData_.size());
	}

	CURLcode result = curl_easy_perform(curl);
	long responseCode = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
	resultCode_ = result == CURLE_OK ? (int)responseCode : -(int)result;
	failed_ = result != CURLE_OK || responseCode != 200 || cancelled_;
	if (!failed_ && !outfile_.empty()) {
		bool clear = !(flags_ & RequestFlags::KeepInMemory);
		if (!buffer_.FlushToFile(outfile_, clear)) {
			ERROR_LOG(Log::HTTP, "Failed writing download to '%s'", outfile_.c_str());
			failed_ = true;
		}
	}
	if (failed_) {
		ERROR_LOG(Log::HTTP, "HTTPS request failed for %s: %s (curl %d, HTTP %ld)", url_.c_str(),
			error[0] ? error : curl_easy_strerror(result), (int)result, responseCode);
	}
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	progress_.Update((int64_t)buffer_.size(), (int64_t)buffer_.size(), true);
	completed_.store(true, std::memory_order_release);
}

}  // namespace http

#else

#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include "Common/Net/HTTPRequest.h"
#include "Common/Net/HTTPNaettRequest.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/StringUtils.h"
#include "Common/Log.h"

#include "ext/naett-lib/naett.h"

namespace http {

HTTPSRequest::HTTPSRequest(RequestMethod method, std::string_view url, std::string_view postData, std::string_view postMime, const Path &outfile, RequestFlags flags, std::string_view name)
	: Request(method, url, name, outfile, &cancelled_, flags), postData_(postData), postMime_(postMime) {
}

HTTPSRequest::~HTTPSRequest() {
	HTTPSRequest::Join();
}

// The response body, and the flag that stops it arriving. naett hands us chunks on its own
// transfer thread, and there's no way to make it stop and be sure it has: naettClose only really
// cancels on Android, and waiting for the others would mean blocking shutdown. So the buffer it
// writes into is owned separately from the request, and that ownership can move - if we go away
// first, the sink is handed off rather than destroyed, and a late chunk lands somewhere that
// still exists instead of in a destroyed object.
struct NaettBodySink {
	Buffer buffer;
	// Written by us, read by the transfer thread.
	std::atomic<bool> cancelled{false};
};

// Sinks belonging to requests that hadn't finished when they were torn down, which only happens
// at shutdown - RequestManager cancels from its destructor. The naett objects can't be freed
// while a callback might still be in flight, and neither can these, so both are deliberately
// leaked. Allocated with new and never deleted, so it can't be destroyed out from under a late
// callback during static destruction either.
static std::vector<std::unique_ptr<NaettBodySink>> *g_abandonedSinks = new std::vector<std::unique_ptr<NaettBodySink>>();

int HTTPSRequest::WriteBodyThunk(const void *source, int bytes, void *userData) {
	NaettBodySink *sink = (NaettBodySink *)userData;
	if (sink->cancelled) {
		// Taking less than we were given fails the request, which is how naett lets us stop a
		// transfer. Without this, cancelling only relabelled the result once it finished anyway.
		return 0;
	}
	if (bytes <= 0) {
		return 0;
	}
	char *dest = sink->buffer.Append((size_t)bytes);
	memcpy(dest, source, bytes);
	return bytes;
}

void HTTPSRequest::Cancel() {
	Request::Cancel();
	if (sink_) {
		sink_->cancelled = true;
	}
}

void HTTPSRequest::Start() {
	_dbg_assert_(!req_);
	_dbg_assert_(!res_);

	std::vector<naettOption *> options;
	options.push_back(naettMethod(method_ == RequestMethod::GET ? "GET" : "POST"));
	options.push_back(naettHeader("Accept", acceptMime_));
	options.push_back(naettUserAgent(userAgent_.c_str()));
	if (!postMime_.empty()) {
		options.push_back(naettHeader("Content-Type", postMime_.c_str()));
	}
	if (method_ == RequestMethod::POST) {
		if (!postData_.empty()) {
			// Note: Naett does not take ownership over the body.
			options.push_back(naettBody(postData_.data(), (int)postData_.size()));
		}
	} else {
		_dbg_assert_(postData_.empty());
	}
	// 30 s timeout - not sure what's reasonable?
	options.push_back(naettTimeout(30 * 1000));  // milliseconds
	// Our own writer, so that Cancel() can actually stop a transfer rather than just relabelling
	// it once it finishes.
	sink_ = std::make_unique<NaettBodySink>();
	// In case someone managed to cancel us between construction and here.
	sink_->cancelled = cancelled_;
	options.push_back(naettBodyWriter(&HTTPSRequest::WriteBodyThunk, sink_.get()));

	const naettOption **opts = (const naettOption **)options.data();
	req_ = naettRequestWithOptions(url_.c_str(), (int)options.size(), opts);
	if (!req_) {
		// naett couldn't set the request up - a URL it can't parse, most likely. Fail it here
		// rather than handing a null to naettMake.
		ERROR_LOG(Log::HTTP, "Couldn't create a request for '%s'", url_.c_str());
		resultCode_ = naettGenericError;
		failed_ = true;
		completed_ = true;
		sink_.reset();
		progress_.Update(0, 0, true);
		return;
	}
	res_ = naettMake(req_);
	if (!res_) {
		ERROR_LOG(Log::HTTP, "Couldn't start a request for '%s'", url_.c_str());
		naettFree(req_);
		req_ = nullptr;
		resultCode_ = naettGenericError;
		failed_ = true;
		completed_ = true;
		sink_.reset();
		progress_.Update(0, 0, true);
		return;
	}

	progress_.Update(0, 0, false);
}

void HTTPSRequest::Join() {
	if (!res_ || !req_)
		return;  // No pending operation.
	// Tear down. A request that finished while nobody was polling Done() can still be closed
	// properly - it's only one that's genuinely still running that can't be.
	if (completed_ || naettComplete(res_)) {
		naettClose(res_);
		naettFree(req_);
		res_ = nullptr;
		req_ = nullptr;
		sink_.reset();
	} else {
		// Only reachable at shutdown, since RequestManager cancels from its destructor and
		// otherwise waits for Done(). Closing a response naett is still working on isn't safe on
		// three of the four backends, and there's nothing of ours to wait on, so let the request
		// go and keep its sink alive - a chunk arriving after this point then writes somewhere
		// that still exists. The process is on its way out; this is the last word on it.
		WARN_LOG(Log::HTTP, "Abandoning an unfinished request to '%s' - shutting down", url_.c_str());
		if (sink_) {
			sink_->cancelled = true;
			g_abandonedSinks->push_back(std::move(sink_));
		}
		res_ = nullptr;
		req_ = nullptr;
	}
}

bool HTTPSRequest::Done() {
	if (completed_)
		return true;
	if (!res_) {
		// Never started, or already let go of. Nothing left to wait for.
		return true;
	}

	if (!naettComplete(res_)) {
		int total = 0;
		int size = naettGetTotalBytesRead(res_, &total);
		progress_.Update(size, total, false);
		return false;
	}

	// -1000 is a code specified by us to represent cancellation, that is unlikely to ever collide with naett error codes.
	resultCode_ = IsCancelled() ? -1000 : naettGetStatus(res_);
	// The body arrived in the sink as it was read; take it over now that nothing else will touch it.
	const int bodyLength = sink_ ? (int)sink_->buffer.size() : 0;
	if (bodyLength > 0) {
		buffer_.Append(sink_->buffer);
	}
	if (resultCode_ < 0) {
		// It's a naett error. Translate and handle.
		switch (resultCode_) {
		case naettConnectionError:  // -1
			ERROR_LOG(Log::HTTP, "Connection error");
			break;
		case naettProtocolError:  // -2
			ERROR_LOG(Log::HTTP, "Protocol error");
			break;
		case naettReadError:  // -3
			ERROR_LOG(Log::HTTP, "Read error");
			break;
		case naettWriteError:  // -4
			ERROR_LOG(Log::HTTP, "Write error");
			break;
		case naettGenericError:  // -5
			ERROR_LOG(Log::HTTP, "Generic error");
			break;
		case -1000:
			// Ours, not naett's - see above. Cancelling is a normal thing to do, not an error.
			INFO_LOG(Log::HTTP, "Request to '%s' cancelled", url_.c_str());
			break;
		default:
			ERROR_LOG(Log::HTTP, "Unhandled naett error %d", resultCode_);
			break;
		}
		failed_ = true;
		progress_.Update(bodyLength, bodyLength, true);
	} else if (resultCode_ == 200) {
		bool clear = !(flags_ & RequestFlags::KeepInMemory);
		if (!outfile_.empty() && !buffer_.FlushToFile(outfile_, clear)) {
			ERROR_LOG(Log::HTTP, "Failed writing download to '%s'", outfile_.c_str());
		}
		progress_.Update(bodyLength, bodyLength, true);
	} else {
		WARN_LOG(Log::HTTP, "Naett request failed: %d", resultCode_);
		failed_ = true;
		progress_.Update(0, 0, true);
	}

	completed_ = true;

	// The callback will be called later.
	return true;
}

}  // namespace http

#endif  // PPSSPP_PLATFORM(SWITCH)

#endif  // HTTPS_NOT_AVAILABLE
