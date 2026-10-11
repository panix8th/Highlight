#include "r2.h"

#include "../common/log.h"
#include "../common/settings.h"
#include "../common/strings.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <vector>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace ui {

namespace fs = std::filesystem;

namespace {

/* Tests point the client at a local S3 stand-in over plain HTTP. */
bool g_plain_http_for_tests = false;

/* ------------------------------------------------------------------ */
/* signing (AWS Signature Version 4, which R2's S3 API uses)          */

/* SHA-256, or HMAC-SHA-256 when a key is given (32 raw bytes) */
std::string sha256(const std::string &data, const std::string *key = nullptr)
{
	BCRYPT_ALG_HANDLE alg = nullptr;
	if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
							key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0)))
		return {};
	std::string out(32, '\0');
	BCRYPT_HASH_HANDLE h = nullptr;
	bool ok = BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, key ? (PUCHAR)key->data() : nullptr,
						  key ? (ULONG)key->size() : 0, 0)) &&
		  BCRYPT_SUCCESS(BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0)) &&
		  BCRYPT_SUCCESS(BCryptFinishHash(h, (PUCHAR)out.data(), (ULONG)out.size(), 0));
	if (h)
		BCryptDestroyHash(h);
	BCryptCloseAlgorithmProvider(alg, 0);
	return ok ? out : std::string();
}

std::string hmac(const std::string &key, const std::string &data)
{
	return sha256(data, &key);
}

std::string hex(const std::string &raw)
{
	static const char digits[] = "0123456789abcdef";
	std::string out;
	for (unsigned char c : raw) {
		out += digits[c >> 4];
		out += digits[c & 15];
	}
	return out;
}

/* RFC 3986 encoding as S3 expects it; '/' is kept between key segments */
std::string uri_encode(const std::string &s, bool keep_slash)
{
	std::string out;
	char buf[4];
	for (unsigned char c : s) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/')) {
			out += (char)c;
		} else {
			snprintf(buf, sizeof(buf), "%%%02X", c);
			out += buf;
		}
	}
	return out;
}

/* Request headers (x-amz-date, x-amz-content-sha256, Authorization) for one request.
 * The body is sent as UNSIGNED-PAYLOAD (HTTPS protects it), so a large clip is read once. */
std::wstring sign(const R2Config &c, const char *method, const std::string &path)
{
	SYSTEMTIME t;
	GetSystemTime(&t);
	char amz_date[32], date[16];
	snprintf(amz_date, sizeof(amz_date), "%04d%02d%02dT%02d%02d%02dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
		 t.wSecond);
	snprintf(date, sizeof(date), "%04d%02d%02d", t.wYear, t.wMonth, t.wDay);
	const std::string payload = "UNSIGNED-PAYLOAD";
	const std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
	const std::string canonical = std::string(method) + "\n" + path + "\n" + "\n" + "host:" + c.host + "\n" +
				      "x-amz-content-sha256:" + payload + "\n" + "x-amz-date:" + amz_date + "\n\n" +
				      signed_headers + "\n" + payload;
	const std::string scope = std::string(date) + "/auto/s3/aws4_request";
	const std::string to_sign = std::string("AWS4-HMAC-SHA256\n") + amz_date + "\n" + scope + "\n" + hex(sha256(canonical));
	std::string key = hmac("AWS4" + c.secret, date);
	key = hmac(key, "auto");
	key = hmac(key, "s3");
	key = hmac(key, "aws4_request");
	const std::string signature = hex(hmac(key, to_sign));
	const std::string headers = std::string("x-amz-date: ") + amz_date + "\r\n" + "x-amz-content-sha256: " + payload +
				    "\r\n" + "Authorization: AWS4-HMAC-SHA256 Credential=" + c.access_key + "/" + scope +
				    ", SignedHeaders=" + signed_headers + ", Signature=" + signature + "\r\n";
	return hl::widen(headers);
}

/* ------------------------------------------------------------------ */
/* HTTPS                                                              */

struct Response {
	int status = 0;     /* 0: no answer (see error) */
	DWORD error = 0;    /* WinHTTP error when status is 0 */
	std::string body;   /* first 64 KB */
	bool cancelled = false;
};

