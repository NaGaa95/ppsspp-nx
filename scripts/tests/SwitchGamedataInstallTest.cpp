// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>

#include "Core/Dialog/GamedataInstallWorker.h"

#define PPSSPP_PLATFORM(platform) 1
#define ERROR_LOG(...) ((void)0)
#define WARN_LOG_REPORT(...) ((void)0)
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s64 = int64_t;
using s32_le = int32_t;
using u32_le = uint32_t;
constexpr u32 SCE_KERNEL_ERROR_ERRNO_IO_ERROR = 0x80010005;
constexpr u32 SCE_ERROR_UTILITY_INVALID_STATUS = 0x80110001;
constexpr u32 SCE_ERROR_UTILITY_GAMEDATA_INVALID_MODE = 0x80111908;
constexpr int PSP_UTILITY_GAMEDATA_MODE_SHOW_PROGRESS = 1;
enum class UtilityDialogType { GAMEDATAINSTALL };
enum FileAccess { FILEACCESS_READ = 1, FILEACCESS_WRITE = 2, FILEACCESS_CREATE = 8, FILEACCESS_TRUNCATE = 16 };
static std::atomic<unsigned> workerStarts{0};
void SetCurrentThreadName(const char *) { ++workerStarts; }
void SetCurrentThreadAffinity(ThreadAffinityRole) {}
class PointerWrap;
struct pspUtilityDialogCommon { s32_le result = 0; };
struct PspUtilitySavedataSFOParam {};
static const auto mainThread = std::this_thread::get_id();
template <class T> struct PSPPointer {
	u32 ptr = 0;
	T data{};
	bool IsValid() const { assert(std::this_thread::get_id() == mainThread); return ptr != 0; }
	T *operator->() { assert(std::this_thread::get_id() == mainThread); return &data; }
	void NotifyWrite(const char *) { assert(std::this_thread::get_id() == mainThread); }
};
struct PSPDialog {
	enum DialogStatus { SCE_UTILITY_STATUS_NONE, SCE_UTILITY_STATUS_INITIALIZE, SCE_UTILITY_STATUS_RUNNING, SCE_UTILITY_STATUS_FINISHED, SCE_UTILITY_STATUS_SHUTDOWN };
	explicit PSPDialog(UtilityDialogType) {}
	virtual ~PSPDialog() = default;
	virtual int Update(int) = 0;
	virtual bool UseAutoStatus() = 0;
	virtual void DoState(PointerWrap &) {}
	virtual int Shutdown(bool = false) { status = SCE_UTILITY_STATUS_SHUTDOWN; return 0; }
	DialogStatus GetStatus() const { return status; }
	DialogStatus ReadStatus() const { return status; }
	void ChangeStatus(DialogStatus value, int) { status = value; }
	void UpdateCommon() {}
	DialogStatus status = SCE_UTILITY_STATUS_RUNNING;
};
struct FakeFileSystem {
	std::vector<u8> input, output;
	size_t readPosition = 0, writePosition = 0;
	size_t readLimit = SIZE_MAX, writeLimit = SIZE_MAX;
	size_t writeCount = 0, maxWrite = 0;
	int readError = 0, writeError = 0;
	int openError = 0;
	int outputOpenError = 0;
	int inputCloses = 0, outputCloses = 0;
	bool readReplay = true, writeReplay = true;
	void SetFileReplayEnabled(u32 handle, bool enabled) {
		assert(std::this_thread::get_id() == mainThread);
		(handle == 1 ? readReplay : writeReplay) = enabled;
	}
	std::function<void()> readGate;
	int OpenFile(const std::string &, FileAccess access) {
		return openError ? openError : access == FILEACCESS_READ ? 1 : outputOpenError ? outputOpenError : 2;
	}
	struct Info { size_t size; };
	Info GetFileInfo(const std::string &) { return {input.size()}; }
	size_t ReadFile(int handle, u8 *data, size_t bytes) {
		assert(handle == 1 && inputCloses == 0);
		assert(!readReplay);
		if (readGate) { readGate(); }
		if (readError) { return (size_t)(s64)readError; }
		bytes = std::min({bytes, input.size() - readPosition, readLimit});
		if (bytes) { memcpy(data, input.data() + readPosition, bytes); }
		readPosition += bytes;
		return bytes;
	}
	size_t WriteFile(int handle, const u8 *data, size_t bytes) {
		assert(handle == 2 && outputCloses == 0);
		assert(!writeReplay);
		if (writeError && writePosition >= writeLimit) { return (size_t)(s64)writeError; }
		bytes = std::min(bytes, writeLimit);
		++writeCount;
		maxWrite = std::max(maxWrite, bytes);
		if (output.size() < writePosition + bytes) { output.resize(writePosition + bytes); }
		if (bytes) { memcpy(output.data() + writePosition, data, bytes); }
		writePosition += bytes;
		return bytes;
	}
	void CloseFile(int handle) {
		assert(std::this_thread::get_id() == mainThread);
		if (handle == 1) { assert(inputCloses++ == 0); }
		else { assert(handle == 2 && outputCloses++ == 0); }
	}
} pspFileSystem;

