#ifndef AGS_ADAPTER_ADAPTER_H
#define AGS_ADAPTER_ADAPTER_H

// Method dispatch for the KytyPS5 AGS adapter (protocol 1) and the Kyty.ini watcher that sends
// library.changed.
#include "actions.h"
#include "adapterState.h"
#include "library.h"
#include "reply.h"

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>

class GameLibrary;
struct GameEntry;

class Adapter final: public QObject {
	Q_OBJECT

public:
	// `notify` writes one notification message (no id) to AGS.
	Adapter(const QString& data_dir, const QString& adapter_dir,
	        std::function<void(const QJsonObject&)> notify, QObject* parent = nullptr);
	~Adapter() override;

	[[nodiscard]] Reply Call(const QString& method, const QJsonObject& params);

private:
	[[nodiscard]] Reply Hello(const QJsonObject& params) const;
	// Validated runtime root and an up-to-date library; false with `reply` set on failure.
	[[nodiscard]] bool Ready(Reply* reply);
	[[nodiscard]] bool FindGame(const QJsonObject& params, bool required, const GameEntry** game,
	                            Reply* reply);
	void               Watch();

	AdapterState                            m_state;
	QString                                 m_adapter_dir;
	std::function<void(const QJsonObject&)> m_notify;
	QString                                 m_runtime_root;
	std::unique_ptr<GameLibrary>            m_library;
	bool                                    m_library_dirty = true;
	QFileSystemWatcher                      m_watcher;
	QTimer                                  m_debounce;
	CheatFetcher                            m_fetcher;
	AdapterLibrary::ArtCache                m_art_cache;
};

#endif
