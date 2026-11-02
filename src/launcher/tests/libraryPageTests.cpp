#include "libraryPage.h"

#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTest>
class LibraryPageTests: public QObject {
	Q_OBJECT
private slots:
	void initTestCase() {
		QFile style(":/styles/launcher.qss");
		QVERIFY(style.open(QIODevice::ReadOnly));
		qApp->setStyleSheet(QString::fromUtf8(style.readAll()));
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
	void sourceAndBusyGuards() {
		LibraryPage page;
		page.SetGames({{"steam-id", "Steam game", "steam", {}, "Missing recipe", false}});
		page.show();
		QSignalSpy streaming(&page, &LibraryPage::StreamingRequested);
		QSignalSpy play(&page, &LibraryPage::PlayRequested);
		page.findChild<QPushButton*>("libraryDetails")->click();
		auto* button = page.findChild<QPushButton*>("libraryPlay");
		QVERIFY(!button->isEnabled());
		page.SetGames({{"steam-id", "Steam game", "steam", {}, {}, true}});
		QVERIFY(button->isEnabled());
		QCOMPARE(button->text(), QString("Open streaming"));
		button->click();
		QCOMPARE(streaming.count(), 1);
		QCOMPARE(play.count(), 0);
		page.SetLaunchEnabled(false);
		button->click();
		QCOMPARE(streaming.count(), 1);
		QSignalSpy settings(&page, &LibraryPage::SettingsRequested);
		page.findChild<QPushButton*>("librarySettings")->click();
		QCOMPARE(settings.count(), 1);
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