struct DirectoryFileHandle {
	int hFile;
	s64 needsTrunc_;
	void Close();
};
// FILE_CLOSE

// DIALOG_DECLARATION
// DIALOG_METHODS
void PSPGamedataInstallDialog::DoState(PointerWrap &) {}
void PSPGamedataInstallDialog::RenderProgress(int) { assert(std::this_thread::get_id() == mainThread); }
void PSPGamedataInstallDialog::WriteSfoFile() {}
std::string PSPGamedataInstallDialog::GetGameDataInstallFileName(const SceUtilityGamedataInstallParam *, const std::string &name) { return name; }
struct FakeSaveDialog { void WaitForIO() {} };
static FakeSaveDialog *saveDialog;
static PSPGamedataInstallDialog *gamedataInstallDialog;
// UTILITY_WAIT

static void Prepare(PSPGamedataInstallDialog &dialog, size_t bytes, bool opened = true) {
	pspFileSystem = {};
	pspFileSystem.input.resize(bytes);
	for (size_t i = 0; i < bytes; ++i) { pspFileSystem.input[i] = (u8)(i * 17 + i / 251); }
	dialog.param.ptr = 1;
	dialog.param->mode = 0;
	dialog.numFiles = 1;
	dialog.inFileNames = {"data.bin"};
	dialog.allFilesSize = bytes;
	dialog.currentInputBytesLeft = (u32)bytes;
	dialog.currentInputFile = opened ? 1 : 0;
	dialog.currentOutputFile = opened ? 2 : 0;
	gamedataInstallDialog = &dialog;
}

static void FinishBatch(PSPGamedataInstallDialog &dialog) {
	dialog.CopyCurrentFileData();
	__UtilityWaitForIO();
	assert(pspFileSystem.readPosition == pspFileSystem.writePosition);
	assert(dialog.allReadSize == pspFileSystem.writePosition);
}

