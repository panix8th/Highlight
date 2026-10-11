#include "clip_writer.h"

#include "../common/log.h"
#include "../media/av_util.h"

#include <algorithm>
#include <cmath>

namespace hl {

static size_t leb128(const uint8_t *p, size_t avail, uint64_t *value)
{
	uint64_t v = 0;
	for (size_t i = 0; i < 8 && i < avail; i++) {
		v |= (uint64_t)(p[i] & 0x7f) << (i * 7);
		if (!(p[i] & 0x80)) {
			*value = v;
			return i + 1;
		}
	}
	return 0;
}

bool extract_codec_headers(const std::string &codec, const uint8_t *data, size_t size, std::vector<uint8_t> *out)
{
	out->clear();
	if (!data || size < 4)
		return false;

	if (codec == "av1") {
		/* Low-overhead OBU stream: keep the sequence header OBU. */
		size_t pos = 0;
		while (pos < size) {
			const uint8_t hdr = data[pos];
			const int type = (hdr >> 3) & 0x0f;
			const bool ext = (hdr >> 2) & 1;
			const bool has_size = (hdr >> 1) & 1;
			size_t head = 1 + (ext ? 1 : 0);
			uint64_t len = size - pos - head;
			if (has_size) {
				size_t n = leb128(data + pos + head, size - pos - head, &len);
				if (!n)
					return false;
				head += n;
			}
			if (pos + head + len > size)
				return false;
			if (type == 1) { /* OBU_SEQUENCE_HEADER */
				out->assign(data + pos, data + pos + head + (size_t)len);
				return true;
			}
			pos += head + (size_t)len;
		}
		return false;
	}

	const bool hevc = codec == "hevc";
	if (!hevc && codec != "h264")
		return false;

	/* Annex B: copy every parameter-set NAL unit, with 4-byte start codes. */
	auto next_start = [&](size_t from, size_t *sc_len) -> size_t {
		for (size_t i = from; i + 3 <= size; i++) {
			if (data[i] == 0 && data[i + 1] == 0) {
				if (data[i + 2] == 1) {
					*sc_len = 3;
					return i;
				}
				if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) {
					*sc_len = 4;
					return i;
				}
			}
		}
		*sc_len = 0;
		return size;
	};
	size_t sc = 0;
	size_t pos = next_start(0, &sc);
	while (pos < size) {
		const size_t nal = pos + sc;
		size_t sc2 = 0;
		size_t end = next_start(nal, &sc2);
		if (nal < end) {
			const int type = hevc ? (data[nal] >> 1) & 0x3f : data[nal] & 0x1f;
			const bool ps = hevc ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
			if (ps) {
				static const uint8_t start_code[4] = {0, 0, 0, 1};
				out->insert(out->end(), start_code, start_code + 4);
				out->insert(out->end(), data + nal, data + end);
			}
		}
		pos = end;
		sc = sc2;
	}
	return !out->empty();
}

ClipResult write_clip(DiskRing &ring, const ClipJob &job)
{
	ClipResult res;
	res.id = job.req.id;
	res.path = job.req.path;
	res.game = job.req.game;

	if (job.streams.empty()) {
		res.error = "The buffer is not running";
		return res;
	}

	const int64_t from = job.req.press_sys_usec - (int64_t)job.req.pre_seconds * 1000000;
	const int64_t to = job.req.press_sys_usec + (int64_t)job.req.post_seconds * 1000000;

	media::Mp4Writer writer;
	bool opened = false;
	bool failed = false;
	std::string err;

	int64_t k_pts = 0;
	int64_t video_start_usec = 0;
	int64_t video_start_sys = 0;
	int64_t last_video_end = 0;
	int32_t video_den = 1;

	struct TrackState {
		bool started = false;
		int64_t first_dts = 0;
		int64_t base = 0;
	};
	std::vector<TrackState> tracks(kMaxClipTracks);

	auto fn = [&](const RingRecord &h, const uint8_t *data) -> bool {
		if (!opened) {
			if (!(h.type == 0 && h.keyframe))
				return true;
			k_pts = h.pts;
			video_den = h.tb_den;
			int64_t pts_offset_usec = (h.pts - h.dts) * 1000000 / std::max(1, h.tb_den);
			video_start_usec = h.dts_usec + pts_offset_usec;
			video_start_sys = h.sys_dts_usec + pts_offset_usec;

			std::vector<media::Chapter> chapters;
			double clip_len = (double)(to - video_start_sys) / 1e6;
			std::vector<double> marks;
			for (int64_t b : job.bookmarks) {
				double t = (double)(b - video_start_sys) / 1e6;
				if (t > 0.05 && t < clip_len)
					marks.push_back(t);
			}
			std::sort(marks.begin(), marks.end());
			if (!marks.empty()) {
				chapters.push_back({0.0, marks[0], "Start"});
				for (size_t i = 0; i < marks.size(); i++) {
					double end = i + 1 < marks.size() ? marks[i + 1] : clip_len;
					chapters.push_back({marks[i], end, "Bookmark " + std::to_string(i + 1)});
				}
			}
			res.bookmarks = (int)marks.size();

			std::vector<media::StreamDesc> streams = job.streams;
			if (streams[0].extradata.empty()) {
				std::vector<uint8_t> headers;
				if (extract_codec_headers(streams[0].codec, data, h.size, &headers)) {
					streams[0].extradata = std::move(headers);
				} else {
					err = "The video encoder has not provided its codec headers";
					failed = true;
					return false;
				}
			}
			if (!writer.open(job.req.path, job.req.format, streams, chapters, &err)) {
				failed = true;
				return false;
			}
			opened = true;
		}

		if (h.type == 0) {
			int64_t pts = h.pts - k_pts;
			int64_t dts = h.dts - k_pts;
			if (!writer.write(0, data, h.size, pts, dts, h.tb_den, h.keyframe != 0)) {
				failed = true;
				err = "Could not write video";
				return false;
			}
			int64_t frame = std::max<int64_t>(1, h.tb_num);
			last_video_end = std::max(last_video_end, pts + frame);
		} else {
			int stream = h.track < job.track_to_stream.size() ? job.track_to_stream[h.track] : -1;
			if (stream < 0)
				return true;
			TrackState &t = tracks[h.track];
			if (!t.started) {
				int64_t rel_usec = h.dts_usec - video_start_usec;
				if (rel_usec < 0)
					return true;
				t.started = true;
				t.first_dts = h.dts;
				t.base = (int64_t)llround((double)rel_usec * h.tb_den / 1e6);
			}
			int64_t ts = h.dts - t.first_dts + t.base;
			if (!writer.write(stream, data, h.size, ts, ts, h.tb_den, true)) {
				failed = true;
				err = "Could not write audio";
				return false;
			}
		}
		return true;
	};

	if (!ring.read_range(from, to, fn, &err) || failed || !opened) {
		writer.abort();
		res.error = err.empty() ? "Nothing to save yet" : err;
		return res;
	}

	if (!writer.finish(&err)) {
		res.error = err;
		return res;
	}

	/* Read the file back before anyone is told it exists. */
	std::string verr;
	if (!media::open_input(job.req.path, &verr)) {
		HL_ERROR("Saved clip does not read back: %s", verr.c_str());
		res.error = "The clip was written but cannot be read back. Please send the log to the developers.";
		return res;
	}

	res.ok = true;
	res.duration = (double)last_video_end / std::max(1, video_den);
	res.bytes = writer.bytes_written();
	return res;
}

} // namespace hl