/* The body comes from a file or from memory (or is empty). */
Response request(const std::wstring &host, INTERNET_PORT port, bool https, const wchar_t *method, const std::wstring &path,
		 const std::wstring &headers, FILE *file, uint64_t file_size, const std::string *data,
		 const std::function<bool(float)> &progress)
{
	Response r;
	const std::wstring agent = L"Highlight/" + hl::widen(HL_VERSION);
	HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
				       WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) /* older Windows 10 without automatic proxy */
		session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
				      WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		r.error = GetLastError();
		return r;
	}
	WinHttpSetTimeouts(session, 15000, 15000, 60000, 60000);
	HINTERNET conn = WinHttpConnect(session, host.c_str(), port, 0);
	HINTERNET req = conn ? WinHttpOpenRequest(conn, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
						  WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0)
			     : nullptr;
	const uint64_t total = file ? file_size : data ? data->size() : 0;
	bool sent = req && WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
					      headers.empty() ? 0 : (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, (DWORD)total, 0);
	if (sent && data && !data->empty()) {
		DWORD wrote = 0;
		sent = WinHttpWriteData(req, data->data(), (DWORD)data->size(), &wrote);
	} else if (sent && file) {
		std::vector<char> buf(256 * 1024);
		uint64_t done = 0;
		while (sent && done < total) {
			const size_t n = fread(buf.data(), 1, buf.size(), file);
			if (n == 0) {
				sent = false;
				break;
			}
			DWORD wrote = 0;
			sent = WinHttpWriteData(req, buf.data(), (DWORD)n, &wrote) && wrote == n;
			done += n;
			if (progress && !progress((float)((double)done / (double)total))) {
				r.cancelled = true;
				sent = false;
			}
		}
	}
	if (sent && WinHttpReceiveResponse(req, nullptr)) {
		DWORD code = 0, len = sizeof(code);
		WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
				    &code, &len, WINHTTP_NO_HEADER_INDEX);
		r.status = (int)code;
		char buf[8192];
		for (;;) {
			DWORD got = 0;
			if (!WinHttpReadData(req, buf, sizeof(buf), &got) || got == 0)
				break;
			if (r.body.size() < 64 * 1024)
				r.body.append(buf, got);
		}
	} else if (!r.cancelled) {
		r.error = GetLastError();
	}
	if (req)
		WinHttpCloseHandle(req);
	if (conn)
		WinHttpCloseHandle(conn);
	WinHttpCloseHandle(session);
	return r;
}

Response r2_request(const R2Config &c, const char *method, const std::string &key, FILE *file, uint64_t size,
		    const std::string *data, const std::string &content_type, const std::function<bool(float)> &progress)
{
	const std::string path = "/" + uri_encode(c.bucket, false) + "/" + uri_encode(key, true);
	std::wstring headers = sign(c, method, path);
	if (!content_type.empty())
		headers += hl::widen("Content-Type: " + content_type + "\r\nCache-Control: public, max-age=31536000\r\n");
	/* the signed host keeps a ":port" (as the Host header does); the connection does not */
	std::string host = c.host;
	INTERNET_PORT port = g_plain_http_for_tests ? INTERNET_DEFAULT_HTTP_PORT : INTERNET_DEFAULT_HTTPS_PORT;
	if (const size_t colon = host.find(':'); colon != std::string::npos) {
		port = (INTERNET_PORT)atoi(host.c_str() + colon + 1);
		host.resize(colon);
	}
	return request(hl::widen(host), port, !g_plain_http_for_tests, hl::widen(method).c_str(), hl::widen(path), headers,
		       file, size, data, progress);
}

std::string xml_value(const std::string &body, const char *tag)
{
	const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
	const size_t a = body.find(open);
	if (a == std::string::npos)
		return {};
	const size_t b = body.find(close, a);
	return b == std::string::npos ? std::string() : body.substr(a + open.size(), b - a - open.size());
}

