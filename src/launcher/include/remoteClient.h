#ifndef AGS_REMOTE_CLIENT_H
#define AGS_REMOTE_CLIENT_H
#include "libraryPage.h"

#include <QJsonArray>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QProcess>

#include <functional>

// Native CLI owns authentication, pin validation, transport and SDL playback.
// This controller stores only nonsecret profile references and library metadata.
class RemoteClient final: public QObject {
	Q_OBJECT
public:
	struct Profile {
		QString    id, name, endpoint, directory;
		QJsonArray games;
		bool       online   = false;
		quint64    revision = 0;
	};
	struct TestOptions {
		QString executable, settingsFile;
		int     timeoutMs = 15000;
		QString inputSysRoot, inputDeviceRoot;
	};
	explicit RemoteClient(QObject* parent = nullptr);
	explicit RemoteClient(const TestOptions& options, QObject* parent = nullptr);
	~RemoteClient() override;
	static QString Executable();
	static QString DefaultSettingsFile();
	static bool    PairingInfo(const QString& bundle, QString* fingerprint, QString* endpoint,
	                           QString* error);
	QString AddProfile(const QString& name, const QString& endpoint, const QString& directory);
	void    Pair(const QString& bundle, const QString& fingerprint, const QString& name,
	             const QString& endpoint, const QString& newDirectory,
	             const QString& receiverName = QStringLiteral("AGS Linux receiver"));
	void    Refresh(const QString& id);
	void    RefreshAll();
	bool    Forget(const QString& id);
	bool    Play(const QString& compositeId);
	bool    SetQuality(int width, int height, int fps);
	struct Limits {
		int width, height, fps;
	};
	Limits Quality() const { return {m_width, m_height, m_fps}; }
	// Per client device: how this receiver asks the host to stream.
	struct Stream {
		int     bitrateKbps = 0; // 0: automatic (host value)
		QString videoCodec  = QStringLiteral("h264");
		QString audioCodec  = QStringLiteral("opus");
		bool    fec         = false;
		QString latency     = QStringLiteral("balanced");
		QString capture     = QStringLiteral("compositor");
	};
	static bool ValidStream(const Stream& stream);
	Stream      StreamOptions() const { return m_stream; }
	bool        SetStream(const Stream& stream);
	struct Controller {
		QString name, path;
	};
	QList<Controller>  Controllers() const;
	bool               SetController(const QString& path);
	QString            ControllerSelection() const { return m_controllerSelection; }
	QString            Activity() const;
	bool               Busy() const { return !m_ownedProfile.isEmpty() || m_stopping; }
	QList<Profile>     Profiles() const { return m_profiles.values(); }
	QList<LibraryCard> Cards() const;
	void               StopForExit();
signals:
	void Changed();
	void Error(QString message);
	void Stopped(QString error);

private:
	using Completion = std::function<void(QByteArray, QString)>;
	QProcess*              Request(const QStringList& args, Completion complete);
	QStringList            Arguments(const Profile& profile) const;
	void                   Load();
	void                   Save();
	void                   FinishStop();
	void                   InterruptReceiver();
	void                   CloseAfterReceiver();
	QString                m_executable, m_settingsFile, m_inputSysRoot, m_inputDeviceRoot;
	QString                m_controllerSelection = "@auto";
	int                    m_timeoutMs;
	QMap<QString, Profile> m_profiles;
	QMap<QString, QPointer<QProcess>> m_requests;
	QPointer<QProcess>                m_pair;
	QProcess                          m_receiver;
	QString                           m_ownedProfile, m_stopError;
	bool    m_stopping = false, m_closeStarted = false, m_closeFinished = false;
	Stream  m_stream;
	int     m_width = 1920, m_height = 1080, m_fps = 60;
	qint64  m_receiverOutputBytes = 0;
	quint64 m_stopRevision = 0, m_pairRevision = 0, m_receiverRevision = 0;
};
#endif
