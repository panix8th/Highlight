#include "disk_ring.h"

#include "../common/log.h"
#include "../common/strings.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>

namespace hl {

static constexpr int64_t kSegmentUsec = 2'000'000;           /* ~2 s per segment file */
static constexpr size_t kWriteBufferBytes = 1024 * 1024;      /* batch writes */
static constexpr size_t kMaxQueuedBytes = 192 * 1024 * 1024; /* disk stalled: drop instead of eating RAM */

namespace {

/* Sequential reader over the first `limit` bytes of a file. */
class FileReader {
public:
	explicit FileReader(HANDLE h, uint64_t limit) : h_(h), limit_(limit) { buf_.resize(1024 * 1024); }

	bool seek(uint64_t offset)
	{
		LARGE_INTEGER li;
		li.QuadPart = (LONGLONG)offset;
		if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN))
			return false;
		file_pos_ = offset;
		pos_ = len_ = 0;
		return true;
	}

	uint64_t tell() const { return file_pos_ - (len_ - pos_); }
	bool at_end() const { return tell() >= limit_; }

	bool read(void *dst, size_t n)
	{
		uint8_t *out = (uint8_t *)dst;
		while (n > 0) {
			if (pos_ == len_ && !fill())
				return false;
			size_t take = std::min(n, len_ - pos_);
			memcpy(out, buf_.data() + pos_, take);
			pos_ += take;
			out += take;
			n -= take;
		}
		return true;
	}

	bool skip(size_t n)
	{
		size_t in_buf = len_ - pos_;
		if (n <= in_buf) {
			pos_ += n;
			return true;
		}
		return seek(tell() + n);
	}

private:
	bool fill()
	{
		if (file_pos_ >= limit_)
			return false;
		DWORD want = (DWORD)std::min<uint64_t>(buf_.size(), limit_ - file_pos_);
		DWORD got = 0;
		if (!ReadFile(h_, buf_.data(), want, &got, nullptr) || got == 0)
			return false;
		file_pos_ += got;
		pos_ = 0;
		len_ = got;
		return true;
	}

	HANDLE h_;
	uint64_t limit_;
	uint64_t file_pos_ = 0;
	std::vector<uint8_t> buf_;
	size_t pos_ = 0;
	size_t len_ = 0;
};

struct HandleGuard {
	HANDLE h = INVALID_HANDLE_VALUE;
	~HandleGuard()
	{
		if (h != INVALID_HANDLE_VALUE)
			CloseHandle(h);
	}
};

} // namespace

DiskRing::~DiskRing()
{
	close();
}

bool DiskRing::open(const std::wstring &dir, int64_t keep_usec, std::string *err)
{
	close();

	dir_ = dir;
	keep_usec_ = keep_usec;

	std::error_code ec;
	std::filesystem::create_directories(dir_, ec);
	if (!std::filesystem::is_directory(dir_, ec)) {
		if (err)
			*err = "Cannot create the buffer folder " + narrow(dir_);
		return false;
	}

	/* Leftovers from a previous run (crash, power loss). */
	for (auto &entry : std::filesystem::directory_iterator(dir_, ec)) {
		if (entry.path().extension() == L".hlr")
			DeleteFileW(entry.path().c_str());
	}

	{
		std::lock_guard lock(queue_mutex_);
		stop_ = false;
		waiting_for_keyframe_ = true;
		queue_.clear();
		queued_bytes_ = 0;
		pushed_seq_ = written_seq_ = flushed_seq_ = 0;
		flush_requested_ = false;
	}
	failed_ = false;
	newest_sys_ = 0;
	disk_bytes_ = 0;
	buffer_.clear();
	buffer_.reserve(kWriteBufferBytes + 64 * 1024);

	writer_ = std::thread(&DiskRing::writer_loop, this);
	HL_INFO("Rolling buffer opened in %s (keeps %lld s)", narrow(dir_).c_str(), (long long)(keep_usec / 1000000));
	return true;
}

void DiskRing::close()
{
	if (writer_.joinable()) {
		{
			std::lock_guard lock(queue_mutex_);
			stop_ = true;
			queue_.clear();
			queued_bytes_ = 0;
		}
		queue_cv_.notify_all();
		flushed_cv_.notify_all();
		writer_.join();
	}

	close_segment();

	std::lock_guard lock(segments_mutex_);
	for (auto &s : segments_)
		DeleteFileW(s.path.c_str());
	segments_.clear();
	disk_bytes_ = 0;
	newest_sys_ = 0;
}

