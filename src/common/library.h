#pragma once

#include <mutex>
#include <set>
#include <string>

namespace hl {

/* Per-clip metadata that is not stored in the video file itself.
 * Kept in %APPDATA%\Highlight\library.json; shared by the core and the UI. */
class Library {
public:
	static Library load();
	bool save() const;

	bool is_favorite(const std::string &path) const;
	void set_favorite(const std::string &path, bool favorite);
	void rename(const std::string &from, const std::string &to);
	void forget(const std::string &path);

	const std::set<std::string> &favorites() const { return favorites_; }

private:
	static std::string key(const std::string &path);
	std::set<std::string> favorites_;
};

} // namespace hl
