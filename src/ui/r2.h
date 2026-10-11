#pragma once

/* Share links: clips are uploaded to the user's own Cloudflare R2 bucket (S3 API,
 * signed with the user's API token) and shared through the bucket's public address.
 * Runs only when the user uploads something; nothing else is sent. */

#include <cstdint>
#include <functional>
#include <string>

namespace hl {
struct ShareSettings;
}

namespace ui {

struct R2Config {
	std::string host;       /* <account>.r2.cloudflarestorage.com (or .eu.) */
	std::string access_key; /* API token: Access Key ID */
	std::string secret;     /* API token: Secret Access Key */
	std::string bucket;
	std::string public_url; /* https://pub-....r2.dev or a custom domain, no trailing slash */

	static R2Config from(const hl::ShareSettings &s);
	/* empty when complete, otherwise what is missing */
	std::string missing() const;
	std::string link(const std::string &key) const;
};

/* Object name for a new upload: "<yyyy-mm-dd>/<name>-<random>.<ext>". The random part
 * keeps links unguessable (the bucket is public, but cannot be listed). */
std::string r2_new_key(const std::string &display_name, const std::string &file_path);

/* Uploads a file with one signed PUT. progress(0..1) returning false cancels.
 * Returns false with a readable reason in *err. */
bool r2_upload(const R2Config &c, const std::string &file, const std::string &key,
	       const std::function<bool(float)> &progress, std::string *err);
bool r2_delete(const R2Config &c, const std::string &key, std::string *err);
/* Uploads a small test file, opens it through the public address, deletes it. */
bool r2_test(const R2Config &c, std::string *err);

bool copy_text_to_clipboard(const std::string &text);

} // namespace ui