void DiskRing::push(const RingRecord &hdr, const uint8_t *data)
{
	std::unique_lock lock(queue_mutex_);
	if (stop_ || !writer_.joinable())
		return;

	if (waiting_for_keyframe_) {
		if (!(hdr.type == 0 && hdr.keyframe))
			return;
		waiting_for_keyframe_ = false;
	}

	if (queued_bytes_ + hdr.size > kMaxQueuedBytes) {
		HL_WARN("Rolling buffer: disk is not keeping up, dropping packets until the next keyframe");
		waiting_for_keyframe_ = true;
		return;
	}

	Pending p;
	p.hdr = hdr;
	p.hdr.magic = kRingMagic;
	p.data.assign(data, data + hdr.size);
	queued_bytes_ += hdr.size;
	pushed_seq_++;
	queue_.push_back(std::move(p));

	if (hdr.sys_dts_usec > newest_sys_.load())
		newest_sys_ = hdr.sys_dts_usec;

	lock.unlock();
	queue_cv_.notify_one();
}

bool DiskRing::flush()
{
	std::unique_lock lock(queue_mutex_);
	if (!writer_.joinable())
		return false;
	uint64_t target = pushed_seq_;
	flush_requested_ = true;
	queue_cv_.notify_one();
	bool ok = flushed_cv_.wait_for(lock, std::chrono::seconds(10), [&] { return stop_ || flushed_seq_ >= target; });
	return ok && !stop_ && !failed_;
}

int64_t DiskRing::oldest_sys_usec()
{
	std::lock_guard lock(segments_mutex_);
	return segments_.empty() ? 0 : segments_.front().first_sys;
}

void DiskRing::writer_loop()
{
	for (;;) {
		std::deque<Pending> batch;
		bool flush_now = false;
		{
			std::unique_lock lock(queue_mutex_);
			queue_cv_.wait(lock, [&] { return stop_ || !queue_.empty() || flush_requested_; });
			if (stop_)
				break;
			batch.swap(queue_);
			queued_bytes_ = 0;
			flush_now = flush_requested_;
		}

		for (auto &p : batch) {
			if (!write_one(p) && !failed_.exchange(true))
				HL_ERROR("Rolling buffer: write failed (error %lu)", GetLastError());
		}
		if (flush_now)
			flush_buffer();

		{
			std::lock_guard lock(queue_mutex_);
			written_seq_ += batch.size();
			if (flush_now) {
				/* Everything written so far, buffered or not, is now on disk. */
				flushed_seq_ = written_seq_;
				if (flushed_seq_ >= pushed_seq_)
					flush_requested_ = false;
			}
		}
		if (flush_now)
			flushed_cv_.notify_all();
	}
	flush_buffer();
}

bool DiskRing::write_one(Pending &p)
{
	const RingRecord &h = p.hdr;
	if (h.type == 0 && h.keyframe) {
		bool rotate = file_ == nullptr;
		if (!rotate) {
			std::lock_guard lock(segments_mutex_);
			rotate = segments_.empty() || h.sys_dts_usec - segments_.back().first_sys >= kSegmentUsec;
		}
		if (rotate) {
			close_segment();
			if (!start_segment(h.sys_dts_usec))
				return false;
			prune();
		}
	}
	if (!file_)
		return false;

	const uint8_t *hdr = (const uint8_t *)&h;
	buffer_.insert(buffer_.end(), hdr, hdr + sizeof(RingRecord));
	buffer_.insert(buffer_.end(), p.data.begin(), p.data.end());

	{
		std::lock_guard lock(segments_mutex_);
		if (!segments_.empty() && h.sys_dts_usec > segments_.back().last_sys)
			segments_.back().last_sys = h.sys_dts_usec;
	}

	if (buffer_.size() >= kWriteBufferBytes)
		return flush_buffer();
	return true;
}

