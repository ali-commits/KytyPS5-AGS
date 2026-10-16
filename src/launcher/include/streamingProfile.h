#ifndef KYTY_STREAMING_PROFILE_H
#define KYTY_STREAMING_PROFILE_H
#include "../../streaming/captureOptions.h"

#include <QProcessEnvironment>
#include <QStringList>

// A negotiated streaming launch never rewrites the local game's saved configuration.
namespace StreamingProfile {
inline bool Apply(bool unified, const QProcessEnvironment& client, QStringList* arguments,
                  QProcessEnvironment* environment, QString* error) {
	if (!unified) return true;
	const auto        width  = client.value("SUNSHINE_CLIENT_WIDTH");
	const auto        height = client.value("SUNSHINE_CLIENT_HEIGHT");
	const auto        fps    = client.value("SUNSHINE_CLIENT_FPS");
	const QStringList sizes {"1280x720", "1920x1080", "2560x1440", "3840x2160"};
	bool              parsed = false;
	const auto        rate   = fps.toUInt(&parsed);
	if (!sizes.contains(width + 'x' + height) || !parsed || rate == 0 ||
	    QString::number(rate) != fps || !Streaming::IsCaptureFrameRateSupported(rate)) {
		if (error) *error = "Managed streaming needs a supported negotiated resolution and FPS.";
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
	*arguments   = std::move(rewritten);
	*environment = std::move(output);
	return true;
}
} // namespace StreamingProfile
#endif
