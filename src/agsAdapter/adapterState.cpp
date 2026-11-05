#include "adapterState.h"

#include "gameServices.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

QString StateFile(const QString& data_dir) {
	return QDir(data_dir).filePath(QStringLiteral("adapter.json"));
}

QJsonObject ReadState(const QString& data_dir) {
	QFile file(StateFile(data_dir));
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	return QJsonDocument::fromJson(file.read(64 * 1024)).object();
}

} // namespace

AdapterState::AdapterState(QString data_dir): m_data_dir(QDir::cleanPath(std::move(data_dir))) {}

QString AdapterState::DefaultRuntimeRoot() const {
	return QDir(m_data_dir).filePath(QStringLiteral("runtime"));
}

QString AdapterState::StoredRuntimeRoot() const {
	const auto stored = ReadState(m_data_dir).value(QStringLiteral("runtime_root")).toString();
	return stored.isEmpty() ? DefaultRuntimeRoot() : stored;
}

QString AdapterState::RuntimeRoot(QString* error) const {
	const auto stored = ReadState(m_data_dir).value(QStringLiteral("runtime_root")).toString();
	const auto path   = stored.isEmpty() ? DefaultRuntimeRoot() : QDir::cleanPath(stored);
	if (stored.isEmpty() && !QFileInfo::exists(path)) {
		if (!QDir().mkpath(path) ||
		    !QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
		                                     QFileDevice::ExeOwner)) {
			*error = QStringLiteral("Cannot create the runtime folder %1.").arg(path);
			return {};
		}
	}
	if (!GameServices::SafeRuntime(path)) {
		*error = QStringLiteral(
		             "The runtime folder %1 must be an existing absolute folder without symbolic "
		             "links.")
		             .arg(path);
		return {};
	}
	return path;
}

bool AdapterState::SetRuntimeRoot(const QString& path, QString* error) {
	const auto clean = path.isEmpty() ? QString {} : QDir::cleanPath(path);
	if (!clean.isEmpty() && !GameServices::SafeRuntime(clean)) {
		*error = QStringLiteral(
		    "runtime_root must be an existing absolute folder without symbolic links.");
		return false;
	}
	auto state = ReadState(m_data_dir);
	if (clean.isEmpty() || clean == DefaultRuntimeRoot()) {
		state.remove(QStringLiteral("runtime_root"));
	} else {
		state[QStringLiteral("runtime_root")] = clean;
	}
	QSaveFile file(StateFile(m_data_dir));
	if (!file.open(QIODevice::WriteOnly) ||
	    file.write(QJsonDocument(state).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
		*error = QStringLiteral("Cannot write %1.").arg(StateFile(m_data_dir));
		return false;
	}
	return true;
}

QString AdapterState::IniPath(const QString& runtime_root) {
	return QDir(runtime_root).filePath(QStringLiteral("Kyty.ini"));
}
