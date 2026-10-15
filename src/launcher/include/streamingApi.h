#ifndef KYTY_STREAMING_API_H
#define KYTY_STREAMING_API_H
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSslCertificate>

#include <functional>
class StreamingApi: public QObject {
	Q_OBJECT
public:
	using Result = std::function<void(const QJsonObject&, const QString&)>;
	explicit StreamingApi(QObject* parent = nullptr);
	void Configure(int port, const QSslCertificate& certificate, const QString& user,
	               const QString& password);
	void Request(const QByteArray& method, const QString& path, const QJsonObject& body,
	             Result done);
	void SubmitPin(const QString& pairing_id, const QString& pin, const QString& name, Result done);
	static QString ValidatePin(const QString& pairing_id, const QString& pin, const QString& name);
	void           Abort();

private:
	QNetworkAccessManager m_network;
	int                   m_port = 0;
	QSslCertificate       m_certificate;
	QByteArray            m_auth;
};
#endif
