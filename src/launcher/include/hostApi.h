#ifndef LAUNCHER_INCLUDE_HOST_API_H_
#define LAUNCHER_INCLUDE_HOST_API_H_

#include "cheatRepository.h"
#include "gameLibrary.h"
#include "hostMetrics.h"
#include "hostSocket.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

// Daemon side of the /kyty/v1/* requests the Sunshine fork forwards over api.sock.
class HostApi: public QObject {
	Q_OBJECT
public:
	struct Status {
		bool    pc_busy = false;
		QString running_id;
	};

	HostApi(GameLibrary* library, std::function<Status()> status, QObject* parent = nullptr);

	// request: {"method":"GET|PUT|POST","path":"/kyty/v1/...","body":{...}?,"if_none_match":"..."?,
	//           "client":{"id":"<uuid>","name":"<pair name>"}?}
	// When "client" is present and valid the request is scoped to that device's own profile;
	// absent, it keeps today's PC behaviour so the laptop client and old hosts keep working.
	void Handle(const QJsonObject& request, std::function<void(HostReply)> done);

	[[nodiscard]] static QString    SettingsRevision(const Configuration& info);
	[[nodiscard]] static QJsonArray SettingsSchema(const Configuration& info);

	// Build a Configuration carrying this client's effective game settings: the game's current
	// settings, overlaid with the client's stored values (or seeded from them when a profile does
	// not exist yet). The catalog uses this to build per-client launch arguments.
	[[nodiscard]] static std::unique_ptr<Configuration> ClientConfig(const Configuration& game,
	                                                                 const QJsonObject&   settings);

	// Trophy progress in the contract shape, read from the same data as the trophy viewer.
	// One trophy's icon PNG; 404 for a hidden trophy that is still locked.
	[[nodiscard]] static HostReply   TrophyIcon(const Configuration& info,
	                                            const QString& runtime_directory, int label, int id,
	                                            const QString& if_none_match);
	[[nodiscard]] static QJsonObject Trophies(const Configuration& info,
	                                          const QString&       runtime_directory);

	static constexpr qsizetype MAX_BODY = 16384;

	// A validated per-request device identity (empty id => PC scope).
	struct Client {
		QString            id;
		QString            name;
		[[nodiscard]] bool scoped() const { return !id.isEmpty(); }
	};

signals:
	// Emitted after a device profile write changes launch arguments, so the daemon can regenerate
	// launch.json's client_games.
	void ClientProfileChanged();

private:
	void Library(const Client& client, const std::function<void(HostReply)>& done) const;
	void Metrics(const std::function<void(HostReply)>& done);
	void Art(const QString& id, const QString& kind, const QString& if_none_match,
	         const std::function<void(HostReply)>& done) const;
	void GetSettings(const GameEntry& game, const Client& client,
	                 const std::function<void(HostReply)>& done) const;
	void PutSettings(const GameEntry& game, const Client& client, const QJsonObject& body,
	                 const std::function<void(HostReply)>& done);
	void ResetSettings(const GameEntry& game, const Client& client,
	                   const std::function<void(HostReply)>& done);
	void GetTrophies(const GameEntry& game, const std::function<void(HostReply)>& done) const;
	void GetCheats(const GameEntry& game, const Client& client,
	               const std::function<void(HostReply)>& done) const;
	void PutCheats(const GameEntry& game, const Client& client, const QJsonObject& body,
	               const std::function<void(HostReply)>& done);
	void GetRemote(const GameEntry& game, std::function<void(HostReply)> done);
	void FetchRemote(const GameEntry& game, const QJsonObject& body,
	                 std::function<void(HostReply)> done);

	// One CheatRepository has a single generation counter, so remote work runs one at a time.
	void Enqueue(std::function<void()> job);
	void Finish();

	GameLibrary*            m_library;
	std::function<Status()> m_status;

	HostMetrics::Sampler m_metrics;

	Cheats::CheatRepository                                 m_repository;
	QHash<QString, QList<Cheats::RemoteFile>>               m_remote;
	QList<std::function<void()>>                            m_pending;
	bool                                                    m_busy = false;
	std::function<void(QList<Cheats::RemoteFile>, QString)> m_on_catalog;
	std::function<void(QByteArray, QString)>                m_on_file;
};

#endif /* LAUNCHER_INCLUDE_HOST_API_H_ */
