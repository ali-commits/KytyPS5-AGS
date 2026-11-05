#ifndef AGS_ADAPTER_FRAMING_H
#define AGS_ADAPTER_FRAMING_H

// AGS adapter protocol 1 framing (STANDARD.md section 5.1): a 4-byte big-endian length, then
// that many bytes of UTF-8 JSON, at most 1 MiB.
#include <QByteArray>
#include <QJsonObject>

namespace AgsFrame {

inline constexpr qsizetype MaxMessage = qsizetype {1} << 20u;

// One length-prefixed message for `object`; empty when the JSON is larger than MaxMessage.
[[nodiscard]] QByteArray Encode(const QJsonObject& object);

enum class Next { Message, NeedMore, TooLarge };

// Takes one complete message off the front of `buffer` into `payload`. TooLarge leaves the
// buffer as it was: the stream cannot be resynchronised, so the caller stops.
[[nodiscard]] Next Take(QByteArray& buffer, QByteArray& payload);

} // namespace AgsFrame

#endif