int main() {
	constexpr size_t batch = GamedataInstallWorker::BATCH_SIZE;
	{
		PSPGamedataInstallDialog dialog(UtilityDialogType::GAMEDATAINSTALL);
		Prepare(dialog, batch * 2 + 123);
		FinishBatch(dialog);
		// Checkpoint after a batch, as required before filesystem/dialog state serialization.
		const size_t checkpoint = dialog.allReadSize;
		assert(checkpoint == batch && dialog.currentInputBytesLeft == batch + 123);
		FinishBatch(dialog);
		pspFileSystem.readPosition = pspFileSystem.writePosition = checkpoint;
		pspFileSystem.readReplay = pspFileSystem.writeReplay = true;
		dialog.allReadSize = checkpoint;
		dialog.currentInputBytesLeft = batch + 123;
		FinishBatch(dialog);
		FinishBatch(dialog);
		assert(dialog.readFiles == 1 && dialog.currentInputFile == 0 && dialog.currentOutputFile == 0);
		assert(pspFileSystem.input == pspFileSystem.output && pspFileSystem.maxWrite == batch);
		assert(pspFileSystem.writeCount == 4 && dialog.param->progress == 100);
		assert(dialog.Shutdown() == (int)SCE_ERROR_UTILITY_INVALID_STATUS);
		dialog.Update(1);
		assert(dialog.Shutdown() == 0);
	}
	for (size_t bytes : {size_t(0), size_t(1), batch - 1, batch}) {
		PSPGamedataInstallDialog dialog(UtilityDialogType::GAMEDATAINSTALL);
		Prepare(dialog, bytes);
		FinishBatch(dialog);
		assert(dialog.readFiles == 1 && pspFileSystem.input == pspFileSystem.output);
		assert(pspFileSystem.inputCloses == 1 && pspFileSystem.outputCloses == 1);
	}
	{
		PSPGamedataInstallDialog dialog(UtilityDialogType::GAMEDATAINSTALL);
		Prepare(dialog, batch + 321);
		pspFileSystem.readLimit = 997;
		pspFileSystem.writeLimit = 701;
		FinishBatch(dialog);
		FinishBatch(dialog);
		assert(pspFileSystem.input == pspFileSystem.output && dialog.allReadSize == batch + 321);
	}
	for (int kind = 0; kind < 4; ++kind) {
		PSPGamedataInstallDialog dialog(UtilityDialogType::GAMEDATAINSTALL);
		Prepare(dialog, batch);
		if (kind == 0) { pspFileSystem.readError = (s32_le)0x80010005; }
		if (kind == 1) { pspFileSystem.input.resize(123); }
		if (kind == 2) { pspFileSystem.writeLimit = 7; pspFileSystem.writeError = (s32_le)0x8001001c; }
		if (kind == 3) { pspFileSystem.writeLimit = 0; }
		dialog.CopyCurrentFileData();
		__UtilityWaitForIO();
		assert(dialog.status == PSPDialog::SCE_UTILITY_STATUS_FINISHED);
		assert(dialog.param->common.result != 0 && dialog.readFiles == 0);
		assert(dialog.allReadSize == pspFileSystem.output.size());
		assert(pspFileSystem.inputCloses == 1 && pspFileSystem.outputCloses == 1);
	}
	for (bool cancel : {false, true}) {
		PSPGamedataInstallDialog dialog(UtilityDialogType::GAMEDATAINSTALL);
		Prepare(dialog, batch * 2);
		std::promise<void> entered, release;
		auto gate = release.get_future().share();
		pspFileSystem.readGate = [&] { entered.set_value(); gate.wait(); };
		dialog.CopyCurrentFileData();
		entered.get_future().wait();
		GamedataInstallWorker::Result result;
		assert(!dialog.copyWorker_.TakeResult(result));
		assert(!dialog.copyWorker_.Submit(1, [](u8 *, size_t) { return 0; }, [](const u8 *, size_t) { return 0; }));
		// Polling the actual dialog update must finish while disk I/O is still blocked.
		dialog.Update(1);
		auto poll = std::async(std::launch::async, [&] {
			GamedataInstallWorker::Result pending;
			return dialog.copyWorker_.TakeResult(pending);
		});
		const bool responsive = poll.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
		assert(!poll.get() && responsive);
		std::thread unblock([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			release.set_value();
		});
		if (cancel) { assert(dialog.Abort() == 0); }
		else { assert(dialog.Shutdown(true) == 0); }
		unblock.join();
		assert(dialog.allReadSize == batch && dialog.readFiles == 0);
		assert(pspFileSystem.inputCloses == 1 && pspFileSystem.outputCloses == 1);
	}
	for (bool outputError : {false, true}) {
		PSPGamedataInstallDialog dialog(UtilityDialogType::GAMEDATAINSTALL);
		Prepare(dialog, 123, false);
		if (outputError) { pspFileSystem.outputOpenError = (s32_le)0x80010002; }
		else { pspFileSystem.openError = (s32_le)0x80010002; }
		dialog.Update(1);
		assert(dialog.status == PSPDialog::SCE_UTILITY_STATUS_FINISHED);
		assert(dialog.param->common.result == (s32_le)0x80010002 && dialog.readFiles == 0);
	}
	{
		char path[] = "/tmp/ppsspp-install-truncate-XXXXXX";
		DirectoryFileHandle file{mkstemp(path), 7};
		assert(file.hFile >= 0);
		std::vector<u8> previous(4096, 42);
		assert(write(file.hFile, previous.data(), previous.size()) == (ssize_t)previous.size());
		file.Close();
		int fd = open(path, O_RDONLY);
		assert(fd >= 0 && lseek(fd, 0, SEEK_END) == 7);
		close(fd);
		unlink(path);
	}
	{
		PSPGamedataInstallDialog dialog(UtilityDialogType::GAMEDATAINSTALL);
		Prepare(dialog, 100);
		dialog.numFiles = 2;
		dialog.inFileNames = {"one.bin", "two.bin"};
		dialog.allFilesSize = 223;
		const unsigned before = workerStarts;
		FinishBatch(dialog);
		assert(workerStarts == before + 1 && dialog.readFiles == 1);
		assert(pspFileSystem.input == pspFileSystem.output);
		pspFileSystem = {};
		pspFileSystem.input.assign(123, 88);
		dialog.Update(1);
		dialog.CopyCurrentFileData();
		__UtilityWaitForIO();
		assert(workerStarts == before + 1 && dialog.readFiles == 2);
		assert(dialog.allReadSize == 223 && dialog.param->progress == 100);
		assert(pspFileSystem.input == pspFileSystem.output);
	}
	puts("Switch installer checks passed: 512KiB batching, byte integrity, partial I/O, errors, empty files, nonblocking polling, one in-flight job, worker reuse across files, checkpoint/resume, abort/shutdown drain, deferred truncation.");
}
