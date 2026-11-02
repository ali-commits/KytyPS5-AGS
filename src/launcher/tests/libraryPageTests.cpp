#include "libraryPage.h"
#include "shellNavigation.h"
#include <QVBoxLayout>
#include <QDialog>
#include <QCheckBox>

#include <QApplication>
#include <QBuffer>
#include <QLabel>
#include <QFile>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QProxyStyle>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTest>
class ActivationStyle final: public QProxyStyle {
public:
    explicit ActivationStyle(bool singleClick): m_singleClick(singleClick) {}
    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr, QStyleHintReturn* data = nullptr) const override {
        return hint == SH_ItemView_ActivateItemOnSingleClick ? int(m_singleClick)
            : QProxyStyle::styleHint(hint, option, widget, data);
    }
private:
    bool m_singleClick;
};
class LibraryPageTests: public QObject {
	Q_OBJECT
private slots:
	void initTestCase() {
		QFile style(":/styles/launcher.qss");
		QVERIFY(style.open(QIODevice::ReadOnly));
		qApp->setStyleSheet(QString::fromUtf8(style.readAll()));
	}
    void unselectedCardKeepsIvoryTextOnLightPalette() {
        LibraryPage page;
        auto palette = page.palette();
        palette.setColor(QPalette::Window, QColor("#f6f6f6"));
        palette.setColor(QPalette::WindowText, QColor("#202124"));
        palette.setColor(QPalette::Text, QColor("#202124"));
        page.setPalette(palette);
        page.SetGames({{"a", "Selected title", "ps5", "/a", {}, true},
                       {"b", "Unselected title", "steam", "/b", {}, true}});
        page.resize(960, 540); page.show();
        auto* list = page.findChild<QListWidget*>("libraryGames");
        list->setCurrentRow(0);
        QApplication::processEvents();
        const auto image = list->viewport()->grab().toImage();
        const auto rect = list->visualItemRect(list->item(1)).intersected(image.rect());
        QVERIFY(!rect.isEmpty());
        int ivory = 0;
        for (int y = rect.top() + 118; y <= rect.bottom(); ++y)
            for (int x = rect.left(); x <= rect.right(); ++x)
                {
                    const auto pixel = image.pixelColor(x, y);
                    ivory += pixel.red() > 170 && pixel.green() > 170 && pixel.blue() > 170;
                }
        QVERIFY2(ivory > 20, "Unselected dark card must render readable Ivory text even on a Light shell");
    }
    void shellNavigationUsesRealFocusAndVisibleTabs() {
        QTabWidget tabs;
        auto* library = new LibraryPage;
        library->SetEmbeddedHeaderVisible(false);
        tabs.addTab(library, "Library");
        auto* settings = new QWidget;
        auto* layout = new QVBoxLayout(settings);
        auto* button = new QPushButton("Action");
        auto* combo = new QComboBox;
        combo->addItems({"First", "Second", "Third"});
        auto* check = new QCheckBox("Toggle");
        layout->addWidget(button); layout->addWidget(combo); layout->addWidget(check);
        tabs.addTab(settings, "Settings");
        auto* remote = new QWidget;
        tabs.addTab(remote, "Remote");
        tabs.addTab(new QWidget, "Hidden host"); tabs.setTabVisible(3, false);
        tabs.resize(520, 480); tabs.show(); tabs.activateWindow();
        QTRY_VERIFY(tabs.isActiveWindow());
        ShellNavigation router;
        auto sample = [&](QSet<QString> pressed) { router.Handle(pressed, &tabs, library); QApplication::processEvents(); };
        auto tap = [&](QString key) { sample({}); sample({key}); sample({}); };
        sample({}); tap("RightShoulder"); QCOMPARE(tabs.currentIndex(), 1);
        button->setFocus(); QTRY_COMPARE(QApplication::focusWidget(), button);
        QSignalSpy clicks(button, &QPushButton::clicked);
        sample({"South"}); sample({"South"}); QCOMPARE(clicks.count(), 1); sample({});
        combo->setFocus(); tap("South"); QVERIFY(QApplication::activePopupWidget());
        tap("DpadDown"); tap("South"); QCOMPARE(combo->currentIndex(), 1);
        QVERIFY(!QApplication::activePopupWidget());
        combo->setFocus(); tap("South"); QVERIFY(QApplication::activePopupWidget());
        tap("East"); QVERIFY(!QApplication::activePopupWidget()); QCOMPARE(tabs.currentIndex(), 1);
        check->setFocus(); tap("South"); QVERIFY(check->isChecked());
        QDialog modal(&tabs); modal.setModal(true); modal.show(); modal.activateWindow();
        QTRY_VERIFY(QApplication::activeModalWidget() == &modal);
        tap("East"); QVERIFY(!modal.isVisible());
        tabs.activateWindow(); QTRY_VERIFY(tabs.isActiveWindow());
        tap("RightShoulder"); QCOMPARE(tabs.currentIndex(), 2);
        tap("RightShoulder"); QCOMPARE(tabs.currentIndex(), 0);
        tap("LeftShoulder"); QCOMPARE(tabs.currentIndex(), 2);
        tap("East"); QCOMPARE(tabs.currentIndex(), 0);
        tap("RightShoulder"); button->setFocus(); router.Reset();
        sample({"South"}); sample({"South"}); QCOMPARE(clicks.count(), 1);
        sample({}); tap("South"); QCOMPARE(clicks.count(), 2);
        tabs.setEnabled(false); tap("South"); QCOMPARE(clicks.count(), 2);
        tabs.setEnabled(true); sample({"South"}); QCOMPARE(clicks.count(), 2);
        QWidget other; other.show(); other.activateWindow(); QTRY_VERIFY(other.isActiveWindow());
        tap("South"); QCOMPARE(clicks.count(), 2);
        tabs.activateWindow(); QTRY_VERIFY(tabs.isActiveWindow());
        sample({"South"}); QCOMPARE(clicks.count(), 2);
    }
    void pointerOpensDetailsBeforePlay_data() {
        QTest::addColumn<bool>("singleClick");
        QTest::newRow("double-click-desktop") << false;
        QTest::newRow("single-click-desktop") << true;
    }
    void pointerOpensDetailsBeforePlay() {
        QFETCH(bool, singleClick);
        LibraryPage page;
        page.SetGames({{"ps5-id", "A game", "ps5", "/fixture/game", {}, true}});
        auto* list = page.findChild<QListWidget*>("libraryGames");
        ActivationStyle style(singleClick);
        list->setStyle(&style);
        page.resize(960, 540);
        page.show();
        QTest::qWait(20);
        QSignalSpy play(&page, &LibraryPage::PlayRequested);
        auto* action = page.findChild<QPushButton*>("libraryPlay");
        const auto point = list->visualItemRect(list->item(0)).center();
        QTest::mouseClick(list->viewport(), Qt::RightButton, Qt::NoModifier, point);
        QVERIFY(list->isVisible());
        QVERIFY(!action->isVisible());
        QCOMPARE(play.count(), 0);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QVERIFY(action->isVisible());
        QVERIFY(!list->isVisible());
        QCOMPARE(play.count(), 0);
        QTest::mouseClick(action, Qt::LeftButton);
        QCOMPARE(play.count(), 1);
        QCOMPARE(play.first().first().toString(), QString("/fixture/game"));
        list->setStyle(nullptr);
    }

