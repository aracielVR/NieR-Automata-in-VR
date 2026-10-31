// download.h - file download and SHA256 (download.cpp), used for the DLSS file.
#pragma once
#include <windows.h>
#include <atomic>
#include <stdint.h>

bool HttpDownload(const wchar_t* url, const wchar_t* path, const char* sha256, std::atomic<int64_t>* got,
				  std::atomic<int64_t>* total, const std::atomic<bool>* cancel, char* err, size_t errCap);
bool FileSha256(const wchar_t* path, char hex[65]);  // lowercase hex; false if the file can't be read
