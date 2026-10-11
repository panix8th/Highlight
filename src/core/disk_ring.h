#pragma once

/* The rolling buffer. Encoded packets from OBS are appended to short segment
 * files on disk (each starting with a video keyframe) and old segments are
 * deleted as time moves on. Memory use stays flat no matter how long the
 * clip length is: only a small write queue and per-segment bookkeeping live
 * in RAM. */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hl {

#pragma pack(push, 1)
struct RingRecord {
	uint32_t magic;
	uint32_t size;
	int64_t pts;
	int64_t dts;
	int64_t dts_usec;
	int64_t sys_dts_usec;
	int32_t tb_num;
	int32_t tb_den;
	uint8_t type; /* 0 = video, 1 = audio */
	uint8_t track;
	uint8_t keyframe;
	uint8_t reserved[5];
};
#pragma pack(pop)
static_assert(sizeof(RingRecord) == 56, "RingRecord must stay 56 bytes");

constexpr uint32_t kRingMagic = 0x50524C48; /* "HLRP" */

class DiskRing {
public:
	using PacketFn = std::function<bool(const RingRecord &hdr, const uint8_t *data)>;

	DiskRing() = default;
	~DiskRing();
	DiskRing(const DiskRing &) = delete;
	DiskRing &operator=(const DiskRing &) = delete;

	bool open(const std::wstring &dir, int64_t keep_usec, std::string *err);
	/* Stops the writer and deletes every segment file. */
	void close();
	void set_keep(int64_t keep_usec) { keep_usec_ = keep_usec; }

	/* Called on the OBS encoder thread; copies the payload into the write queue. */
	void push(const RingRecord &hdr, const uint8_t *data);

	/* Blocks until everything pushed so far is on disk. */
	bool flush();

	int64_t newest_sys_usec() const { return newest_sys_.load(); }
	int64_t oldest_sys_usec();
	uint64_t disk_bytes() const { return disk_bytes_.load(); }

	/* Streams the packets that cover [from_sys, to_sys] (system time, microseconds).
	 * Starts at the last video keyframe at or before from_sys, so the result is
	 * always decodable and at least as long as requested (when the buffer has it). */
	bool read_range(int64_t from_sys, int64_t to_sys, const PacketFn &fn, std::string *err);

private:
	struct Segment {
		uint64_t id = 0;
		std::wstring path;
		int64_t first_sys = 0;
		int64_t last_sys = 0;
		uint64_t bytes_on_disk = 0;
	};
	struct Pending {
		RingRecord hdr;
		std::vector<uint8_t> data;
	};

	void writer_loop();
	bool write_one(Pending &p);
	bool start_segment(int64_t first_sys);
	bool flush_buffer();
	void close_segment();
	void prune();

	std::wstring dir_;
	std::atomic<int64_t> keep_usec_{0};

	std::thread writer_;
	std::mutex queue_mutex_;
	std::condition_variable queue_cv_;
	std::condition_variable flushed_cv_;
	std::deque<Pending> queue_;
	size_t queued_bytes_ = 0;
	uint64_t pushed_seq_ = 0;
	uint64_t written_seq_ = 0;
	uint64_t flushed_seq_ = 0;
	bool flush_requested_ = false;
	bool stop_ = false;
	bool waiting_for_keyframe_ = true;

	/* writer-thread state */
	void *file_ = nullptr;
	std::vector<uint8_t> buffer_;
	uint64_t next_id_ = 0;

	std::mutex segments_mutex_;
	std::deque<Segment> segments_;

	std::atomic<int64_t> newest_sys_{0};
	std::atomic<uint64_t> disk_bytes_{0};
	std::atomic<bool> failed_{false};
};

} // namespace hl
