// kyty-ags-adapter: describes KytyPS5 to AGS over framed JSON on stdin and stdout
// (docs/extensions/STANDARD.md in the AGS repository, protocol 1). It never starts the emulator.
//
// Framing rules: a frame larger than 1 MiB, or a frame that is not a JSON object, cannot be
// answered (there is no id to echo) and means the stream is no longer trustworthy: the adapter
// logs to stderr and exits with code 2, and AGS restarts it. A message without `id` is a
// notification and gets no reply. An `id` that is not an integer is answered with
// `invalid-params`, echoing the id as received. EOF exits 0.
#include "adapter.h"
#include "framing.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QSocketNotifier>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace {

int g_out = -1; // the real stdout; fd 1 is redirected to stderr

bool WriteAll(const QByteArray& data) {
	qsizetype done = 0;
	while (done < data.size()) {
		const auto n =
		    ::write(g_out, data.constData() + done, static_cast<size_t>(data.size() - done));
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return false;
		done += n;
	}
	return true;
}

void Send(const QJsonObject& message) {
	auto frame = AgsFrame::Encode(message);
	if (frame.isEmpty() && message.contains(QStringLiteral("id"))) {
		frame = AgsFrame::Encode(
		    QJsonObject {{"id", message[QStringLiteral("id")]},
			             {"error", QJsonObject {{"code", "failed"},
			                                    {"message", "The reply is larger than 1 MiB."}}}});
	}
	if (!frame.isEmpty() && !WriteAll(frame)) {
		fprintf(stderr, "kyty-ags-adapter: cannot write to AGS, stopping\n");
		QCoreApplication::exit(1);
	}
}

bool IsInteger(const QJsonValue& id) {
	const double n = id.toDouble();
	return id.isDouble() && std::isfinite(n) && n == std::floor(n) &&
	       std::fabs(n) < 9007199254740992.0;
}

// False when the stream must stop (the frame is not a JSON object).
bool Handle(Adapter& adapter, const QByteArray& payload) {
	QJsonParseError error {};
	const auto      document = QJsonDocument::fromJson(payload, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		fprintf(stderr, "kyty-ags-adapter: message is not a JSON object, stopping\n");
		return false;
	}
	const auto request = document.object();
	if (!request.contains(QStringLiteral("id"))) {
		return true; // a notification from AGS; protocol 1 defines none
	}
	const auto id = request.value(QStringLiteral("id"));
	if (!IsInteger(id)) {
		Send({{"id", id},
		      {"error",
		       QJsonObject {{"code", "invalid-params"}, {"message", "id must be an integer."}}}});
		return true;
	}
	const auto params = request.value(QStringLiteral("params"));
	Reply      reply;
	if (!request.value(QStringLiteral("method")).isString()) {
		reply = Reply::Invalid(QStringLiteral("method is required."));
	} else if (!params.isUndefined() && !params.isNull() && !params.isObject()) {
		reply = Reply::Invalid(QStringLiteral("params must be an object."));
	} else {
		reply = adapter.Call(request.value(QStringLiteral("method")).toString(), params.toObject());
	}
	if (reply.Ok()) {
		Send({{"id", id}, {"result", reply.result}});
	} else {
		Send({{"id", id},
		      {"error", QJsonObject {{"code", reply.code}, {"message", reply.message}}}});
	}
	return true;
}

} // namespace

int main(int argc, char* argv[]) {
	// Only protocol frames may reach stdout: keep the real stdout private and send anything else
	// that writes to fd 1 (libraries, printf) to stderr, which AGS logs.
	g_out = ::dup(STDOUT_FILENO);
	if (g_out < 0 || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
		fprintf(stderr, "kyty-ags-adapter: cannot set up stdout\n");
		return 1;
	}
	::fcntl(g_out, F_SETFD, FD_CLOEXEC);
	QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName(QStringLiteral("kyty-ags-adapter"));

	Adapter adapter(QDir::currentPath(), QCoreApplication::applicationDirPath(), Send);

	::fcntl(STDIN_FILENO, F_SETFL, ::fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
	QByteArray      buffer;
	QSocketNotifier notifier(STDIN_FILENO, QSocketNotifier::Read);
	QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&] {
		bool eof = false;
		char chunk[65536];
		for (;;) {
			const auto n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
			if (n > 0) {
				buffer.append(chunk, n);
				continue;
			}
			if (n < 0 && errno == EINTR) continue;
			eof = n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK);
			break;
		}
		// A call may run a nested event loop (cheats.fetch); no new request starts meanwhile.
		notifier.setEnabled(false);
		QByteArray payload;
		for (;;) {
			const auto next = AgsFrame::Take(buffer, payload);
			if (next == AgsFrame::Next::NeedMore) break;
			if (next == AgsFrame::Next::TooLarge) {
				fprintf(stderr, "kyty-ags-adapter: message larger than 1 MiB, stopping\n");
				QCoreApplication::exit(2);
				return;
			}
			if (!Handle(adapter, payload)) {
				QCoreApplication::exit(2);
				return;
			}
		}
		if (eof) {
			QCoreApplication::exit(0);
			return;
		}
		notifier.setEnabled(true);
	});
	return QCoreApplication::exec();
}
