#include "updater.h"

#include "../common/log.h"
#include "../common/paths.h"
#include "../common/strings.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <vector>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace ui {

namespace fs = std::filesystem;

static const wchar_t *k_latest = L"https://api.github.com/repos/panix8th/Highlight/releases/latest";

/* GET over HTTPS (redirects followed). Calls sink for each chunk; returns the HTTP status or 0. */
static int http_get(const std::wstring &url, const std::function<bool(const char *, size_t, uint64_t)> &sink)
{
	URL_COMPONENTS uc = {sizeof(uc)};
	wchar_t host[256] = {}, path[2048] = {};
	uc.lpszHostName = host;
	uc.dwHostNameLength = 256;
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = 2048;
	if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
		return 0;

	const std::wstring agent = L"Highlight/" + hl::widen(HL_VERSION);
	HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
				       WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) /* older Windows 10 without automatic proxy */
		session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
				      WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session)
		return 0;
	WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
	int status = 0;
	HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
	HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
						  WINHTTP_DEFAULT_ACCEPT_TYPES,
						  uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
			     : nullptr;
	if (req) {
		const wchar_t *headers = L"Accept: application/vnd.github+json\r\n";
		if (WinHttpSendRequest(req, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
		    WinHttpReceiveResponse(req, nullptr)) {
			DWORD code = 0, len = sizeof(code);
			WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
					    &code, &len, WINHTTP_NO_HEADER_INDEX);
			uint64_t total = 0;
			wchar_t clen[32] = {};
			DWORD clen_size = sizeof(clen);
			if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen, &clen_size,
						WINHTTP_NO_HEADER_INDEX))
				total = _wcstoui64(clen, nullptr, 10);
			status = (int)code;
			std::vector<char> buf(64 * 1024);
			for (;;) {
				DWORD got = 0;
				if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got)) {
					status = 0;
					break;
				}
				if (got == 0)
					break;
				if (!sink(buf.data(), got, total)) {
					status = 0;
					break;
				}
			}
		}
		WinHttpCloseHandle(req);
	}
	if (conn)
		WinHttpCloseHandle(conn);
	WinHttpCloseHandle(session);
	return status;
}

Updater::~Updater()
{
	join();
}

void Updater::join()
{
	if (thread_.joinable())
		thread_.join();
}

UpdateInfo Updater::info() const
{
	std::lock_guard lock(mutex_);
	return info_;
}

std::string Updater::error() const
{
	std::lock_guard lock(mutex_);
	return error_;
}

bool Updater::newer(const std::string &a, const std::string &b)
{
	int va[3] = {}, vb[3] = {};
	sscanf(a.c_str(), "%d.%d.%d", &va[0], &va[1], &va[2]);
	sscanf(b.c_str(), "%d.%d.%d", &vb[0], &vb[1], &vb[2]);
	for (int i = 0; i < 3; i++)
		if (va[i] != vb[i])
			return va[i] > vb[i];
	return false;
}

bool Updater::installed_copy()
{
	std::error_code ec;
	return fs::exists(hl::paths::install_dir() / L"unins000.exe", ec);
}

void Updater::check(bool manual)
{
	State s = state_;
	if (s == State::Checking || s == State::Downloading)
		return;
	join();
	manual_ = manual;
	state_ = State::Checking;
	thread_ = std::thread([this]() {
		std::string body;
		int status = http_get(k_latest, [&](const char *d, size_t n, uint64_t) {
			body.append(d, n);
			return body.size() < 4 * 1024 * 1024;
		});
		UpdateInfo info;
		std::string err;
		if (status == 200) {
			try {
				auto j = nlohmann::json::parse(body);
				std::string tag = j.value("tag_name", "");
				info.version = (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? tag.substr(1) : tag;
				info.page_url = j.value("html_url", "");
				for (const auto &a : j.value("assets", nlohmann::json::array())) {
					std::string name = a.value("name", "");
					if (name.size() > 10 && name.substr(name.size() - 10) == "-setup.exe")
						info.setup_url = a.value("browser_download_url", "");
				}
			} catch (...) {
				err = "Unexpected answer from GitHub";
			}
		} else {
			err = status ? "GitHub answered " + std::to_string(status) : "No connection to GitHub";
		}
		{
			std::lock_guard lock(mutex_);
			info_ = info;
			error_ = err;
		}
		if (!err.empty()) {
			HL_WARN("Update check failed: %s", err.c_str());
			state_ = State::Failed;
		} else if (newer(info.version, HL_VERSION)) {
			HL_INFO("Update available: %s", info.version.c_str());
			state_ = State::Available;
		} else {
			state_ = State::UpToDate;
		}
	});
}

void Updater::install(const UpdateInfo &info)
{
	if (state_ == State::Downloading)
		return;
	if (info.setup_url.empty() || !installed_copy()) {
		/* portable copy: let the user download the new zip */
		ShellExecuteW(nullptr, L"open", hl::widen(info.page_url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		return;
	}
	join();
	state_ = State::Downloading;
	progress_ = 0;
	thread_ = std::thread([this, info]() {
		wchar_t tmp[MAX_PATH] = {};
		GetTempPathW(MAX_PATH, tmp);
		const fs::path out = fs::path(tmp) / hl::widen("Highlight-" + info.version + "-setup.exe");
		FILE *f = _wfopen(out.c_str(), L"wb");
		uint64_t got = 0;
		int status = 0;
		if (f) {
			status = http_get(hl::widen(info.setup_url), [&](const char *d, size_t n, uint64_t total) {
				got += n;
				if (total)
					progress_ = (float)((double)got / (double)total);
				return fwrite(d, 1, n, f) == n;
			});
			fclose(f);
		}
		if (status != 200 || got < 1024 * 1024) {
			std::error_code ec;
			fs::remove(out, ec);
			std::lock_guard lock(mutex_);
			error_ = "The download did not finish";
			state_ = State::Failed;
			return;
		}
		/* The setup closes Highlight, installs over it and offers to start it again. */
		ShellExecuteW(nullptr, L"open", out.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		HL_INFO("Started installer %s", hl::narrow(out.wstring()).c_str());
		state_ = State::Launched;
	});
}

} // namespace ui
