#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace hl {

/* A clip uploaded for a share link (Cloudflare R2). */
struct Upload {
	std::string key;  /* object name in the bucket */
	std::string link; /* public address */
	uint64_t bytes = 0;
	int64_t time = 0; /* unix seconds */
};

/* Per-clip metadata that is not stored in the video file itself.
 * Kept in %APPDATA%\Highlight\library.json; shared by the core and the UI. */
class Library {
public:
	static Library load();
	bool save() const;

	bool is_favorite(const std::string &path) const;
	void set_favorite(const std::string &path, bool favorite);
	void rename(const std::string &from, const std::string &to);
	void forget(const std::string &path); /* the clip was deleted (its upload stays online) */

	const std::set<std::string> &favorites() const { return favorites_; }

	/* share links, by clip path */
	const Upload *upload(const std::string &path) const;
	void set_upload(const std::string &path, const Upload &u);
	void remove_upload(const std::string &path);
	uint64_t uploaded_bytes() const;
	size_t upload_count() const { return uploads_.size(); }

private:
	static std::string key(const std::string &path);
	std::set<std::string> favorites_;
	std::map<std::string, Upload> uploads_;
};

} // namespace hl
