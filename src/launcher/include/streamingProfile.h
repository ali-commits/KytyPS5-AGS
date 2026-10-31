#ifndef KYTY_STREAMING_PROFILE_H
#define KYTY_STREAMING_PROFILE_H
#include "../../streaming/aspectMode.h"
#include "../../streaming/captureOptions.h"

#include <QProcessEnvironment>
#include <QStringList>

// A negotiated streaming launch never rewrites the local game's saved configuration.
namespace StreamingProfile {

// Safe capture bounds for any client-negotiated mode, 16:9 or not (e.g. a 20:9 phone panel).
// Even dimensions keep chroma subsampling and the 4-byte-aligned stream stride happy; the
// ceilings match the GPU capture pool's own 4096 limit while staying within 4K.
inline constexpr unsigned MIN_WIDTH = 640, MAX_WIDTH = 3840;
inline constexpr unsigned MIN_HEIGHT = 360, MAX_HEIGHT = 2160;

inline bool IsSupportedDimension(unsigned value, unsigned lo, unsigned hi) {
	return value >= lo && value <= hi && (value % 2 == 0);
}

inline bool Apply(bool unified, const QProcessEnvironment& client, QStringList* arguments,
                  QProcessEnvironment* environment, QString* error) {
	if (!unified) return true;
	const auto width  = client.value("SUNSHINE_CLIENT_WIDTH");
	const auto height = client.value("SUNSHINE_CLIENT_HEIGHT");
	const auto fps    = client.value("SUNSHINE_CLIENT_FPS");
	bool       wok = false, hok = false, fok = false;
	const auto w    = width.toUInt(&wok);
	const auto h    = height.toUInt(&hok);
	const auto rate = fps.toUInt(&fok);
	if (!wok || !hok || QString::number(w) != width || QString::number(h) != height ||
	    !IsSupportedDimension(w, MIN_WIDTH, MAX_WIDTH) ||
	    !IsSupportedDimension(h, MIN_HEIGHT, MAX_HEIGHT) || !fok || rate == 0 ||
	    QString::number(rate) != fps || !Streaming::IsCaptureFrameRateSupported(rate)) {
		if (error)
			*error = "Streaming needs an even client resolution within 640x360..3840x2160 and a "
			         "supported FPS.";
		return false;
	}
	QStringList rewritten;
	for (qsizetype i = 0; i < arguments->size(); ++i) {
		const auto& token = arguments->at(i);
		if (token == "--screen-width" || token == "--screen-height") {
			if (i + 1 >= arguments->size() || arguments->at(i + 1).startsWith("--")) {
				if (error) *error = "Malformed local screen argument in streaming catalog.";
				return false;
			}
			++i;
		} else if (token != "--fullscreen") {
			rewritten.append(token);
		}
	}
	rewritten << "--screen-width" << width << "--screen-height" << height;
	auto output = *environment;
	output.insert("KYTY_STREAM_WIDTH", width);
	output.insert("KYTY_STREAM_HEIGHT", height);
	output.insert("KYTY_STREAM_FPS", fps);
	output.insert("KYTY_STREAM_ONLY", "1");
	// Per-game presentation mode for a non-16:9 stream. The host seeds it into the environment
	// (or the client may negotiate one); the default is geometry-correct letter/pillarboxing.
	const auto requested =
	    environment->value("KYTY_STREAM_ASPECT", client.value("SUNSHINE_CLIENT_ASPECT"));
	output.insert(
	    "KYTY_STREAM_ASPECT",
	    QString::fromLatin1(
	        Streaming::AspectModeName(Streaming::ParseAspectMode(requested.toStdString())).data()));
	*arguments   = std::move(rewritten);
	*environment = std::move(output);
	return true;
}
} // namespace StreamingProfile
#endif
