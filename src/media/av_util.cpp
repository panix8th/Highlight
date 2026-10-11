#include "av_util.h"

namespace hl::media {

std::string av_error(int code)
{
	char buf[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, buf, sizeof(buf));
	return buf;
}

InputPtr open_input(const std::string &path, std::string *err)
{
	AVFormatContext *ctx = nullptr;
	int ret = avformat_open_input(&ctx, path.c_str(), nullptr, nullptr);
	if (ret < 0) {
		if (err)
			*err = "Could not open " + path + ": " + av_error(ret);
		return nullptr;
	}
	InputPtr in(ctx);
	ret = avformat_find_stream_info(ctx, nullptr);
	if (ret < 0) {
		if (err)
			*err = "Could not read " + path + ": " + av_error(ret);
		return nullptr;
	}
	return in;
}

} // namespace hl::media
