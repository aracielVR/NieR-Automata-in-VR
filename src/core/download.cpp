// download.cpp - HTTPS download to a file with a SHA256 check (WinHTTP, BCrypt), and a file's SHA256.
#include "core/download.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <winhttp.h>
#include <bcrypt.h>

namespace {
struct Sha256 {
	BCRYPT_ALG_HANDLE alg = nullptr;
	BCRYPT_HASH_HANDLE h = nullptr;
	bool ok = false;
	Sha256() {
		ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
			 BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0;
	}
	~Sha256() {
		if (h) BCryptDestroyHash(h);
		if (alg) BCryptCloseAlgorithmProvider(alg, 0);
	}
	void Add(const void* p, DWORD n) { ok = ok && BCryptHashData(h, (PUCHAR)p, n, 0) >= 0; }
	// lowercase hex, 64 characters
	bool Hex(char out[65]) {
		uint8_t d[32];
		if (!ok || BCryptFinishHash(h, d, 32, 0) < 0) return false;
		for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
		return true;
	}
};

struct Inet {
	HINTERNET h = nullptr;
	~Inet() {
		if (h) WinHttpCloseHandle(h);
	}
};
}  // namespace

bool HttpDownload(const wchar_t* url, const wchar_t* path, const char* sha256, std::atomic<int64_t>* got,
				  std::atomic<int64_t>* total, const std::atomic<bool>* cancel, char* err, size_t errCap) {
	*got = 0;
	*total = 0;
	err[0] = 0;
	URL_COMPONENTS parts = {};
	parts.dwStructSize = sizeof(parts);
	wchar_t host[256], pathPart[1024];
	parts.lpszHostName = host;
	parts.dwHostNameLength = 256;
	parts.lpszUrlPath = pathPart;
	parts.dwUrlPathLength = 1024;
	if (!WinHttpCrackUrl(url, 0, 0, &parts)) {
		snprintf(err, errCap, "bad address");
		return false;
	}
	// automatic proxy needs Windows 8.1; older systems fall back to the default proxy settings
	Inet session, connection, request;
	session.h = WinHttpOpen(L"NieRVR", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
	if (!session.h) session.h = WinHttpOpen(L"NieRVR", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
	if (session.h) connection.h = WinHttpConnect(session.h, host, parts.nPort, 0);
	if (connection.h)
		request.h = WinHttpOpenRequest(connection.h, L"GET", pathPart, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
									   parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
	if (!request.h || !WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
		!WinHttpReceiveResponse(request.h, nullptr)) {
		snprintf(err, errCap, "no connection (error %u)", (unsigned)GetLastError());
		return false;
	}
	DWORD status = 0, size = sizeof(status);
	WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
	if (status != 200) {
		snprintf(err, errCap, "the server answered %u", (unsigned)status);
		return false;
	}
	wchar_t lengthText[32];
	size = sizeof(lengthText);
	if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, lengthText, &size, nullptr)) *total = _wtoi64(lengthText);

	wchar_t partPath[MAX_PATH];
	snwprintf(partPath, MAX_PATH, L"%ls.part", path);
	HANDLE file = CreateFileW(partPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		snprintf(err, errCap, "can't write to the game's folder (error %u)", (unsigned)GetLastError());
		return false;
	}
	Sha256 hash;
	char buf[1 << 16];  // 64 KB of the download thread's stack
	bool ok = true;
	while (true) {
		if (*cancel) {
			snprintf(err, errCap, "cancelled");
			ok = false;
			break;
		}
		DWORD read = 0, wrote = 0;
		if (!WinHttpReadData(request.h, buf, sizeof(buf), &read)) {
			snprintf(err, errCap, "the connection broke (error %u)", (unsigned)GetLastError());
			ok = false;
			break;
		}
		if (read == 0) break;  // done
		hash.Add(buf, read);
		if (!WriteFile(file, buf, read, &wrote, nullptr) || wrote != read) {
			snprintf(err, errCap, "can't write (disk full?)");
			ok = false;
			break;
		}
		*got += read;
	}
	CloseHandle(file);
	char hex[65];
	if (ok && (!hash.Hex(hex) || _stricmp(hex, sha256) != 0)) {
		snprintf(err, errCap, "the file isn't the expected one (checksum)");
		ok = false;
	}
	if (ok && !MoveFileExW(partPath, path, MOVEFILE_REPLACE_EXISTING)) {
		snprintf(err, errCap, "can't put the file in place (error %u)", (unsigned)GetLastError());
		ok = false;
	}
	if (!ok) DeleteFileW(partPath);
	return ok;
}

bool FileSha256(const wchar_t* path, char hex[65]) {
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
							  FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	Sha256 hash;
	char buf[1 << 16];  // 64 KB of the hashing thread's stack
	DWORD read = 0;
	bool ok = true;
	while (true) {
		ok = ReadFile(file, buf, sizeof(buf), &read, nullptr);
		if (!ok || read == 0) break;
		hash.Add(buf, read);
	}
	CloseHandle(file);
	return ok && hash.Hex(hex);
}
