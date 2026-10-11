#include "library.h"
#include "paths.h"
#include "strings.h"

#include <windows.h>

#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hl {

static Upload upload_from(const json &j)
{
	Upload up;
	up.key = j.value("key", "");
	up.link = j.value("link", "");
	up.host = j.value("host", "");
	up.bucket = j.value("bucket", "");
	up.bytes = j.value("bytes", (uint64_t)0);
	up.time = j.value("time", (int64_t)0);
	return up;
}

static json upload_to(const Upload &u)
{
	return {{"key", u.key}, {"link", u.link}, {"host", u.host}, {"bucket", u.bucket}, {"bytes", u.bytes}, {"time", u.time}};
}

std::string Library::key(const std::string &path)
{
	return to_lower(path);
}

Library Library::load()
{
	Library lib;
	std::ifstream f(paths::library_file());
	if (!f)
		return lib;
	try {
		json j = json::parse(f);
		if (auto it = j.find("favorites"); it != j.end() && it->is_array())
			for (const auto &p : *it)
				if (p.is_string())
					lib.favorites_.insert(key(p.get<std::string>()));
		if (auto it = j.find("uploads"); it != j.end() && it->is_object())
			for (auto u = it->begin(); u != it->end(); ++u) {
				if (!u.value().is_object())
					continue;
				Upload up = upload_from(u.value());
				if (!up.key.empty() && !up.link.empty())
					lib.uploads_[u.key()] = up;
			}
		if (auto it = j.find("older_uploads"); it != j.end() && it->is_array())
			for (const auto &u : *it)
				if (u.is_object() && !u.value("key", "").empty())
					lib.older_.push_back(upload_from(u));
	} catch (...) {
	}
	return lib;
}

bool Library::save() const
{
	json j;
	j["favorites"] = json::array();
	for (const auto &f : favorites_)
		j["favorites"].push_back(f);
	j["uploads"] = json::object();
	for (const auto &[path, u] : uploads_)
		j["uploads"][path] = upload_to(u);
	j["older_uploads"] = json::array();
	for (const auto &u : older_)
		j["older_uploads"].push_back(upload_to(u));

	auto path = paths::library_file();
	auto tmp = path;
	tmp += L".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f)
			return false;
		f << j.dump(2);
	}
	return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool Library::is_favorite(const std::string &path) const
{
	return favorites_.count(key(path)) != 0;
}

void Library::set_favorite(const std::string &path, bool favorite)
{
	if (favorite)
		favorites_.insert(key(path));
	else
		favorites_.erase(key(path));
}

void Library::rename(const std::string &from, const std::string &to)
{
	if (favorites_.erase(key(from)))
		favorites_.insert(key(to));
	if (auto it = uploads_.find(key(from)); it != uploads_.end()) {
		Upload u = it->second;
		uploads_.erase(it);
		set_upload(to, u);
	}
}

const Upload *Library::upload(const std::string &path) const
{
	auto it = uploads_.find(key(path));
	return it == uploads_.end() ? nullptr : &it->second;
}

void Library::set_upload(const std::string &path, const Upload &u)
{
	auto it = uploads_.find(key(path));
	if (it != uploads_.end() && it->second.key != u.key)
		older_.push_back(it->second); /* the earlier link stays online: keep it deletable */
	uploads_[key(path)] = u;
}

void Library::add_older(const Upload &u)
{
	older_.push_back(u);
}

void Library::remove_older(const std::string &object_key)
{
	for (auto it = older_.begin(); it != older_.end();)
		it = it->key == object_key ? older_.erase(it) : it + 1;
}

void Library::remove_upload(const std::string &path)
{
	uploads_.erase(key(path));
}

uint64_t Library::uploaded_bytes() const
{
	uint64_t total = 0;
	for (const auto &[path, u] : uploads_)
		total += u.bytes;
	for (const auto &u : older_)
		total += u.bytes;
	return total;
}

void Library::forget(const std::string &path)
{
	favorites_.erase(key(path));
	/* a later clip with the same name must not inherit this link */
	if (auto it = uploads_.find(key(path)); it != uploads_.end()) {
		older_.push_back(it->second);
		uploads_.erase(it);
	}
}

} // namespace hl