/* What went wrong, in words the user can act on. */
std::string describe(const Response &r, const R2Config &c)
{
	if (r.cancelled)
		return "Cancelled";
	if (r.status == 0) {
		switch (r.error) {
		case ERROR_WINHTTP_NAME_NOT_RESOLVED:
			return "Cloudflare R2 could not be found at " + c.host + ". Check the account ID.";
		case ERROR_WINHTTP_TIMEOUT:
			return "The connection to Cloudflare timed out. Try again.";
		default:
			return "No connection to Cloudflare (error " + std::to_string(r.error) + ").";
		}
	}
	const std::string code = xml_value(r.body, "Code");
	if (code == "InvalidAccessKeyId" || code == "SignatureDoesNotMatch" || code == "Unauthorized")
		return "Cloudflare did not accept the access key or secret. Check both in Settings > Share links.";
	if (code == "NoSuchBucket")
		return "The bucket \"" + c.bucket + "\" does not exist in this account. Check its name.";
	if (code == "AccessDenied")
		return "This API token may not write to the bucket. Give it \"Object Read & Write\" for \"" + c.bucket + "\".";
	if (code == "RequestTimeTooSkewed")
		return "Your PC clock is off, so Cloudflare refused the request. Turn on \"Set time automatically\" in Windows.";
	if (code == "EntityTooLarge")
		return "The file is too big for one upload. Make a smaller share copy first.";
	std::string msg = "Cloudflare answered " + std::to_string(r.status);
	if (!code.empty())
		msg += " (" + code + ")";
	const std::string detail = xml_value(r.body, "Message");
	if (!detail.empty())
		msg += ": " + detail;
	return msg;
}

std::string content_type_for(const std::string &file)
{
	std::string ext = hl::to_lower(hl::narrow(fs::path(hl::widen(file)).extension().wstring()));
	if (ext == ".mp4")
		return "video/mp4";
	if (ext == ".mkv")
		return "video/x-matroska";
	if (ext == ".mov")
		return "video/quicktime";
	if (ext == ".png")
		return "image/png";
	return "application/octet-stream";
}

std::string random_part(int n)
{
	static const char alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789"; /* no look-alikes */
	std::vector<unsigned char> raw(n);
	BCryptGenRandom(nullptr, raw.data(), (ULONG)raw.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	std::string out;
	for (unsigned char b : raw)
		out += alphabet[b % 32];
	return out;
}

std::string trim(std::string s)
{
	while (!s.empty() && isspace((unsigned char)s.back()))
		s.pop_back();
	size_t i = 0;
	while (i < s.size() && isspace((unsigned char)s[i]))
		i++;
	return s.substr(i);
}

} // namespace

/* ------------------------------------------------------------------ */

R2Config R2Config::from(const hl::ShareSettings &s)
{
	R2Config c;
	/* The account ID, or the whole S3 endpoint as Cloudflare shows it
	 * (https://<id>.r2.cloudflarestorage.com, also the .eu. one). */
	std::string account = trim(s.r2_account);
	if (account.find("r2.cloudflarestorage.com") != std::string::npos) {
		size_t start = account.find("://");
		start = start == std::string::npos ? 0 : start + 3;
		c.host = account.substr(start, account.find('/', start) - start);
	} else if (!account.empty()) {
		c.host = account + ".r2.cloudflarestorage.com";
	}
	c.access_key = trim(s.r2_access_key);
	c.secret = trim(s.r2_secret);
	c.bucket = trim(s.r2_bucket);
	c.public_url = trim(s.r2_public_url);
	if (!c.public_url.empty() && c.public_url.find("://") == std::string::npos)
		c.public_url = "https://" + c.public_url;
	while (!c.public_url.empty() && c.public_url.back() == '/')
		c.public_url.pop_back();
	return c;
}

std::string R2Config::missing() const
{
	if (host.empty())
		return "account ID";
	if (access_key.empty())
		return "access key ID";
	if (secret.empty())
		return "secret access key";
	if (bucket.empty())
		return "bucket name";
	if (public_url.empty())
		return "public bucket address";
	return {};
}

std::string R2Config::link(const std::string &key) const
{
	return public_url + "/" + uri_encode(key, true);
}

