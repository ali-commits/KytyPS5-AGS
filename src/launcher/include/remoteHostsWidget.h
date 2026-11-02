#ifndef AGS_REMOTE_HOSTS_WIDGET_H
#define AGS_REMOTE_HOSTS_WIDGET_H
#include <QWidget>
class RemoteClient;
class RemoteHostsWidget final: public QWidget {
	Q_OBJECT
public:
	explicit RemoteHostsWidget(RemoteClient* client, QWidget* parent = nullptr);
};
#endif
