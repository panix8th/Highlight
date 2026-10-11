#include "probe.h"
#include "av_util.h"

#include "../common/strings.h"

#include <filesystem>

namespace hl::media {

MediaInfo probe(const std::string &path)
{
	MediaInfo info;
	std::error_code ec;
	info.file_size = std::filesystem::file_size(widen(path), ec);

	InputPtr in = open_input(path, &info.error);
	if (!in)
		return info;

	AVFormatContext *ctx = in.get();
	if (ctx->duration > 0)
		info.duration = (double)ctx->duration / AV_TIME_BASE;

	for (unsigned i = 0; i < ctx->nb_streams; i++) {
		AVStream *st = ctx->streams[i];
		AVCodecParameters *par = st->codecpar;
		const AVCodecDescriptor *desc = avcodec_descriptor_get(par->codec_id);
		AVDictionaryEntry *title = av_dict_get(st->metadata, "title", nullptr, 0);
		if (!title)
			title = av_dict_get(st->metadata, "handler_name", nullptr, 0);

		if (par->codec_type == AVMEDIA_TYPE_VIDEO && info.video_index < 0 &&
		    !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
			info.video_index = (int)i;
			info.video_codec = desc ? desc->name : "";
			info.width = par->width;
			info.height = par->height;
			AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
			info.fps = fr.den ? av_q2d(fr) : 0;
			info.video_bitrate = par->bit_rate;
			if (info.duration <= 0 && st->duration > 0)
				info.duration = (double)st->duration * av_q2d(st->time_base);
		} else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
			MediaInfo::Audio a;
			a.index = (int)i;
			a.codec = desc ? desc->name : "";
			a.sample_rate = par->sample_rate;
			a.channels = par->ch_layout.nb_channels;
			a.title = title && title->value ? title->value : "";
			/* MP4 writers often put a generic handler name in there. */
			if (a.title == "SoundHandler" || a.title.empty())
				a.title = "Track " + std::to_string(info.audio.size() + 1);
			info.audio.push_back(a);
		}
	}

	for (unsigned i = 0; i < ctx->nb_chapters; i++) {
		AVChapter *ch = ctx->chapters[i];
		MediaInfo::Mark m;
		m.time = (double)ch->start * av_q2d(ch->time_base);
		AVDictionaryEntry *t = av_dict_get(ch->metadata, "title", nullptr, 0);
		m.title = t && t->value ? t->value : "";
		info.chapters.push_back(m);
	}

	if (info.video_bitrate <= 0 && info.duration > 0)
		info.video_bitrate = (int64_t)((double)info.file_size * 8.0 / info.duration);

	info.ok = info.video_index >= 0;
	if (!info.ok && info.error.empty())
		info.error = "No video stream";
	return info;
}

} // namespace hl::media
