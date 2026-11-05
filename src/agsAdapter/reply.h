#ifndef AGS_ADAPTER_REPLY_H
#define AGS_ADAPTER_REPLY_H

#include <QJsonObject>
#include <QString>

// The result of one method: a result object, or an error code from protocol 1
// (unknown-method, invalid-params, not-found, failed) with a sentence for the user.
struct Reply {
	QJsonObject result;
	QString     code;
	QString     message;

	[[nodiscard]] bool Ok() const { return code.isEmpty(); }

	static Reply Success(QJsonObject result = {}) { return {std::move(result), {}, {}}; }
	static Reply Error(const QString& code, const QString& message) { return {{}, code, message}; }
	static Reply Invalid(const QString& message) {
		return Error(QStringLiteral("invalid-params"), message);
	}
	static Reply NotFound(const QString& message) {
		return Error(QStringLiteral("not-found"), message);
	}
	static Reply Failed(const QString& message) { return Error(QStringLiteral("failed"), message); }
};

#endif
