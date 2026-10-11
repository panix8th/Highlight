#include "strings.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace hl {

std::wstring widen(const std::string &utf8)
{
	if (utf8.empty())
		return {};
	int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
	std::wstring out(len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), len);
	return out;
}

std::string narrow(const std::wstring &wide)
{
	if (wide.empty())
		return {};
	int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
	std::string out(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(), len, nullptr, nullptr);
	return out;
}

std::string format_duration(double seconds)
{
	if (seconds < 0)
		seconds = 0;
	long long total = (long long)(seconds + 0.5);
	long long h = total / 3600;
	long long m = (total / 60) % 60;
	long long s = total % 60;
	char buf[32];
	if (h > 0)
		snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", h, m, s);
	else
		snprintf(buf, sizeof(buf), "%lld:%02lld", m, s);
	return buf;
}

std::string format_bytes(uint64_t bytes)
{
	char buf[32];
	double mb = (double)bytes / (1024.0 * 1024.0);
	if (mb < 1.0)
		snprintf(buf, sizeof(buf), "%llu KB", (unsigned long long)(bytes / 1024));
	else if (mb < 10.0)
		snprintf(buf, sizeof(buf), "%.1f MB", mb);
	else if (mb < 1000.0)
		snprintf(buf, sizeof(buf), "%.0f MB", mb);
	else
		snprintf(buf, sizeof(buf), "%.1f GB", mb / 1024.0);
	return buf;
}

std::string sanitize_filename(const std::string &name)
{
	std::string out;
	out.reserve(name.size());
	for (char c : name) {
		unsigned char u = (unsigned char)c;
		if (u < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
		    c == '?' || c == '*')
			out.push_back('_');
		else
			out.push_back(c);
	}
	while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
		out.pop_back();
	if (out.empty())
		out = "Clip";
	return out;
}

std::string to_lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
	return s;
}

} // namespace hl
