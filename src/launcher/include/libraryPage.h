#ifndef AGS_LIBRARY_PAGE_H
#define AGS_LIBRARY_PAGE_H
#include <QByteArray>
#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>
class QListWidget;
class QLineEdit;
class QStackedWidget;
class QLabel;
class QPushButton;
struct LibraryCard {
	QString    id, name, source, gamePath, reason;
	bool       ready = false;
	QString    sourceId;
	QByteArray artwork;
};
// Presentation only: launch authority remains with the existing source-specific owner.
class LibraryPage final: public QWidget {
	Q_OBJECT
public:
	explicit LibraryPage(QWidget* parent = nullptr);
	void    SetGames(const QList<LibraryCard>& games);
	void    SetLaunchEnabled(bool enabled);
	QString SelectedId() const;
	void    SelectId(const QString& id);
	void    HandleController(const QSet<QString>& pressed);
signals:
	void SelectionChanged(QString id);
	void PlayRequested(QString gamePath);
	void GameActionRequested(QString stableId, QString source);
	void StreamingRequested();
	void SettingsRequested();

private:
	void               ShowDetails();
	void               Back();
	void               ApplyFilter();
	QList<LibraryCard> m_games;
	QListWidget*       m_list;
	QLineEdit*         m_search;
	QStackedWidget*    m_pages;
	QLabel *           m_title, *m_source, *m_reason;
	QPushButton*       m_play;
	bool               m_launchEnabled = true;
	QSet<QString>      m_previousButtons;
	QLabel*            m_empty;
	QLabel*            m_artwork;
};
#endif
