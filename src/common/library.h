#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace hl {

/* A clip uploaded for a share link (Cloudflare R2). */
struct Upload {
	std::string key;    /* object name in the bucket */
	std::string link;   /* public address */
	std::string host;   /* R2 endpoint and bucket it went to, so "Delete link" deletes it */
	std::string bucket; /* there even after the settings change */
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
	void forget(const std::string &path); /* the clip was deleted; its link stays online as an older link */

	const std::set<std::string> &favorites() const { return favorites_; }

	/* Share links. A clip has one current link. Links it had before (it was uploaded
	 * again) and links of deleted clips stay online; they are kept as older links so
	 * they can still be deleted. */
	const Upload *upload(const std::string &path) const;
	void set_upload(const std::string &path, const Upload &u); /* a different earlier link becomes older */
	void remove_upload(const std::string &path);
	void add_older(const Upload &u);
	void remove_older(const std::string &object_key);
	const std::vector<Upload> &older_uploads() const { return older_; }
	uint64_t uploaded_bytes() const;
	size_t upload_count() const { return uploads_.size() + older_.size(); }

private:
	static std::string key(const std::string &path);
	std::set<std::string> favorites_;
	std::map<std::string, Upload> uploads_;
	std::vector<Upload> older_;
};

} // namespace hl
