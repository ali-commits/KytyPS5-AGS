// kyty-ags-adapter: describes KytyPS5 to AGS over framed JSON on stdin and stdout
// (docs/extensions/STANDARD.md in the AGS repository, protocol 1). It never starts the emulator.
#include "adapter.h"
#include "framing.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QSocketNotifier>

#include <cerrno>
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

void Handle(Adapter& adapter, const QByteArray& payload) {
	QJsonParseError error {};
	const auto      document = QJsonDocument::fromJson(payload, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		fprintf(stderr, "kyty-ags-adapter: ignoring a message that is not a JSON object\n");
		return;
	}
	const auto request = document.object();
	const auto id      = request.value(QStringLiteral("id"));
	if (!id.isDouble()) {
		return; // a notification from AGS; protocol 1 defines none
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
			Handle(adapter, payload);
		}
		if (eof) {
			QCoreApplication::exit(0);
			return;
		}
		notifier.setEnabled(true);
	});
	return QCoreApplication::exec();
}
