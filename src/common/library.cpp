#include "library.h"
#include "paths.h"
#include "strings.h"

#include <windows.h>

#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hl {

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
}

void Library::forget(const std::string &path)
{
	favorites_.erase(key(path));
}

} // namespace hl