bool DiskRing::start_segment(int64_t first_sys)
{
	Segment seg;
	seg.id = next_id_++;
	seg.path = dir_ + L"\\seg_" + std::to_wstring(seg.id) + L".hlr";
	seg.first_sys = first_sys;
	seg.last_sys = first_sys;

	HANDLE h = CreateFileW(seg.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
			       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	file_ = h;

	std::lock_guard lock(segments_mutex_);
	segments_.push_back(seg);
	return true;
}

bool DiskRing::flush_buffer()
{
	if (!file_ || buffer_.empty()) {
		buffer_.clear();
		return true;
	}
	const uint8_t *data = buffer_.data();
	size_t left = buffer_.size();
	bool ok = true;
	while (left > 0) {
		DWORD wrote = 0;
		if (!WriteFile((HANDLE)file_, data, (DWORD)std::min<size_t>(left, 64u * 1024 * 1024), &wrote, nullptr) ||
		    wrote == 0) {
			ok = false;
			break;
		}
		data += wrote;
		left -= wrote;
	}
	uint64_t written = buffer_.size() - left;
	buffer_.clear();

	{
		std::lock_guard lock(segments_mutex_);
		if (!segments_.empty())
			segments_.back().bytes_on_disk += written;
	}
	disk_bytes_ += written;
	return ok;
}

void DiskRing::close_segment()
{
	if (!file_)
		return;
	flush_buffer();
	CloseHandle((HANDLE)file_);
	file_ = nullptr;
}

void DiskRing::prune()
{
	int64_t newest = newest_sys_.load();
	int64_t keep = keep_usec_.load();
	std::lock_guard lock(segments_mutex_);
	while (segments_.size() > 2 && newest - segments_.front().last_sys > keep) {
		DeleteFileW(segments_.front().path.c_str());
		disk_bytes_ -= segments_.front().bytes_on_disk;
		segments_.pop_front();
	}
}

bool DiskRing::read_range(int64_t from_sys, int64_t to_sys, const PacketFn &fn, std::string *err)
{
	if (!flush()) {
		if (err)
			*err = "The rolling buffer could not be written to disk";
		return false;
	}

	std::vector<Segment> segs;
	{
		std::lock_guard lock(segments_mutex_);
		segs.assign(segments_.begin(), segments_.end());
	}
	if (segs.empty()) {
		if (err)
			*err = "Nothing has been captured yet";
		return false;
	}

	size_t start = 0;
	for (size_t i = 0; i < segs.size(); i++)
		if (segs[i].first_sys <= from_sys)
			start = i;

	/* Open every needed segment up front: once a handle is open the data stays
	 * readable even if the writer prunes the file meanwhile. */
	std::vector<HandleGuard> handles(segs.size());
	for (size_t i = start; i < segs.size(); i++) {
		if (segs[i].first_sys > to_sys)
			break;
		handles[i].h = CreateFileW(segs[i].path.c_str(), GENERIC_READ,
					   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
					   OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (handles[i].h == INVALID_HANDLE_VALUE) {
			if (i == start && start + 1 < segs.size()) {
				/* pruned between snapshot and open: start one segment later */
				start++;
				continue;
			}
			if (err)
				*err = "Could not open a buffer segment";
			return false;
		}
	}

	/* Pass 1: the last keyframe at or before from_sys inside the start segment. */
	uint64_t start_offset = 0;
	{
		FileReader r(handles[start].h, segs[start].bytes_on_disk);
		RingRecord hdr;
		while (!r.at_end()) {
			uint64_t off = r.tell();
			if (!r.read(&hdr, sizeof(hdr)) || hdr.magic != kRingMagic)
				break;
			if (hdr.sys_dts_usec > from_sys)
				break;
			if (hdr.type == 0 && hdr.keyframe)
				start_offset = off;
			if (!r.skip(hdr.size))
				break;
		}
	}

	/* Pass 2: stream packets until to_sys. */
	std::vector<uint8_t> payload;
	for (size_t i = start; i < segs.size(); i++) {
		if (handles[i].h == INVALID_HANDLE_VALUE)
			break;
		FileReader r(handles[i].h, segs[i].bytes_on_disk);
		if (i == start && !r.seek(start_offset))
			break;
		else if (i != start && !r.seek(0))
			break;

		RingRecord hdr;
		while (!r.at_end()) {
			if (!r.read(&hdr, sizeof(hdr)))
				break;
			if (hdr.magic != kRingMagic) {
				HL_WARN("Rolling buffer: corrupt record in segment %llu", (unsigned long long)segs[i].id);
				break;
			}
			if (hdr.sys_dts_usec > to_sys)
				return true;
			payload.resize(hdr.size);
			if (hdr.size && !r.read(payload.data(), hdr.size))
				break;
			if (!fn(hdr, payload.data()))
				return true;
		}
	}
	return true;
}

} // namespace hl