	void detailsPlayAndBack() {
		LibraryPage page;
		page.SetGames({{"ps5-id", "A game", "ps5", "/fixture/game", {}, true}});
		page.show();
		QSignalSpy play(&page, &LibraryPage::PlayRequested);
		auto*      list = page.findChild<QListWidget*>("libraryGames");
		list->setFocus();
		QTest::keyClick(list, Qt::Key_Return);
		QVERIFY(page.findChild<QPushButton*>("libraryPlay")->isVisible());
		QTest::mouseClick(page.findChild<QPushButton*>("libraryPlay"), Qt::LeftButton);
		QCOMPARE(play.count(), 1);
		QCOMPARE(play.first().first().toString(), QString("/fixture/game"));
		QTest::mouseClick(page.findChild<QPushButton*>("libraryBack"), Qt::LeftButton);
		QVERIFY(list->isVisible());
		QCOMPARE(page.SelectedId(), QString("ps5-id"));
		QSignalSpy selection(&page, &LibraryPage::SelectionChanged);
		page.SetGames({{"other", "Other", "ps5", {}, {}, false},
		               {"ps5-id", "A game", "ps5", "/fixture/game", {}, true}});
		QCOMPARE(page.SelectedId(), QString("ps5-id"));
		QCOMPARE(selection.count(),
		         0); // reload must not feed settings persistence back into rescanning
	}

