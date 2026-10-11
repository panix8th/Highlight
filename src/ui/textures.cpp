#include "textures.h"

#include "../common/image.h"
#include "../common/paths.h"
#include "../common/strings.h"
#include "../media/thumbnail.h"

#include <windows.h>
#include <objbase.h>
#include <d3d11.h>

#include <filesystem>

namespace ui {

static ID3D11Device *g_device = nullptr;
static ID3D11DeviceContext *g_ctx = nullptr;

void textures_init(ID3D11Device *device, ID3D11DeviceContext *ctx)
{
	g_device = device;
	g_ctx = ctx;
}

bool texture_create(Texture &t, const uint8_t *bgra, int w, int h, bool dynamic)
{
	texture_release(t);
	if (!g_device || w <= 0 || h <= 0)
		return false;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = (UINT)w;
	desc.Height = (UINT)h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;

	D3D11_SUBRESOURCE_DATA init = {};
	init.pSysMem = bgra;
	init.SysMemPitch = (UINT)w * 4;
	if (FAILED(g_device->CreateTexture2D(&desc, bgra ? &init : nullptr, &t.tex)))
		return false;

	D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = desc.Format;
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	if (FAILED(g_device->CreateShaderResourceView(t.tex, &srv, &t.srv))) {
		t.tex->Release();
		t.tex = nullptr;
		return false;
	}
	t.w = w;
	t.h = h;
	t.dynamic = dynamic;
	return true;
}

bool texture_update(Texture &t, const uint8_t *bgra, int w, int h)
{
	if (!t.srv || !t.dynamic || t.w != w || t.h != h)
		return texture_create(t, bgra, w, h, true);
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(g_ctx->Map(t.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
		return false;
	for (int y = 0; y < h; y++)
		memcpy((uint8_t *)m.pData + (size_t)y * m.RowPitch, bgra + (size_t)y * w * 4, (size_t)w * 4);
	g_ctx->Unmap(t.tex, 0);
	return true;
}

void texture_release(Texture &t)
{
	if (t.srv)
		t.srv->Release();
	if (t.tex)
		t.tex->Release();
	t = Texture{};
}

/* ------------------------------------------------------------------ */

static std::string cache_key(const std::string &path, uint64_t stamp)
{
	uint64_t h = 1469598103934665603ull;
	for (unsigned char c : hl::to_lower(path)) {
		h ^= c;
		h *= 1099511628211ull;
	}
	h ^= stamp;
	h *= 1099511628211ull;
	char buf[32];
	snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
	return buf;
}

ThumbCache::~ThumbCache()
{
	stop();
}

void ThumbCache::start()
{
	stop_ = false;
	thread_ = std::thread(&ThumbCache::worker, this);
}

void ThumbCache::stop()
{
	{
		std::lock_guard lock(mutex_);
		stop_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
	for (auto &[k, t] : ready_)
		texture_release(t);
	ready_.clear();
	requested_.clear();
}

const Texture *ThumbCache::get(const std::string &path, uint64_t stamp)
{
	/* upload finished decodes (a few per frame keeps the UI smooth) */
	{
		std::lock_guard lock(mutex_);
		for (int n = 0; n < 4 && !results_.empty(); n++) {
			Result r = std::move(results_.front());
			results_.pop_front();
			Texture t;
			if (r.ok)
				texture_create(t, r.px.data(), r.w, r.h);
			ready_[r.key] = t;
		}
	}

	std::string key = cache_key(path, stamp);
	auto it = ready_.find(key);
	if (it != ready_.end())
		return it->second ? &it->second : nullptr;
	if (!requested_[key]) {
		requested_[key] = true;
		std::lock_guard lock(mutex_);
		queue_.emplace_back(key, path);
		cv_.notify_one();
	}
	return nullptr;
}

void ThumbCache::forget(const std::string &path)
{
	(void)path; /* keys include the timestamp, so stale entries are simply never asked for again */
}

void ThumbCache::worker()
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const auto dir = hl::paths::thumbs_dir();
	for (;;) {
		std::pair<std::string, std::string> job;
		{
			std::unique_lock lock(mutex_);
			cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
			if (stop_)
				break;
			/* newest requests first: those are the cards on screen */
			job = queue_.back();
			queue_.pop_back();
		}

		Result r;
		r.key = job.first;
		std::wstring file = (dir / (hl::widen(job.first) + L".jpg")).wstring();
		if (std::filesystem::exists(file))
			r.ok = hl::read_image(file, r.px, r.w, r.h);
		if (!r.ok) {
			r.ok = hl::media::decode_thumbnail(job.second, 1.0, 400, 225, r.px, r.w, r.h);
			if (r.ok)
				hl::write_image(file, r.px.data(), r.w, r.h, r.w * 4, "jpg", 0.82f);
		}
		std::lock_guard lock(mutex_);
		results_.push_back(std::move(r));
	}
	CoUninitialize();
}

} // namespace ui
