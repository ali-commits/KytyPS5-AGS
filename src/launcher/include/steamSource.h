#pragma once
#include "streamingCatalog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <functional>
#include <optional>
#ifdef __unix__
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Only public installation manifests and an explicit owner recipe file are read.
namespace SteamSource {
struct Record {
	QString                             id, app_id, name, directory, reason;
	bool                                installed = false;
	std::optional<StreamingLaunchEntry> launch;
};
inline QString Id(const QString& app) {
	return QString::fromLatin1(
	    QCryptographicHash::hash(("steam:" + app).toUtf8(), QCryptographicHash::Sha256).toHex());
}
inline QByteArray Read(const QString& path) {
	constexpr qsizetype limit = 1024 * 1024;
#ifdef __unix__
	// Inspect the opened inode, not a prior path lookup. NONBLOCK prevents a FIFO
	// from blocking before fstat; NOFOLLOW rejects a substituted final symlink.
	// Parent Steam root aliases remain supported.
	const int fd =
	    ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
	if (fd < 0) return {};
	struct Close {
		int fd;
		~Close() { ::close(fd); }
	} close {fd};
	struct stat metadata {};
	if (::fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
	    metadata.st_size > limit)
		return {};
	QByteArray bytes;
	char       buffer[16384];
	while (bytes.size() <= limit) {
		const auto count =
		    ::read(fd, buffer, std::min<qsizetype>(sizeof(buffer), limit + 1 - bytes.size()));
		if (count < 0) {
			if (errno == EINTR) continue;
			return {};
		}
		if (count == 0) return bytes;
		bytes.append(buffer, count);
	}
	return {};
#else
	QFile f(path);
	if (QFileInfo(path).isSymLink() || !QFileInfo(path).isFile() || !f.open(QIODevice::ReadOnly) ||
	    f.size() > limit)
		return {};
	const auto bytes = f.read(limit + 1);
	return bytes.size() <= limit ? bytes : QByteArray {};
#endif
}
// Strict small KeyValues reader: quoted scalars/objects, escapes, comments; reject malformed input.
inline QJsonObject Parse(const QByteArray& bytes) {
	const QString s     = QString::fromUtf8(bytes);
	qsizetype     p     = 0;
	bool          ok    = true;
	auto          space = [&] {
		while (p < s.size()) {
			if (s[p].isSpace()) {
				++p;
				continue;
			}
			if (s.mid(p, 2) == "//") {
				while (p < s.size() && s[p] != '\n')
					++p;
				continue;
			}
			break;
		}
	};
	auto token = [&]() {
		space();
		QString out;
		if (p >= s.size() || s[p++] != '"') {
			ok = false;
			return out;
		}
		while (p < s.size()) {
			auto c = s[p++];
			if (c == '"') return out;
			if (c == '\\') {
				if (p >= s.size()) break;
				c = s[p++];
			}
			out += c;
		}
		ok = false;
		return out;
	};
	std::function<QJsonObject(bool, int)> object = [&](bool nested, int depth) {
		QJsonObject out;
		if (depth > 16) {
			ok = false;
			return out;
		}
		while (ok) {
			space();
			if (p == s.size()) {
				if (nested) ok = false;
				break;
			}
			if (s[p] == '}') {
				++p;
				if (!nested) ok = false;
				break;
			}
			auto key = token();
			space();
			if (!ok || p >= s.size()) {
				ok = false;
				break;
			}
			if (out.contains(key)) {
				ok = false;
				break;
			}
			if (s[p] == '{') {
				++p;
				out[key] = object(true, depth + 1);
			} else
				out[key] = token();
		}
		return out;
	};
	auto out = object(false, 0);
	return ok ? out : QJsonObject {};
}
inline QList<Record> Discover(const QStringList& roots, const QJsonObject& recipes) {
	QStringList   libraries = roots;
	QSet<QString> seenLibraries, seenApps;
	QList<Record> out;
	for (const auto& root: roots) {
		const auto folders =
		    Parse(Read(QDir(root).filePath("steamapps/libraryfolders.vdf")))["libraryfolders"]
		        .toObject();
		for (auto i = folders.begin(); i != folders.end(); ++i) {
			const auto path = i.value().isObject() ? i.value().toObject()["path"].toString()
			                                       : i.value().toString();
			if (QDir::isAbsolutePath(path)) libraries.append(path);
		}
	}
	for (const auto& root: libraries) {
		auto canonical = QFileInfo(root).canonicalFilePath();
		if (canonical.isEmpty() || seenLibraries.contains(canonical)) continue;
		seenLibraries.insert(canonical);
		QDir apps(QDir(root).filePath("steamapps"));
		for (const auto& file: apps.entryList({"appmanifest_*.acf"}, QDir::Files, QDir::Name)) {
			auto app  = Parse(Read(apps.filePath(file)))["AppState"].toObject();
			auto id   = app["appid"].toString();
			auto name = app["name"].toString();
			auto dir  = app["installdir"].toString();
			if (!QRegularExpression("^[1-9][0-9]*$").match(id).hasMatch() ||
			    file != "appmanifest_" + id + ".acf" || name.isEmpty() || dir.isEmpty() ||
			    dir.contains('/') || dir.contains('\\') || dir == ".." || dir == "." ||
			    seenApps.contains(id))
				continue;
			if (QRegularExpression(
			        "^(Steamworks|Steam Linux Runtime|Proton|Steam Controller|SteamVR)(\\b|$)",
			        QRegularExpression::CaseInsensitiveOption)
			        .match(name)
			        .hasMatch())
				continue;
			Record r;
			r.id         = Id(id);
			r.app_id     = id;
			r.name       = name;
			r.directory  = apps.filePath("common/" + dir);
			bool stateOk = false;
			auto state   = app["StateFlags"].toString().toUInt(&stateOk);
			r.installed  = stateOk && (state & 4) && QFileInfo(r.directory).isDir();
			r.reason     = r.installed ? "owned-launch-recipe-required" : "installation-incomplete";
			auto        recipe  = recipes[id].toObject();
			auto        program = recipe["executable"].toString();
			auto        cwd     = recipe["cwd"].toString();
			auto visibility = recipe.value("process_visibility").toString("private");
			QStringList args;
			bool        valid = recipe["args"].isArray() &&
                            (!recipe.contains("process_visibility") || recipe["process_visibility"].isString()) &&
                            (visibility == "private" || visibility == "host_steam");
			for (auto a: recipe["args"].toArray()) {
				if (!a.isString() || a.toString().contains(QChar('\0')) ||
				    a.toString() == "-applaunch" || a.toString().startsWith("steam://") ||
                    a.toString() == "--streaming-only" ||
				    a.toString().startsWith("--streaming-only=") ||
				    a.toString() == "--streaming-socket" ||
				    a.toString().startsWith("--streaming-socket="))
					valid = false;
				args.append(a.toString());
			}
			// No Steam IPC handoff: process lifetime must belong to the launch transaction.
			auto base = QFileInfo(program).fileName().toLower();
			valid = valid && QDir::isAbsolutePath(program) && QFileInfo(program).isFile() &&
			        QFileInfo(program).isExecutable() && !program.contains(QChar('\0')) &&
			        QDir::isAbsolutePath(cwd) && QFileInfo(cwd).isDir() &&
			        QFileInfo(cwd).canonicalFilePath() == QDir::cleanPath(cwd) && base != "steam" &&
			        base != "steam.sh";
			QJsonObject env = recipe["env"].toObject();
			if (recipe.contains("env") && !recipe["env"].isObject()) valid = false;
			const QSet<QString> allowed = {"SteamAppId", "SteamGameId", "STEAM_COMPAT_DATA_PATH",
			                               "STEAM_COMPAT_CLIENT_INSTALL_PATH",
			                               "STEAM_COMPAT_INSTALL_PATH"};
			for (auto i = env.begin(); i != env.end(); ++i) {
				if (!allowed.contains(i.key()) || !i.value().isString() ||
				    i.value().toString().contains(QChar('\0')))
					valid = false;
				if ((i.key() == "SteamAppId" || i.key() == "SteamGameId") &&
				    i.value().toString() != id)
					valid = false;
				if (i.key().startsWith("STEAM_COMPAT_") &&
				    !QDir::isAbsolutePath(i.value().toString()))
					valid = false;
			}
			if (r.installed && valid) {
				r.launch = StreamingLaunchEntry {r.id, r.name, program, args, cwd, {}, env, visibility};
				r.reason.clear();
			} else if (!recipe.isEmpty() && r.installed)
				r.reason = "invalid-owned-launch-recipe";
			seenApps.insert(id);
			out.append(r);
		}
	}
	return out;
}
} // namespace SteamSource
