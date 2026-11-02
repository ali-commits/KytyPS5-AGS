#include "libraryPage.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {
QPixmap Cover(const LibraryCard& game) {
	QPixmap cover;
	if (!game.artwork.isEmpty()) cover.loadFromData(game.artwork);
	if (!cover.isNull())
		return cover.scaled(360, 224, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	cover = QPixmap(360, 224);
	QPainter        painter(&cover);
	QLinearGradient gradient(0, 0, 360, 224);
	gradient.setColorAt(0, QColor("#315f9f"));
	gradient.setColorAt(1, QColor("#252a36"));
	painter.fillRect(cover.rect(), gradient);
	painter.setPen(QColor("#e3edff"));
	auto font = painter.font();
	font.setPixelSize(68);
	font.setBold(true);
	painter.setFont(font);
	painter.drawText(cover.rect(), Qt::AlignCenter, game.name.left(1).toUpper());
	return cover;
}
} // namespace

LibraryPage::LibraryPage(QWidget* parent): QWidget(parent) {
	setObjectName("libraryPage");
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(16, 12, 16, 12);
	layout->setSpacing(10);
	auto* header = new QHBoxLayout;
	auto* logo   = new QLabel(this);
	logo->setPixmap(
	    QPixmap(":/brand/ags-horizontal.png").scaledToWidth(150, Qt::SmoothTransformation));
	logo->setAccessibleName(tr("AGS"));
	header->addWidget(logo);
	header->addStretch();
	auto* settings = new QPushButton(tr("Settings"), this);
	settings->setObjectName("librarySettings");
	header->addWidget(settings);
	connect(settings, &QPushButton::clicked, this, &LibraryPage::SettingsRequested);
	layout->addLayout(header);
	m_pages = new QStackedWidget(this);
	layout->addWidget(m_pages);
	auto* browse       = new QWidget(this);
	auto* browseLayout = new QVBoxLayout(browse);
	auto* heading      = new QLabel(tr("Your library"), browse);
	heading->setObjectName("libraryHeading");
	browseLayout->addWidget(heading);
	auto* search = new QLineEdit(browse);
	m_search     = search;
	search->setObjectName("librarySearch");
	search->setPlaceholderText(tr("Search games"));
	search->setClearButtonEnabled(true);
	search->setAccessibleName(tr("Search games"));
	browseLayout->addWidget(search);
	m_list = new QListWidget(browse);
	m_list->setObjectName("libraryGames");
	m_list->setAccessibleName(tr("Games. Press Enter to view details."));
	m_list->setWordWrap(true);
	m_list->setViewMode(QListView::IconMode);
	m_list->setResizeMode(QListView::Adjust);
	m_list->setMovement(QListView::Static);
	m_list->setIconSize(QSize(180, 100));
	m_list->setGridSize(QSize(202, 188));
	m_list->setUniformItemSizes(true);
	connect(m_list, &QListWidget::currentRowChanged, this,
	        [this] { emit SelectionChanged(SelectedId()); });
	m_list->setSpacing(6);
	m_list->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& point) {
		auto* item = m_list->itemAt(point);
		if (!item) return;
		m_list->setCurrentItem(item);
		QMenu menu(this);
		menu.addAction(tr("View game"), this, &LibraryPage::ShowDetails);
		menu.addAction(tr("Settings"), this, &LibraryPage::SettingsRequested);
		menu.exec(m_list->viewport()->mapToGlobal(point));
	});
	browseLayout->addWidget(m_list);
	m_empty = new QLabel(tr("No games found. Open Settings to add game directories. Steam roots "
	                        "and owned launch recipes are configured in Kyty.ini."),
	                     browse);
	m_empty->setWordWrap(true);
	browseLayout->addWidget(m_empty);
	auto* details = new QPushButton(tr("View game"), browse);
	details->setObjectName("libraryDetails");
	browseLayout->addWidget(details);
	connect(details, &QPushButton::clicked, this, &LibraryPage::ShowDetails);
	connect(m_list, &QListWidget::itemActivated, this, [this] { ShowDetails(); });
	connect(search, &QLineEdit::textChanged, this, [this] { ApplyFilter(); });
	m_pages->addWidget(browse);
	auto* detail       = new QWidget(this);
	auto* detailLayout = new QVBoxLayout(detail);
	auto* back         = new QPushButton(tr("← Library"), detail);
	back->setObjectName("libraryBack");
	detailLayout->addWidget(back, 0, Qt::AlignLeft);
	connect(back, &QPushButton::clicked, this, &LibraryPage::Back);
	m_title = new QLabel(detail);
	m_title->setWordWrap(true);
	m_title->setTextFormat(Qt::PlainText);
	m_title->setObjectName("libraryHeading");
	m_source = new QLabel(detail);
	m_reason = new QLabel(detail);
    m_reason->setObjectName("libraryLaunchReason");
	m_reason->setWordWrap(true);
	m_source->setTextFormat(Qt::PlainText);
	m_reason->setTextFormat(Qt::PlainText);
	auto* body = new QWidget(detail);
	body->setObjectName("libraryDetailBody");
	auto* bodyLayout = new QVBoxLayout(body);
	auto* scroll     = new QScrollArea(detail);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidget(body);
	detailLayout->addWidget(scroll, 1);
	m_artwork = new QLabel(body);
	m_artwork->setAlignment(Qt::AlignLeft);
	m_artwork->setMinimumHeight(112);
	bodyLayout->addWidget(m_artwork);
	bodyLayout->addWidget(m_title);
	bodyLayout->addWidget(m_source);
	bodyLayout->addWidget(m_reason);
	bodyLayout->addStretch();
	m_play = new QPushButton(detail);
	m_play->setObjectName("libraryPlay");
	detailLayout->addWidget(m_play);

	connect(m_play, &QPushButton::clicked, this, [this] {
		int row = m_list->currentRow();
		if (row < 0 || row >= m_games.size() || !m_games[row].ready || !m_launchEnabled) return;
		emit GameActionRequested(m_games[row].id, m_games[row].source);
		if (m_games[row].source == "steam") {
            SetLaunchEnabled(false);
			emit LocalPlayRequested(m_games[row].id);
        } else
			emit PlayRequested(m_games[row].gamePath);
	});
	m_pages->addWidget(detail);
	auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
	escape->setContext(Qt::WidgetWithChildrenShortcut);
	connect(escape, &QShortcut::activated, this, &LibraryPage::Back);
}
QString LibraryPage::SelectedId() const {
	int row = m_list->currentRow();
	return row >= 0 && row < m_games.size() ? m_games[row].id : QString();
}
void LibraryPage::SetGames(const QList<LibraryCard>& games) {
	const auto           selected = SelectedId();
	const QSignalBlocker blockSelection(m_list);
	m_games = games;
	m_empty->setVisible(games.isEmpty());
	m_list->clear();
	int selectedRow = -1;
	for (int i = 0; i < games.size(); ++i) {
		const auto& game = games[i];
		auto*       item = new QListWidgetItem(
		    game.name + "\n" + (game.source == "steam" ? tr("Steam") : tr("PS5")) +
		        (game.ready ? (game.source == "steam" ? tr(" · Ready") : tr(" · Ready"))
				            : tr(" · Setup required")),
		    m_list);
		item->setIcon(QIcon(Cover(game)));
		item->setTextAlignment(Qt::AlignLeft);
		item->setSizeHint(QSize(196, 184));
		if (game.id == selected) selectedRow = i;
	}
	ApplyFilter();
	if (selectedRow < 0 || m_list->item(selectedRow)->isHidden()) {
		selectedRow = -1;
		for (int row = 0; row < m_list->count(); ++row)
			if (!m_list->item(row)->isHidden()) {
				selectedRow = row;
				break;
			}
	}
	m_list->setCurrentRow(selectedRow);
	if (m_pages->currentIndex() == 1) {
		if (m_list->currentRow() >= 0)
			ShowDetails();
		else
			Back();
	}
	m_list->setToolTip(games.isEmpty() ? tr("No games found. Open Settings to add game "
	                                        "directories. Steam roots are configured in Kyty.ini.")
	                                   : QString());
}
void LibraryPage::SetLaunchEnabled(bool enabled) {
	m_launchEnabled = enabled;
	const int row   = m_list->currentRow();
	m_play->setEnabled(enabled && row >= 0 && row < m_games.size() && m_games[row].ready);
}
void LibraryPage::SetLaunchError(const QString& error) {
    if (m_launchError == error) return;
    m_launchError = error;
    if (m_pages->currentIndex() == 1) ShowDetails();
}
void LibraryPage::ShowDetails() {
	int row = m_list->currentRow();
	if (row < 0 || m_list->item(row)->isHidden()) return;
	const auto& game = m_games[row];
	m_title->setText(game.name);
	m_artwork->setPixmap(
	    Cover(game).scaled(280, 150, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	m_source->setText(game.source == "steam" ? tr("Steam · App %1").arg(game.sourceId) : tr("PS5"));
    m_reason->setText(!m_launchError.isEmpty() ? m_launchError : game.ready ? tr("Ready to play on this computer.") : game.reason);
    m_play->setText(tr("Play"));
	SetLaunchEnabled(m_launchEnabled);
	m_pages->setCurrentIndex(1);
	(m_play->isEnabled() ? m_play : findChild<QPushButton*>("libraryBack"))->setFocus();
}
void LibraryPage::Back() {
	m_pages->setCurrentIndex(0);
	m_list->setFocus();
}

void LibraryPage::HandleController(const QSet<QString>& pressed) {
	const auto edges  = pressed - m_previousButtons;
	m_previousButtons = pressed;
	if (!isVisible() || !window()->isActiveWindow()) return;
	if (edges.contains("East")) {
		Back();
		return;
	}
	if (edges.contains("Start")) {
		emit SettingsRequested();
		return;
	}
	auto* focus = QApplication::focusWidget();
	if (!focus || (focus != this && !isAncestorOf(focus))) {
		m_list->setFocus();
		focus = m_list;
	}
	if (edges.contains("South")) {
		if (focus == m_list)
			ShowDetails();
		else if (auto* button = qobject_cast<QPushButton*>(focus))
			button->click();
		return;
	}
	int key = 0;
	if (edges.contains("DpadUp")) key = Qt::Key_Up;
	if (edges.contains("DpadDown")) key = Qt::Key_Down;
	if (edges.contains("DpadLeft")) key = Qt::Key_Left;
	if (edges.contains("DpadRight")) key = Qt::Key_Right;
	if (key && focus == m_list) {
		QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
		QApplication::sendEvent(m_list, &event);
	} else if (key)
		focusNextPrevChild(key == Qt::Key_Down || key == Qt::Key_Right);
}

void LibraryPage::SelectId(const QString& id) {
	for (int row = 0; row < m_games.size(); ++row)
		if (m_games[row].id == id && !m_list->item(row)->isHidden()) {
			m_list->setCurrentRow(row);
			return;
		}
}

void LibraryPage::ApplyFilter() {
	const QSignalBlocker blockSelection(m_list);
	bool                 anyVisible = false;
	for (int row = 0; row < m_list->count(); ++row) {
		const bool hidden = !m_games[row].name.contains(m_search->text(), Qt::CaseInsensitive);
		m_list->item(row)->setHidden(hidden);
		anyVisible |= !hidden;
	}
	if (m_list->currentItem() && m_list->currentItem()->isHidden()) m_list->setCurrentRow(-1);
	m_empty->setVisible(!anyVisible);
	m_empty->setText(m_games.isEmpty()
	                     ? tr("No games found. Open Settings to add game directories. Steam roots "
						      "and owned launch recipes are configured in Kyty.ini.")
						 : tr("No games match your search."));
}
