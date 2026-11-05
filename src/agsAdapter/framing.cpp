#include "framing.h"

#include <QJsonDocument>

namespace AgsFrame {

QByteArray Encode(const QJsonObject& object) {
	const auto json = QJsonDocument(object).toJson(QJsonDocument::Compact);
	if (json.size() > MaxMessage) {
		return {};
	}
	const auto n = static_cast<quint32>(json.size());
	QByteArray out;
	out.reserve(json.size() + 4);
	out.append(static_cast<char>((n >> 24u) & 0xffu));
	out.append(static_cast<char>((n >> 16u) & 0xffu));
	out.append(static_cast<char>((n >> 8u) & 0xffu));
	out.append(static_cast<char>(n & 0xffu));
	out.append(json);
	return out;
}

Next Take(QByteArray& buffer, QByteArray& payload) {
	if (buffer.size() < 4) {
		return Next::NeedMore;
	}
	const auto*   b = reinterpret_cast<const unsigned char*>(buffer.constData());
	const quint32 n =
	    (quint32 {b[0]} << 24u) | (quint32 {b[1]} << 16u) | (quint32 {b[2]} << 8u) | quint32 {b[3]};
	if (n > static_cast<quint32>(MaxMessage)) {
		return Next::TooLarge;
	}
	if (buffer.size() < static_cast<qsizetype>(n) + 4) {
		return Next::NeedMore;
	}
	payload = buffer.mid(4, static_cast<qsizetype>(n));
	buffer.remove(0, static_cast<qsizetype>(n) + 4);
	return Next::Message;
}

} // namespace AgsFrame
