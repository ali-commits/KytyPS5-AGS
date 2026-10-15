#include "streamingApi.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSslConfiguration>

StreamingApi::StreamingApi(QObject* parent): QObject(parent), m_network(this) {
	m_network.setProxy(QNetworkProxy::NoProxy);
}
void StreamingApi::Configure(int port, const QSslCertificate& certificate, const QString& user,
                             const QString& password) {
	Abort();
	m_network.clearConnectionCache();
	m_port        = port;
	m_certificate = certificate;
	m_auth        = "Basic " + (user + ':' + password).toUtf8().toBase64();
}
void StreamingApi::Abort() {
	for (auto* reply: m_network.findChildren<QNetworkReply*>())
		reply->abort();
}
QString StreamingApi::ValidatePin(const QString& id, const QString& pin, const QString& name) {
	if (!QRegularExpression("^[a-fA-F0-9]{32}$").match(id).hasMatch())
		return "Choose a current pending pairing request.";
	if (!QRegularExpression("^[0-9]{4}$").match(pin).hasMatch())
		return "Enter the four-digit PIN shown by Moonlight.";
	if (name.trimmed().isEmpty() || name.toUtf8().size() > 128 || name.contains(QChar::Null))
		return "Client name must contain 1–128 UTF-8 bytes.";
	return {};
}
void StreamingApi::SubmitPin(const QString& id, const QString& pin, const QString& name,
                             Result done) {
	const auto error = ValidatePin(id, pin, name);
	if (!error.isEmpty()) {
		done({}, error);
		return;
	}
	Request("POST", "/api/pin", {{"pairing_id", id}, {"pin", pin}, {"name", name}},
	        std::move(done));
}
void StreamingApi::Request(const QByteArray& method, const QString& path, const QJsonObject& body,
                           Result done) {
	if (m_port < 1024 || m_port > 65535 || m_certificate.isNull() || !path.startsWith("/api/") ||
	    path.contains('?') || path.contains('#')) {
		done({}, "Private management endpoint is not ready.");
		return;
	}
	QNetworkRequest request(QUrl(QStringLiteral("https://127.0.0.1:%1%2").arg(m_port).arg(path)));
	request.setRawHeader("Authorization", m_auth);
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                     QNetworkRequest::ManualRedirectPolicy);
	// Closing an app waits for Sunshine's five-second process shutdown grace.
	request.setTransferTimeout(method == "POST" && path == "/api/apps/close" ? 8000 : 2500);
	auto* reply = m_network.sendCustomRequest(
	    request, method,
	    method == "GET" ? QByteArray {} : QJsonDocument(body).toJson(QJsonDocument::Compact));
	const auto expected = m_certificate;
	connect(reply, &QNetworkReply::sslErrors, this,
	        [reply, expected](const QList<QSslError>& errors) {
		        if (reply->sslConfiguration().peerCertificate() != expected) return;
		        for (const auto& error: errors) {
			        if (error.error() != QSslError::SelfSignedCertificate &&
			            error.error() != QSslError::HostNameMismatch &&
			            error.error() != QSslError::CertificateUntrusted)
				        return;
		        }
		        reply->ignoreSslErrors(errors);
	        });
	connect(reply, &QNetworkReply::encrypted, this, [reply, expected] {
		if (reply->sslConfiguration().peerCertificate() != expected) reply->abort();
	});
	connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
		const int       status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto      bytes  = reply->isOpen() ? reply->readAll() : QByteArray {};
		QString         error;
		QJsonParseError parse;
		const auto      document = QJsonDocument::fromJson(bytes, &parse);
		if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
			error = QStringLiteral("Management request failed (HTTP %1): %2")
			            .arg(status)
			            .arg(reply->errorString());
		else if (parse.error != QJsonParseError::NoError || !document.isObject())
			error = "Management host returned invalid JSON.";
		else if (document.object().contains("status") &&
		         !document.object().value("status").toBool())
			error =
			    document.object().value("error").toString("Management host rejected the request.");
		const auto object = document.object();
		reply->deleteLater();
		done(object, error);
	});
}
