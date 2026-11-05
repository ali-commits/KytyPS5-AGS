#ifndef AGS_ADAPTER_STATE_H
#define AGS_ADAPTER_STATE_H

#include <QString>

// Machine values that are not in Kyty.ini, kept in <data>/adapter.json. <data> is the working
// directory AGS starts the adapter in.
class AdapterState {
public:
	explicit AdapterState(QString data_dir);

	[[nodiscard]] const QString& DataDir() const { return m_data_dir; }
	// The runtime root, validated (absolute, an existing directory, no symlink components). The
	// default <data>/runtime is created (0700) on first use. Empty with `error` set otherwise.
	[[nodiscard]] QString RuntimeRoot(QString* error) const;
	// The value as stored (or the default), without validation, for settings.get.
	[[nodiscard]] QString StoredRuntimeRoot() const;
	[[nodiscard]] bool    SetRuntimeRoot(const QString& path, QString* error);
	[[nodiscard]] QString DefaultRuntimeRoot() const;

	// <runtime_root>/Kyty.ini for a validated runtime root.
	[[nodiscard]] static QString IniPath(const QString& runtime_root);

private:
	QString m_data_dir;
};

#endif