	void selectionPersistenceKeepsSearchAcrossReload() {
		LibraryPage        page;
		QList<LibraryCard> games {{"one", "First", "ps5", "/one", {}, true},
		                          {"two", "Second", "ps5", "/two", {}, true}};
		page.SetGames(games);
		page.show();
		page.activateWindow();
		QTest::qWait(20);
		auto* list      = page.findChild<QListWidget*>("libraryGames");
		auto* search    = page.findChild<QLineEdit*>("librarySearch");
		int   persisted = 0;
		connect(&page, &LibraryPage::SelectionChanged, &page, [&](const QString& id) {
			if (id.isEmpty()) return;
			++persisted;
			page.SetGames(
			    games); // synchronous WriteSettings -> CatalogChanged -> ReloadLibrary path
		});
		search->setText("Second");
		QVERIFY(list->item(0)->isHidden());
		list->setFocus();
		list->setCurrentRow(1);
		QCOMPARE(persisted, 1);
		QCOMPARE(search->text(), QString("Second"));
		QCOMPARE(page.SelectedId(), QString("two"));
		QVERIFY(list->item(0)->isHidden());
		QVERIFY(!list->item(1)->isHidden());
		QVERIFY(list->hasFocus());
		page.SetGames(games);
		QVERIFY(list->item(0)->isHidden());
		QCOMPARE(page.SelectedId(), QString("two"));
		page.findChild<QPushButton*>("libraryDetails")->click();
		games.removeLast();
		page.SetGames(games);
		QVERIFY(list->isVisible());
		QVERIFY(list->hasFocus());
		QVERIFY(page.SelectedId().isEmpty());
		QVERIFY(list->item(0)->isHidden());
		QCOMPARE(search->text(), QString("Second"));
		QSignalSpy play(&page, &LibraryPage::PlayRequested);
		page.findChild<QPushButton*>("libraryDetails")->click();
		QCOMPARE(play.count(), 0);
	}
	void sourceAndBusyGuards() {
		LibraryPage page;
		page.SetGames({{"steam-id", "Steam game", "steam", {}, "Missing recipe", false}});
		page.show();
		QSignalSpy local(&page, &LibraryPage::LocalPlayRequested);
		QSignalSpy play(&page, &LibraryPage::PlayRequested);
		page.findChild<QPushButton*>("libraryDetails")->click();
		auto* button = page.findChild<QPushButton*>("libraryPlay");
		QVERIFY(!button->isEnabled());
		page.SetGames({{"steam-id", "Steam game", "steam", {}, {}, true}});
		QVERIFY(button->isEnabled());
		QCOMPARE(button->text(), QString("Play"));
		button->click();
		QCOMPARE(local.count(), 1);
        button->click(); // Pending local launch cannot dispatch twice.
        QCOMPARE(local.count(), 1);
        QCOMPARE(local.first().first().toString(), QString("steam-id"));
		QCOMPARE(play.count(), 0);
        page.SetLaunchError("Local game exited unsuccessfully.");
        QCOMPARE(page.findChild<QLabel*>("libraryLaunchReason")->text(), QString("Local game exited unsuccessfully."));
        page.SetGames({{"steam-id", "Steam game", "steam", {}, {}, true}});
        QCOMPARE(page.findChild<QLabel*>("libraryLaunchReason")->text(), QString("Local game exited unsuccessfully."));
        page.SetLaunchError({});
        QCOMPARE(page.findChild<QLabel*>("libraryLaunchReason")->text(), QString("Ready to play on this computer."));
		page.SetLaunchEnabled(false);
		button->click();
		QCOMPARE(local.count(), 1);
		QSignalSpy settings(&page, &LibraryPage::SettingsRequested);
		page.findChild<QPushButton*>("librarySettings")->click();
		QCOMPARE(settings.count(), 1);
	}
    void remoteAndUnknownSourcesNeverDispatchLocalPlay() {
        LibraryPage page;
        page.SetGames({{"remote:one:same", "Remote game", "remote", "/must-not-run", {}, true, "PC 日本"}});
        QSignalSpy local(&page, &LibraryPage::LocalPlayRequested), ps5(&page, &LibraryPage::PlayRequested), action(&page, &LibraryPage::GameActionRequested);
        page.findChild<QPushButton*>("libraryDetails")->click();
        auto* play = page.findChild<QPushButton*>("libraryPlay");
        play->click(); play->click(); QCOMPARE(action.size(), 1);
        QCOMPARE(action[0][1].toString(), QString("remote")); QCOMPARE(local.size(), 0); QCOMPARE(ps5.size(), 0);
        page.SetGames({{"unknown", "Untrusted", "unknown", "/must-not-run", {}, true}});
        page.SetLaunchEnabled(true); play->click();
        QCOMPARE(action.size(), 1); QCOMPARE(local.size(), 0); QCOMPARE(ps5.size(), 0);
        const auto text = page.findChild<QListWidget*>("libraryGames")->item(0)->text();
        QVERIFY(text.contains("Unknown source")); QVERIFY(!text.contains("PS5"));
        page.SetGames({{"remote:one:same", "Remote game", "remote", {}, "Host offline", false, "PC 日本"}});
        page.SetLaunchEnabled(true); QVERIFY(!play->isEnabled());
        QCOMPARE(page.findChild<QLabel*>("libraryLaunchReason")->text(), QString("Host offline"));
        QSignalSpy hosts(&page, &LibraryPage::RemoteHostsRequested);
        page.findChild<QPushButton*>("libraryRemoteHosts")->click(); QCOMPARE(hosts.size(), 1);
    }
	void controllerResumeUsesHeldSnapshotAsBaseline() {
		LibraryPage page;
		const QList<LibraryCard> games {{"one", "First", "ps5", "/one", {}, true}};
		page.SetGames(games);
		page.show();
		page.activateWindow();
		QTest::qWait(20);
		QVERIFY(page.isActiveWindow());
		page.findChild<QListWidget*>("libraryGames")->setFocus();
		page.HandleController({});
		page.HandleController({"South"});
		page.SetLaunchEnabled(false);
		page.SetGames(games); // Remote Changed reloads details while Play is disabled.
		auto* back = page.findChild<QPushButton*>("libraryBack");
		auto* list = page.findChild<QListWidget*>("libraryGames");
		QVERIFY(!page.findChild<QPushButton*>("libraryPlay")->isEnabled());
		QVERIFY(back->hasFocus());
		page.HandleController({}); // Suspension clears the preview's raw buttons.
		page.ResetControllerNavigation();
		page.HandleController({"South"}); // Immediate resume snapshot: still held.
		QVERIFY(back->isVisible());
		QVERIFY(!list->isVisible());
		page.HandleController({"South"});
		QVERIFY(back->isVisible());
		page.HandleController({});
		page.HandleController({"South"});
		QVERIFY(list->isVisible());
	}
	void controllerFirstSnapshotAndInactiveTracking() {
		LibraryPage page;
		page.SetGames({{"one", "First", "ps5", "/one", {}, true}});
		page.HandleController({"South"}); // First snapshot arrives while hidden.
		page.show();
		page.activateWindow();
		QTest::qWait(20);
		auto* list = page.findChild<QListWidget*>("libraryGames");
		list->setFocus();
		page.HandleController({"South"});
		QVERIFY(list->isVisible());
		page.HandleController({});
		page.HandleController({"South"});
		QVERIFY(!list->isVisible());
		page.ResetControllerNavigation();
		page.HandleController({"South"}); // An active first snapshot is safe too.
		QSignalSpy play(&page, &LibraryPage::PlayRequested);
		page.HandleController({"South"});
		QCOMPARE(play.count(), 0);
		page.HandleController({});
		page.HandleController({"South"});
		page.HandleController({"South"});
		QCOMPARE(play.count(), 1);
	}
	void controllerEdges() {
		LibraryPage page;
		page.SetGames({{"one", "First", "ps5", "/one", {}, true},
		               {"two", "Second", "ps5", "/two", {}, true}});
		page.show();
		page.activateWindow();
		QTest::qWait(20);
		QVERIFY(page.isActiveWindow());
		page.findChild<QListWidget*>("libraryGames")->setFocus();
		page.HandleController({});
		page.HandleController({"DpadRight"});
		QCOMPARE(page.SelectedId(), QString("two"));
		page.HandleController({});
		page.HandleController({"South"});
		QSignalSpy play(&page, &LibraryPage::PlayRequested);
		page.HandleController({"South"}); // held confirm cannot also launch
		QCOMPARE(play.count(), 0);
		page.HandleController({});
		page.HandleController({"South"});
		QCOMPARE(play.count(), 1);
		page.HandleController({});
		page.HandleController({"East"});
		QVERIFY(page.findChild<QListWidget*>("libraryGames")->isVisible());
	}
	void renderSizes() {
		LibraryPage page;
		QImage      art(360, 224, QImage::Format_RGB32);
		art.fill(QColor("#2c5270"));
		QPainter painter(&art);
		painter.setPen(Qt::white);
		auto font = painter.font();
		font.setPixelSize(28);
		painter.setFont(font);
		painter.drawText(art.rect(), Qt::AlignCenter, "PS5 cover fixture");
		painter.end();
		QByteArray bytes;
		QBuffer    buffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		QVERIFY(art.save(&buffer, "PNG"));
		page.SetGames(
		    {{"one", "A game with a long descriptive title", "ps5", {}, {}, true, {}, bytes},
			 {"two",
			  "Installed Steam game",
			  "steam",
			  {},
			  "Missing owned launch recipe",
			  false,
			  "620"}});

		for (const QSize size: {QSize(520, 480), QSize(1280, 720)}) {
			page.resize(size);
			page.show();
			QTest::qWait(20);
			QCOMPARE(page.size(), size);
			QVERIFY(page.grab().save(QString("/tmp/ags-library-%1.png").arg(size.width())));
			page.findChild<QPushButton*>("libraryDetails")->click();
			QCOMPARE(page.size(), size);
			QVERIFY(page.grab().save(QString("/tmp/ags-details-%1.png").arg(size.width())));
			page.findChild<QPushButton*>("libraryBack")->click();
		}
	}
};
QTEST_MAIN(LibraryPageTests)
#include "libraryPageTests.moc"
