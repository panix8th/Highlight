#pragma once

#include <imgui.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

namespace ui {

struct Texture {
	ID3D11ShaderResourceView *srv = nullptr;
	ID3D11Texture2D *tex = nullptr;
	int w = 0;
	int h = 0;
	bool dynamic = false;

	ImTextureID id() const { return (ImTextureID)(intptr_t)srv; }
	explicit operator bool() const { return srv != nullptr; }
};

void textures_init(ID3D11Device *device, ID3D11DeviceContext *ctx);
bool texture_create(Texture &t, const uint8_t *bgra, int w, int h, bool dynamic = false);
/* Re-uploads pixels, recreating the texture if the size changed. */
bool texture_update(Texture &t, const uint8_t *bgra, int w, int h);
void texture_release(Texture &t);

/* Clip thumbnails: decoded on a worker thread, cached as small JPEGs on disk. */
class ThumbCache {
public:
	~ThumbCache();
	void start();
	void stop();
	/* Returns the texture once ready, otherwise queues it and returns nullptr. */
	const Texture *get(const std::string &path, uint64_t stamp);
	void forget(const std::string &path);

private:
	struct Result {
		std::string key;
		std::vector<uint8_t> px;
		int w = 0, h = 0;
		bool ok = false;
	};
	void worker();

	std::map<std::string, Texture> ready_;
	std::map<std::string, bool> requested_;
	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<std::pair<std::string, std::string>> queue_; /* key, path */
	std::deque<Result> results_;
	bool stop_ = false;
};

} // namespace ui