std::string r2_new_key(const std::string &display_name, const std::string &file_path)
{
	std::string slug;
	for (unsigned char ch : display_name) {
		if (isalnum(ch) && ch < 128)
			slug += (char)tolower(ch);
		else if (!slug.empty() && slug.back() != '-')
			slug += '-';
		if (slug.size() >= 48)
			break;
	}
	while (!slug.empty() && slug.back() == '-')
		slug.pop_back();
	if (slug.empty())
		slug = "clip";
	time_t now = time(nullptr);
	tm local = {};
	localtime_s(&local, &now);
	char day[16];
	strftime(day, sizeof(day), "%Y-%m-%d", &local);
	std::string ext = hl::to_lower(hl::narrow(fs::path(hl::widen(file_path)).extension().wstring()));
	return std::string(day) + "/" + slug + "-" + random_part(10) + ext;
}

bool r2_upload(const R2Config &c, const std::string &file, const std::string &key,
	       const std::function<bool(float)> &progress, std::string *err)
{
	auto fail = [&](const std::string &m) {
		if (err)
			*err = m;
		HL_WARN("Upload failed: %s", m.c_str());
		return false;
	};
	if (!c.missing().empty())
		return fail("Share links are not set up: the " + c.missing() + " is missing (Settings > Share links).");
	std::error_code ec;
	const uint64_t size = fs::file_size(hl::widen(file), ec);
	if (ec)
		return fail("The file could not be read.");
	if (size >= 0xFFFFFFFFull) /* one PUT, and WinHTTP's length is 32 bit */
		return fail("This file is over 4 GB, too big for one upload. Make a smaller share copy first.");
	FILE *f = _wfopen(hl::widen(file).c_str(), L"rb");
	if (!f)
		return fail("The file could not be opened.");
	Response r = r2_request(c, "PUT", key, f, size, nullptr, content_type_for(file), progress);
	fclose(f);
	if (r.status != 200)
		return fail(describe(r, c));
	HL_INFO("Uploaded %s (%llu bytes) as %s", file.c_str(), (unsigned long long)size, key.c_str());
	return true;
}

bool r2_delete(const R2Config &c, const std::string &key, std::string *err)
{
	Response r = r2_request(c, "DELETE", key, nullptr, 0, nullptr, "", nullptr);
	if (r.status == 204 || r.status == 200 || r.status == 404)
		return true;
	if (err)
		*err = describe(r, c);
	return false;
}

bool r2_test(const R2Config &c, std::string *err)
{
	auto fail = [&](const std::string &m) {
		if (err)
			*err = m;
		return false;
	};
	if (!c.missing().empty())
		return fail("The " + c.missing() + " is missing.");
	const std::string key = "highlight-test/" + random_part(12) + ".txt";
	const std::string body = "Highlight share link test\n";
	Response put = r2_request(c, "PUT", key, nullptr, 0, &body, "text/plain", nullptr);
	if (put.status != 200)
		return fail(describe(put, c));

	/* the public address must serve it, or the links would not open */
	const std::wstring url = hl::widen(c.link(key));
	URL_COMPONENTS uc = {sizeof(uc)};
	wchar_t host[256] = {}, path[2048] = {};
	uc.lpszHostName = host;
	uc.dwHostNameLength = 256;
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = 2048;
	std::string problem;
	if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
		problem = "The public bucket address is not a web address. It looks like https://pub-….r2.dev";
	} else {
		Response get = request(host, uc.nPort, uc.nScheme == INTERNET_SCHEME_HTTPS, L"GET", path, L"", nullptr, 0,
				       nullptr, nullptr);
		if (get.status == 0)
			problem = "Uploading works, but " + c.public_url +
				  " could not be reached. Check the public bucket address.";
		else if (get.status != 200 || get.body != body)
			problem = "Uploading works, but the public address does not show the file (" +
				  std::to_string(get.status) +
				  "). In Cloudflare open the bucket > Settings > Public access, allow the R2.dev "
				  "subdomain, and paste that address here. It can take a minute to start working.";
	}
	r2_delete(c, key, nullptr);
	return problem.empty() ? true : fail(problem);
}

bool copy_text_to_clipboard(const std::string &text)
{
	const std::wstring w = hl::widen(text);
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
	if (!mem)
		return false;
	memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
	GlobalUnlock(mem);
	if (!OpenClipboard(nullptr)) {
		GlobalFree(mem);
		return false;
	}
	EmptyClipboard();
	const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
	CloseClipboard();
	if (!ok)
		GlobalFree(mem);
	return ok;
}

} // namespace ui
