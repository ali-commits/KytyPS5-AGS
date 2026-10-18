#include "cheatFile.h"
#include "cheatRepository.h"
#include "configuration.h"
#include "configurationEditDialog.h"
#include "configurationItem.h"
#include "configurationListWidget.h"
#include "controllerPreview.h"
#include "inputMappingDialog.h"
#include "titleSound.h"
#include "updateChecker.h"
#include "emulatorCommand.h"
#include "mainDialog.h"
#include "launcherTheme.h"
#include "shaderPrecompileDialog.h"
#include "streamingCatalog.h"
#include "streamingBackend.h"
#include "streamingWidget.h"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QImage>
#include <QGroupBox>
#include <QPainter>
#include <QLinearGradient>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QListView>
#include <QMenu>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

#include <array>
#include <vector>

namespace {
class EditDialog: public ConfigurationEditDialog {
public:
    using ConfigurationEditDialog::ConfigurationEditDialog;
    using ConfigurationEditDialog::clear;
    using ConfigurationEditDialog::save;
};

class WorkingDirectory {
public:
    explicit WorkingDirectory(const QString& path): m_previous(QDir::currentPath()) {
        QDir::setCurrent(path);
    }
    ~WorkingDirectory() { QDir::setCurrent(m_previous); }
private:
    QString m_previous;
};

bool Write(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QByteArray Read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray {};
}
QJsonDocument CheatDocument() {
    return QJsonDocument::fromJson(R"({"id":"PPSA99998","version":"01.018.000","process":"eboot.bin",
        "mods":[{"name":"First","enabled":false,"memory":[{"offset":"10","off":"00","on":"01"}]},
                {"name":"Second","enabled":true,"memory":[{"offset":"20","off":"FF","on":"00"}]}]})");
}
QString Value(const QStringList& args, const QString& flag) {
    const auto i = args.indexOf(flag);
    return i >= 0 && i + 1 < args.size() ? args.at(i + 1) : QString {};
}
bool Capture(QWidget& widget, const QString& suffix = {}, bool expand = true) {
    const auto requested = qEnvironmentVariable("KYTY_PRESERVATION_SCREENSHOT");
    if (requested.isEmpty()) return true;
    const QFileInfo base(requested);
    const auto path = suffix.isEmpty() ? requested : base.dir().filePath(base.completeBaseName() + suffix + ".png");
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    const auto requested_size=widget.size();
    widget.adjustSize();
    widget.resize(expand ? widget.size().expandedTo(QSize(1100, 900)) : requested_size.expandedTo(widget.minimumSizeHint()));
    widget.show();
    QApplication::processEvents();
    return widget.grab().save(path, "PNG");
}
bool CreateGridFixture(const QTemporaryDir& temp) {
    const std::array<const char*,3> names {"Astro Fixture", "Moonlight Fixture", "Stream Fixture"};
    for (int i=0;i<3;++i) {
        const auto base = temp.filePath(QString("games/%1").arg(names[i]));
        if (!Write(base+"/eboot.bin","fixture, never executed") ||
            !Write(base+"/sce_sys/param.json",QJsonDocument(QJsonObject{
                {"titleId",QString("PPSA9999%1").arg(i)}, {"appVersion","01.000.000"},
                {"titleName",QString::fromLatin1(names[i])}}).toJson())) return false;
        QImage art(480,270,QImage::Format_RGB32);
        art.fill(QColor::fromHsv(195+i*65,160,210));
        QPainter painter(&art);
        QLinearGradient backdrop(0,0,480,270);
        backdrop.setColorAt(0,QColor::fromHsv(195+i*65,180,200));
        backdrop.setColorAt(1,QColor::fromHsv(215+i*65,230,65));
        painter.fillRect(art.rect(),backdrop);
        painter.setPen(QPen(QColor(255,255,255,45),3));
        for(int line=0;line<12;++line) painter.drawLine(line*60-100,0,line*60+100,270);
        painter.setPen(Qt::white);
        QFont cover_font=painter.font(); cover_font.setPixelSize(34); cover_font.setBold(true);
        painter.setFont(cover_font); painter.drawText(QRect(30,125,420,100),Qt::AlignLeft|Qt::TextWordWrap,names[i]);
        cover_font.setPixelSize(16); cover_font.setBold(false); painter.setFont(cover_font);
        painter.drawText(30,242,"KYTY UI TEST LIBRARY");
        painter.end();
        if(!art.save(base+"/sce_sys/icon0.png")) return false;
    }
    QSettings settings(temp.filePath("Kyty.ini"),QSettings::IniFormat);
    settings.setValue("Launcher/game_dirs",QStringList{temp.filePath("games")});
    settings.sync();
    return Write(temp.filePath("compatibility_db.json"),"{}");
}

constexpr std::array<const char*, 5> Sliders {
    "slider_audio_master", "slider_audio_main", "slider_audio_music",
    "slider_audio_pad_speaker", "slider_audio_objects"};
constexpr std::array<const char*, 5> AudioKeys {
    "audio_master_volume", "audio_main_volume", "audio_music_volume",
    "audio_pad_speaker_main_volume", "audio_objects_volume"};
constexpr std::array<int, 5> Defaults {100, 100, 100, 30, 100};
constexpr std::array<int, 5> Levels {0, 200, 137, 43, 181};

void SetLevels(Configuration& info) {
    info.audio_mix.master = 0;
    info.audio_mix.main = 200;
    info.audio_mix.music = 137;
    info.audio_mix.pad_speaker = 43;
    info.audio_mix.objects = 181;
    info.audio_mix.objects_enabled = false;
    info.gpu_occlusion_accurate = true;
}
} // namespace

class LauncherPreservationTests: public QObject {
    Q_OBJECT
private slots:
    void precompileActionRequiresASelectedGameAndAnInterpreter() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        WorkingDirectory cwd(temp.path());
        QVERIFY(CreateGridFixture(temp));
        MainDialog dialog;
        auto* prepare=dialog.findChild<QPushButton*>("precompileShadersButton");
        QVERIFY2(prepare,"The selected game's Library has no precompile action");
        QVERIFY(!prepare->isEnabled()); // interpreter discovery has not completed
        auto* tree=dialog.findChild<QTreeWidget*>("cfgs_list");
        QVERIFY(tree);
        tree->setCurrentItem(tree->topLevelItem(1));
        QVERIFY(!prepare->isEnabled());
    }

    void selectedGamePrecompileButtonStartsTheConfiguredGame() {
#if defined(__linux__)
        QTemporaryDir temp; QVERIFY(temp.isValid()); QVERIFY(CreateGridFixture(temp));
        const auto bin=temp.filePath("bin"); QVERIFY(QDir().mkpath(bin));
        const auto self=bin+"/launcher_preservation_tests";
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(),self));
        const auto runner=bin+"/kyty_emulator";
        QVERIFY(Write(runner,R"PY(#!/usr/bin/python3
import json,sys,time,os
from pathlib import Path
if len(sys.argv)==1:
    print('Fixture emulator\nusage');sys.exit(0)
p=Path.cwd()
(p/'selected-launch.json').write_text(json.dumps({'args':sys.argv[1:],'mode':os.environ.get('KYTY_PIPELINE_LIST_PREPARE')}))
print('KYTY_PREPARE_PROGRESS shaders 1 4 0',flush=True)
while not (p/'ui-ack').exists():time.sleep(.01)
print('KYTY_PREPARE_PROGRESS complete 4 4 0',flush=True)
)PY"));
        QVERIFY(QFile::setPermissions(runner,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QSettings settings(temp.filePath("Kyty.ini"),QSettings::IniFormat);
        settings.setValue("MainDialog/check_updates_on_startup",false);settings.sync();
        QProcess child;auto environment=QProcessEnvironment::systemEnvironment();
        environment.insert("KYTY_STREAMING_CONFIG_DIR",temp.filePath("streaming"));
        child.setProcessEnvironment(environment);child.setWorkingDirectory(temp.path());
        child.start(self,{"--precompile-ui-fixture"});QVERIFY(child.waitForFinished(15000));
        QVERIFY2(child.exitCode()==0,child.readAllStandardError().constData());
        const auto launch=QJsonDocument::fromJson(Read(bin+"/selected-launch.json")).object();
        QStringList args;for(const auto& arg:launch.value("args").toArray())args<<arg.toString();
        QCOMPARE(Value(args,"--game"),temp.filePath("games/Stream Fixture/eboot.bin"));
        QCOMPARE(launch.value("mode").toString(),QString("1"));
#else
        QSKIP("Isolated executable fixture uses Linux");
#endif
    }

    void precompileDialogUsesSelectionAndShowsRealStagesBeforeCompletion() {
#if defined(__linux__)
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const auto runtime=temp.filePath("runtime with spaces"); QVERIFY(QDir().mkpath(runtime));
        const auto runner=runtime+"/kyty_emulator";
        QVERIFY(Write(runner,R"PY(#!/usr/bin/python3
import json,os,sys,time
from pathlib import Path
p=Path(__file__).parent
(p/'launch.json').write_text(json.dumps({'args':sys.argv[1:],'cwd':os.getcwd(),'mode':os.environ.get('KYTY_PIPELINE_LIST_PREPARE'),'replay':os.environ.get('KYTY_SHADER_PRECOMPILE_REPLAY')}))
def wait(name):
    while not (p/name).exists(): time.sleep(.01)
sys.stdout.write('diagnostic noise\nKYTY_PREPARE_PROG');sys.stdout.flush()
time.sleep(.03)
print('RESS shaders 1 4 0',flush=True);wait('shaders-ack')
print('KYTY_PREPARE_PROGRESS pipelines 1 2 0',flush=True);wait('pipelines-ack')
print('KYTY_PREPARE_PROGRESS saving 0 0 0',flush=True)
print('KYTY_PREPARE_PROGRESS complete 6 6 0',flush=True);wait('exit-ack')
)PY"));
        QVERIFY(QFile::setPermissions(runner,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        Configuration game; game.name="Selected Game"; game.basedir=temp.filePath("selected game"); game.elf="eboot.bin";
        ShaderPrecompileDialog dialog(runner,game); dialog.show();
        auto* bar=dialog.findChild<QProgressBar*>("precompileProgress");
        auto* status=dialog.findChild<QLabel*>("precompileStatus");
        QVERIFY(bar && status);
        QTRY_COMPARE(bar->value(),250);
        QVERIFY(status->text().contains("1 of 4"));
        const auto launch=QJsonDocument::fromJson(Read(runtime+"/launch.json")).object();
        const auto args=launch.value("args").toArray(); QStringList actual;
        for(const auto& arg:args) actual<<arg.toString();
        QCOMPARE(Value(actual,"--game"),temp.filePath("selected game/eboot.bin"));
        QCOMPARE(launch.value("cwd").toString(),runtime);
        QCOMPARE(launch.value("mode").toString(),QString("1"));
        QCOMPARE(launch.value("replay").toString(),QString("1"));
        QVERIFY(Capture(dialog,"-precompile",false));
        QVERIFY(Write(runtime+"/shaders-ack",""));
        QTRY_COMPARE(bar->value(),500);
        QVERIFY(status->text().contains("1 of 2"));
        QVERIFY(Write(runtime+"/pipelines-ack",""));
        QTRY_VERIFY(status->text().contains("Saving"));
        QVERIFY(dialog.IsRunning()); // protocol completion alone must not report success
        QVERIFY(Write(runtime+"/exit-ack",""));
        QTRY_VERIFY(!dialog.IsRunning());
        QCOMPARE(bar->value(),1000);
        QVERIFY(status->text().contains("Ready to play"));
        QVERIFY(Capture(dialog,"-precompile-complete",false));
#else
        QSKIP("Executable process fixture uses Python on Linux");
#endif
    }

    void precompileDialogRejectsIncompleteRuns_data() {
        QTest::addColumn<QByteArray>("output"); QTest::addColumn<int>("code");
        QTest::newRow("exit-zero-without-confirmation")<<QByteArray("some output")<<0;
        QTest::newRow("confirmation-but-failed-process")<<QByteArray("KYTY_PREPARE_PROGRESS complete 2 2 0")<<3;
        QTest::newRow("failed-recipes")<<QByteArray("KYTY_PREPARE_PROGRESS failed 2 2 1")<<3;
        QTest::newRow("no-recipes")<<QByteArray("KYTY_PREPARE_PROGRESS empty 0 0 1")<<2;
        QTest::newRow("incomplete-confirmation")<<QByteArray("KYTY_PREPARE_PROGRESS complete 1 4 0")<<0;
        QTest::newRow("empty-confirmation")<<QByteArray("KYTY_PREPARE_PROGRESS complete 0 0 0")<<0;
        QTest::newRow("invalid-counts")<<QByteArray("KYTY_PREPARE_PROGRESS shaders 5 2 0")<<0;
    }
    void precompileDialogRejectsIncompleteRuns() {
#if defined(__linux__)
        QFETCH(QByteArray,output); QFETCH(int,code);
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const auto runner=temp.filePath("kyty_emulator");
        QVERIFY(Write(runner,"#!/bin/sh\nprintf '%s\\n' '"+output+"'\nexit "+QByteArray::number(code)+"\n"));
        QVERIFY(QFile::setPermissions(runner,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        Configuration game; game.name="Fixture"; game.basedir=temp.path();
        ShaderPrecompileDialog dialog(runner,game); dialog.show();
        QTRY_VERIFY(!dialog.IsRunning());
        auto* status=dialog.findChild<QLabel*>("precompileStatus");
        auto* bar=dialog.findChild<QProgressBar*>("precompileProgress");
        QVERIFY(!status->text().contains("Ready to play"));
        QCOMPARE(bar->value(),0);
        if(code==2) QVERIFY(status->text().contains("No saved shader recipes"));
#else
        QSKIP("Executable fixture uses a Linux shell");
#endif
    }
    void precompileCancelStopsTheOwnedProcessAndItsChild() {
#if defined(__linux__)
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const auto runner=temp.filePath("kyty_emulator");
        QVERIFY(Write(runner,R"PY(#!/usr/bin/python3
import sys,subprocess,time
from pathlib import Path
child=subprocess.Popen([sys.executable,'-c',"import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); print('ready',flush=True); time.sleep(60)"],stdout=subprocess.PIPE)
child.stdout.readline()
Path(__file__).with_suffix('.pid').write_text(str(child.pid))
print('KYTY_PREPARE_PROGRESS shaders 0 1000 0',flush=True)
time.sleep(60)
)PY"));
        QVERIFY(QFile::setPermissions(runner,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        Configuration game; game.name="Fixture"; game.basedir=temp.path();
        ShaderPrecompileDialog dialog(runner,game); dialog.show();
        QTRY_VERIFY(QFile::exists(temp.filePath("kyty_emulator.pid")));
        const auto pid=Read(temp.filePath("kyty_emulator.pid")).trimmed();
        QTest::mouseClick(dialog.findChild<QPushButton*>("precompileCancelClose"),Qt::LeftButton);
        QTRY_VERIFY(!dialog.IsRunning());
        QTRY_VERIFY(!QFile::exists("/proc/"+pid+"/stat") || Read("/proc/"+pid+"/stat").contains(") Z"));
        QVERIFY(dialog.findChild<QLabel*>("precompileStatus")->text().contains("cancelled"));
#else
        QSKIP("Owned process-group fixture is Linux-specific");
#endif
    }

    void headlessStartupSelectsStreamingBeforePreviewAndStartsOnlyHost() {
#if defined(__linux__)
        QTemporaryDir temp; QVERIFY(temp.isValid());
        QVERIFY(CreateGridFixture(temp));
        const auto bin=temp.filePath("bin"); QVERIFY(QDir().mkpath(bin));
        const auto runner=bin+"/launcher_preservation_tests";
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(),runner));
        const auto emulator=bin+"/kyty_emulator";
        const QByteArray version_stub = "#!/bin/sh\nif [ \"$#\" -ne 0 ]; then touch unexpected-game; exit 1; fi\nprintf 'UI fixture emulator\\nusage\\n'\n";
        QVERIFY(Write(emulator,version_stub));
        QVERIFY(QFile::setPermissions(emulator,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        const auto profile=temp.filePath("streaming"); QVERIFY(QDir().mkpath(profile));
        QSettings settings(profile+"/streaming.ini",QSettings::IniFormat);
        settings.setValue("host/backend",temp.filePath("missing-helper")); settings.sync();
        QProcess child; auto environment=QProcessEnvironment::systemEnvironment();
        environment.insert("KYTY_STREAMING_CONFIG_DIR",profile);
        child.setProcessEnvironment(environment); child.setWorkingDirectory(temp.path());
        child.start(runner,{"--headless-ui-startup-fixture"});
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitStatus(),QProcess::NormalExit);
        QVERIFY2(child.exitCode()==0,child.readAllStandardError().constData());
        QVERIFY(!QFile::exists(temp.filePath("unexpected-game")));
        QVERIFY(!QFile::exists(profile+"/host-key.pem"));
#else
        QSKIP("The isolated executable fixture uses a Linux shell version stub");
#endif
    }

    void upstreamGridSharesSelectionResizesPersistsAndKeepsCatalog() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        WorkingDirectory cwd(temp.path());
        QVERIFY(CreateGridFixture(temp));
        QString selected_title;
        {
            ConfigurationListWidget library;
            library.resize(1100,700);
            library.show();
            auto* tree=library.findChild<QTreeWidget*>("cfgs_list");
            auto* grid=library.findChild<QListView*>("cfgs_grid");
            auto* toggle=library.findChild<QToolButton*>("grid_view_button");
            auto* size=library.findChild<QSlider*>("grid_size_slider");
            auto* views=library.findChild<QStackedWidget*>("game_views");
            QVERIFY(tree && grid && toggle && size && views);
            QCOMPARE(tree->topLevelItemCount(),3);
            QCOMPARE(grid->model(),tree->model());
            QCOMPARE(grid->selectionModel(),tree->selectionModel());
            QVERIFY(!toggle->isChecked());
            QCOMPARE(views->currentWidget(),tree);
            QCOMPARE(size->minimum(),128); QCOMPARE(size->maximum(),320);
            QCOMPARE(size->singleStep(),16); QCOMPARE(size->pageStep(),32);
            QCOMPARE(size->value(),224);
            tree->setCurrentItem(tree->topLevelItem(1),2);
            selected_title=tree->currentItem()->text(0);
            QTest::mouseClick(toggle,Qt::LeftButton);
            QCOMPARE(views->currentWidget(),grid);
            QCOMPARE(grid->currentIndex().column(),0);
            QCOMPARE(grid->currentIndex().data().toString(),selected_title);
            const auto before=library.CreateStreamingCatalog("/fixture/kyty_emulator");
            QCOMPARE(before.size(),3);
            QSignalSpy changed(&library,&ConfigurationListWidget::CatalogChanged);
            size->setValue(320);
            QCOMPARE(grid->iconSize(),QSize(320,180));
            QVERIFY(changed.count()>0);
            const auto after=library.CreateStreamingCatalog("/fixture/kyty_emulator");
            QCOMPARE(after.size(),before.size());
            for(int i=0;i<after.size();++i) QCOMPARE(after[i].arguments,before[i].arguments);
            QSettings saved("Kyty.ini",QSettings::IniFormat);
            QVERIFY(saved.value("Launcher/grid_view").toBool());
            QCOMPARE(saved.value("Launcher/grid_icon_width").toInt(),320);
            QVERIFY(Capture(library,"-grid"));
            QTest::mouseClick(toggle,Qt::LeftButton);
            QCOMPARE(views->currentWidget(),tree);
            QCOMPARE(tree->currentItem()->text(0),selected_title);
            QVERIFY(Capture(library,"-list"));
            QTest::mouseClick(toggle,Qt::LeftButton);
        }
        ConfigurationListWidget restored;
        QVERIFY(restored.findChild<QToolButton*>("grid_view_button")->isChecked());
        QCOMPARE(restored.findChild<QSlider*>("grid_size_slider")->value(),320);
        QCOMPARE(restored.findChild<QListView*>("cfgs_grid")->iconSize(),QSize(320,180));
    }

    void gridFilteringSortingDoubleClickAndContextActionsUseTheSameGame() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        WorkingDirectory cwd(temp.path());
        QVERIFY(CreateGridFixture(temp));
        ConfigurationListWidget library;
        library.resize(1100,700); library.show();
        auto* tree=library.findChild<QTreeWidget*>("cfgs_list");
        auto* grid=library.findChild<QListView*>("cfgs_grid");
        auto* search=library.findChild<QLineEdit*>("search_line_edit");
        auto* toggle=library.findChild<QToolButton*>("grid_view_button");
        QVERIFY(tree && grid && search && toggle);
        QTest::mouseClick(toggle,Qt::LeftButton);
        search->setText("Moonlight");
        const auto verify_filter=[&] {
            int visible=0;
            for(int row=0;row<tree->topLevelItemCount();++row) {
                const bool hidden=!tree->topLevelItem(row)->text(0).contains("Moonlight");
                if(tree->topLevelItem(row)->isHidden()!=hidden || grid->isRowHidden(row)!=hidden) return false;
                visible += !hidden;
            }
            return visible==1;
        };
        QVERIFY(verify_filter());
        tree->sortItems(0,Qt::DescendingOrder);
        QVERIFY(verify_filter());
        search->clear();
        const auto index=grid->model()->index(1,0);
        QSignalSpy run(&library,&ConfigurationListWidget::Run);
        QVERIFY(QMetaObject::invokeMethod(grid,"doubleClicked",Qt::DirectConnection,Q_ARG(QModelIndex,index)));
        QCOMPARE(run.count(),1);
        QCOMPARE(library.GetSelectedItem()->GetInfo().name,index.data().toString());
        library.SetRunEnabled(false);
        QVERIFY(QMetaObject::invokeMethod(grid,"doubleClicked",Qt::DirectConnection,Q_ARG(QModelIndex,index)));
        QCOMPARE(run.count(),1);
        bool popup_seen=false;
        QStringList actions;
        QTimer popup_timer;
        popup_timer.setInterval(1);
        connect(&popup_timer,&QTimer::timeout,&library,[&] {
            auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if(!menu) for(auto* widget:QApplication::topLevelWidgets())
                if(auto* candidate=qobject_cast<QMenu*>(widget);candidate && candidate->isVisible()) menu=candidate;
            if(menu) {
                popup_seen=true;
                for(auto* action:menu->actions()) if(action->isVisible()) actions.append(action->text());
                menu->close();
            }
        });
        QApplication::processEvents();
        popup_timer.start();
        const auto point=grid->visualRect(index).center();
        QVERIFY(QMetaObject::invokeMethod(grid,"customContextMenuRequested",Qt::DirectConnection,Q_ARG(QPoint,point)));
        popup_timer.stop();
        QVERIFY(popup_seen);
        for(const auto* action:{"Cheats (experimental)...","Additional Content...","Edit game config...","Remove save data..."})
            QVERIFY2(actions.contains(action),action);
        QCOMPARE(library.GetSelectedItem()->GetInfo().name,index.data().toString());
    }

    void currentUpstreamRenderingAndTrophyControlsPersistWithNativeAudio() {
        QTemporaryDir temp;
        Configuration info;
        EditDialog dialog(info); dialog.SetGlobalSettings({});
        auto* zmask=dialog.findChild<QCheckBox*>("checkBox_support_zmask");
        auto* sync=dialog.findChild<QCheckBox*>("checkBox_sync_raw_image_buffers");
        auto* compatibility=dialog.findChild<QGroupBox*>("rendering_compatibility_group");
        auto* duration=dialog.findChild<QSpinBox*>("spinBox_trophy_duration");
        QVERIFY(zmask && sync && compatibility && duration);
        QVERIFY(compatibility->isAncestorOf(zmask) && compatibility->isAncestorOf(sync));
        QCOMPARE(duration->minimum(),5); QCOMPARE(duration->maximum(),10); QCOMPARE(duration->value(),6);
        QVERIFY(!zmask->isChecked());
        zmask->setChecked(true); duration->setValue(9);
        dialog.findChild<QSlider*>("slider_audio_music")->setValue(142);
        dialog.save();
        QSettings file(temp.filePath("options.ini"),QSettings::IniFormat);
        info.WriteSettings(&file); info.WriteGlobalOnlySettings(&file);
        Configuration loaded; loaded.ReadSettings(&file); loaded.ReadGlobalOnlySettings(&file);
        QCOMPARE(Value(BuildEmulatorArgs(loaded),"--support-zmask"),"true");
        QCOMPARE(Value(BuildEmulatorArgs(loaded),"--trophy-duration"),"9");
        QCOMPARE(Value(BuildEmulatorArgs(loaded),"--audio-music-volume"),"142");
        QSettings legacy(temp.filePath("legacy.ini"),QSettings::IniFormat);
        legacy.setValue("user_name","Legacy");
        Configuration migrated; migrated.ReadSettings(&legacy);
        QCOMPARE(Value(BuildEmulatorArgs(migrated),"--trophy-duration"),"6");
        legacy.setValue("trophy_duration_seconds","invalid"); migrated.ReadSettings(&legacy);
        QCOMPARE(migrated.trophy_duration_seconds,6);
        legacy.setValue("trophy_duration_seconds",1); migrated.ReadSettings(&legacy);
        QCOMPARE(migrated.trophy_duration_seconds,5);
        legacy.setValue("trophy_duration_seconds",90); migrated.ReadSettings(&legacy);
        QCOMPARE(migrated.trophy_duration_seconds,10);
        auto* tabs=dialog.findChild<QTabWidget*>("settings_tabs");
        tabs->setCurrentWidget(tabs->findChild<QWidget*>("debug_tab"));
        QVERIFY(Capture(dialog,"-debug"));
    }

    void sharedControllerPreviewAndTitleSoundKeepStreamingAndPresetSafeguards() {
        QTemporaryDir temp; WorkingDirectory cwd(temp.path());
        QVERIFY(CreateGridFixture(temp));
        const auto old=qgetenv("KYTY_STREAMING_CONFIG_DIR");
        qputenv("KYTY_STREAMING_CONFIG_DIR",temp.filePath("private-stream").toUtf8());
        struct Restore { QByteArray previous; ~Restore(){previous.isNull()?qunsetenv("KYTY_STREAMING_CONFIG_DIR"):qputenv("KYTY_STREAMING_CONFIG_DIR",previous);} } restore{old};
        MainDialog dialog;
        auto* preview=dialog.findChild<ControllerPreview*>("controllerPreview");
        QVERIFY(preview);
        auto* title=static_cast<TitleSound*>(dialog.findChild<QObject*>("titleSound"));
        QVERIFY(title);
        preview->SetSuspended(true); // This test never opens physical devices.
        {
            InputMappingDialog mapping({},preview,&dialog);
            QVERIFY(mapping.findChild<QTabWidget*>("inputTabs"));
            QVERIFY(mapping.findChildren<ControllerPreview*>().empty());
            QVERIFY(Capture(mapping,"-controls"));
        }
        dialog.SetPresetWarning("Fixture preset warning");
        bool warning_visible=false;
        for(auto* label:dialog.findChildren<QLabel*>()) warning_visible|=label->text()=="Fixture preset warning";
        QVERIFY(warning_visible);
        QVERIFY(!UpdateChecker::IsSupported()); // Custom packages cannot install official runtime archives.
        auto* streaming=dialog.findChild<StreamingWidget*>("streamingWidget");
        QVERIFY(streaming);
        auto* tabs=qobject_cast<QTabWidget*>(streaming->parentWidget()->parentWidget());
        QVERIFY(tabs);
        auto* tree=dialog.findChild<QTreeWidget*>("cfgs_list");
        tree->setCurrentItem(tree->topLevelItem(0));
        auto* toggle=dialog.findChild<QToolButton*>("grid_view_button");
        QTest::mouseClick(toggle,Qt::LeftButton);
        QVERIFY(Capture(dialog,"-main-grid"));
        QCOMPARE(title->Title(),dialog.findChild<ConfigurationListWidget*>()->GetSelectedItem()->GetInfo().basedir);
        tabs->setCurrentWidget(streaming);
        QVERIFY(title->Title().isEmpty());
        QVERIFY(Capture(dialog,"-streaming-current-ui"));
        dialog.resize(1100,900);
        dialog.show();
        QLabel* version=nullptr;
        for(auto* label:streaming->findChildren<QLabel*>())
            if(label->text()=="Version checked when hosting starts") version=label;
        QVERIFY(version);
        QApplication::processEvents();
        QVERIFY2(version->height()>=version->heightForWidth(version->width()),
                 "The native form field clips its wrapped backend status text");
        tabs->setCurrentIndex(0);
        QVERIFY(!title->Title().isEmpty());
        auto* backend=dialog.findChild<StreamingBackend*>();
        QVERIFY(backend);
        bool preview_paused=false,title_cleared=false;
        connect(backend,&StreamingBackend::Changed,&dialog,[&] {
            if(backend->GetState()==StreamingBackend::State::Starting) {
                preview_paused=preview->Error().contains("paused");
                title_cleared=title->Title().isEmpty();
            }
        });
        StreamingSettings invalid;
        invalid.backend=temp.filePath("missing-helper");
        backend->Start(invalid); // Fails before profile/audio/process setup.
        QVERIFY(preview_paused && title_cleared);
        QCOMPARE(backend->GetState(),StreamingBackend::State::Error);
        QVERIFY(!QFile::exists(temp.filePath("private-stream/host-key.pem")));
    }

    void updatedTabsKeepEveryGlobalAudioAndOcclusionControl() {
        Configuration info;
        EditDialog dialog(info);
        dialog.SetGlobalSettings({});
        auto* tabs = dialog.findChild<QTabWidget*>("settings_tabs");
        QVERIFY2(tabs, "Updated tabbed settings must coexist with native audio controls");
        for (const auto* name: {"settings_tab", "debug_tab", "network_tab"}) {
            auto* page = tabs->findChild<QWidget*>(name);
            QVERIFY2(page, name);
            QVERIFY(tabs->indexOf(page) >= 0);
        }
        for (int i = 0; i < 5; ++i) {
            auto* slider = dialog.findChild<QSlider*>(Sliders[i]);
            QVERIFY2(slider, Sliders[i]);
            QVERIFY(!slider->isHidden());
            QCOMPARE(slider->minimum(), 0);
            QCOMPARE(slider->maximum(), 200);
            QCOMPARE(slider->value(), Defaults[i]);
            const auto stem = QString::fromLatin1(Sliders[i]).mid(7);
            for (const auto& name: {"label_" + stem, "label_" + stem + "_value"}) {
                auto* label = dialog.findChild<QLabel*>(name);
                QVERIFY2(label && !label->isHidden(), qPrintable(name));
            }
        }
        auto* objects = dialog.findChild<QCheckBox*>("checkBox_audio_objects");
        auto* occlusion = dialog.findChild<QCheckBox*>("checkBox_gpu_occlusion");
        QVERIFY(objects && !objects->isHidden());
        QVERIFY(occlusion && !occlusion->isHidden());
        QVERIFY(objects->isChecked());
        QVERIFY(!occlusion->isChecked());
        objects->setChecked(false);
        QVERIFY(!dialog.findChild<QSlider*>("slider_audio_objects")->isEnabled());
        objects->setChecked(true);
        QVERIFY(dialog.findChild<QSlider*>("slider_audio_objects")->isEnabled());
        for (const auto* name: {"checkBox_hdr", "checkBox_perf_overlay", "checkBox_network_connected",
                                "comboBox_keyboard_player", "comboBox_audio_input_device",
                                "slider_controller_volume", "slider_controller_vibration",
                                "button_controller_color"}) {
            QVERIFY2(dialog.findChild<QWidget*>(name), name);
            // uic renames duplicate controls with a numeric suffix; users must see one control.
            const QRegularExpression same_control("^" + QRegularExpression::escape(name) + "[0-9]*$");
            QVERIFY2(dialog.findChildren<QWidget*>(same_control).size() == 1, name);
        }
        tabs->setCurrentWidget(tabs->findChild<QWidget*>("settings_tab"));
        QVERIFY(Capture(dialog));
    }

    void globalDialogEditsPersistAndPerGameResetCannotClobberThem() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        Configuration global;
        EditDialog dialog(global);
        dialog.SetGlobalSettings({});
        for (int i = 0; i < 5; ++i)
            dialog.findChild<QSlider*>(Sliders[i])->setValue(Levels[i]);
        dialog.findChild<QCheckBox*>("checkBox_audio_objects")->setChecked(false);
        dialog.findChild<QCheckBox*>("checkBox_gpu_occlusion")->setChecked(true);
        dialog.save();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QSettings file(temp.filePath("settings.ini"), QSettings::IniFormat);
        global.WriteGlobalOnlySettings(&file);
        for (int i = 0; i < 5; ++i) QCOMPARE(file.value(AudioKeys[i]).toInt(), Levels[i]);
        QCOMPARE(file.value("audio_objects_enabled").toBool(), false);
        QCOMPARE(file.value("gpu_occlusion_accurate").toBool(), true);
        file.sync();
        Configuration loaded;
        loaded.ReadGlobalOnlySettings(&file);
        Configuration per_game;
        per_game.CopyGlobalOnlySettingsFrom(loaded);
        EditDialog game_dialog(per_game);
        for (const auto* name: Sliders) QVERIFY(game_dialog.findChild<QSlider*>(name)->isHidden());
        QVERIFY(game_dialog.findChild<QCheckBox*>("checkBox_audio_objects")->isHidden());
        QVERIFY(game_dialog.findChild<QCheckBox*>("checkBox_gpu_occlusion")->isHidden());
        game_dialog.clear();
        game_dialog.save();
        QCOMPARE(game_dialog.result(), int(QDialog::Accepted));
        QSettings game_file(temp.filePath("game.ini"), QSettings::IniFormat);
        per_game.WriteSettings(&game_file);
        for (const auto* key: AudioKeys) QVERIFY(!game_file.contains(key));
        QVERIFY(!game_file.contains("audio_objects_enabled"));
        QVERIFY(!game_file.contains("gpu_occlusion_accurate"));
        per_game.WriteGlobalOnlySettings(&file);
        for (int i = 0; i < 5; ++i) QCOMPARE(file.value(AudioKeys[i]).toInt(), Levels[i]);
        QCOMPARE(file.value("audio_objects_enabled").toBool(), false);
        QCOMPARE(file.value("gpu_occlusion_accurate").toBool(), true);
    }

    void malformedGlobalVolumesClampWithoutChangingDefaults() {
        QTemporaryDir temp;
        QSettings file(temp.filePath("audio.ini"), QSettings::IniFormat);
        AudioMixSettings mix;
        mix.ReadSettings(&file);
        mix.WriteSettings(&file);
        for (int i = 0; i < 5; ++i) QCOMPARE(file.value(AudioKeys[i]).toInt(), Defaults[i]);
        for (int i = 0; i < 5; ++i) {
            for (const auto& [raw, expected]: std::array<std::pair<QVariant, int>, 4> {
                     std::pair<QVariant, int> {-1, 0}, {201, 200}, {"broken", Defaults[i]}, {0, 0}}) {
                file.setValue(AudioKeys[i], raw);
                mix.ReadSettings(&file);
                mix.WriteSettings(&file);
                QCOMPARE(file.value(AudioKeys[i]).toInt(), expected);
            }
        }
    }

    void newSettingEditsPersistAlongsideGlobalAudio() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        Configuration info;
        SetLevels(info);
        EditDialog dialog(info);
        dialog.SetGlobalSettings({});
        for (const auto* name: {"checkBox_hdr", "checkBox_perf_overlay", "checkBox_network_connected"}) {
            auto* box = dialog.findChild<QCheckBox*>(name);
            QVERIFY2(box, name);
            box->setChecked(true);
        }
        auto* keyboard = dialog.findChild<QComboBox*>("comboBox_keyboard_player");
        QVERIFY(keyboard);
        const auto player = keyboard->findData(3);
        QVERIFY(player >= 0);
        keyboard->setCurrentIndex(player);
        dialog.save();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QSettings file(temp.filePath("new-settings.ini"), QSettings::IniFormat);
        info.WriteSettings(&file);
        info.controller.WriteSettings(&file);
        info.WriteGlobalOnlySettings(&file);
        for (const auto* key: {"hdr_enabled", "perf_overlay_enabled", "network_connected"})
            QVERIFY2(file.value(key).toBool(), key);
        QCOMPARE(file.value("controller_keyboard_player").toInt(), 3);
        for (int i = 0; i < 5; ++i) QCOMPARE(file.value(AudioKeys[i]).toInt(), Levels[i]);
        Configuration reloaded;
        reloaded.ReadSettings(&file);
        reloaded.controller.ReadSettings(&file);
        reloaded.ReadGlobalOnlySettings(&file);
        const auto args = BuildEmulatorArgs(reloaded);
        QVERIFY(args.contains("--hdr"));
        QCOMPARE(Value(args, "--perf-overlay"), "true");
        QCOMPARE(Value(args, "--network-connected"), "true");
        QCOMPARE(Value(args, "--keyboard-player"), "3");
        QCOMPARE(Value(args, "--audio-music-volume"), "137");
        EditDialog per_game(reloaded);
        QVERIFY(!per_game.findChild<QComboBox*>("comboBox_keyboard_player")->isVisibleTo(&per_game));
        per_game.clear();
        per_game.save();
        QCOMPARE(Value(BuildEmulatorArgs(reloaded), "--keyboard-player"), "3");
    }

    void libraryAndStreamingCatalogUseTheSameInheritedAudioAndCheatArguments() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        WorkingDirectory cwd(temp.path());
        const auto game = temp.filePath("games/Space Game");
        QVERIFY(Write(game + "/eboot.bin", "fixture, never executed"));
        QVERIFY(Write(game + "/sce_sys/param.json", R"({"titleId":"PPSA99998","appVersion":"01.018.000","titleName":"Space Game"})"));
        QVERIFY(Write("compatibility_db.json", "{}"));
        Configuration global;
        SetLevels(global);
        global.audio_input_device = "Unavailable fixture microphone";
        global.controller.speaker_volume = 67;
        global.controller.vibration_intensity = 83;
        global.host_input_mapping = {"Cross=J", "MouseSensitivity=1.25"};
        global.screen_resolution = Configuration::Resolution::R2560X1440;
        {
            QSettings file("Kyty.ini", QSettings::IniFormat);
            file.setValue("Launcher/game_dirs", QStringList {temp.filePath("games")});
            file.setValue("Launcher/addons_dir", temp.filePath("Additional Content"));
            file.beginGroup("GlobalConfiguration");
            global.WriteSettings(&file);
            global.controller.WriteSettings(&file);
            global.WriteGlobalOnlySettings(&file);
            file.setValue("controller_keyboard_player", 3);
            file.endGroup();
            file.beginWriteArray("GameConfigurations");
            file.setArrayIndex(0);
            Configuration custom;
            custom.game_path = game;
            custom.screen_resolution = Configuration::Resolution::R1920X1080;
            custom.audio_input_device = "Unavailable fixture microphone";
            custom.audio_mix.master = 1; // A stale per-game global key must be ignored.
            custom.WriteSettings(&file);
            custom.WriteGlobalOnlySettings(&file);
            file.setValue("hdr_enabled", true);
            file.setValue("perf_overlay_enabled", true);
            file.setValue("network_connected", true);
            file.setValue("support_zmask", true);
            file.setValue("trophy_duration_seconds", 8);
            file.endArray();
        }
        const auto patch = Cheats::PlanPath("PPSA99998");
        QVERIFY2(!QFile::exists(patch), "Refuse to replace any existing patch fixture");
        struct RemovePatch { QString path; ~RemovePatch() { QFile::remove(path); } } cleanup {patch};
        QVERIFY(Cheats::SaveDocument(patch, CheatDocument(), {}).isEmpty());
        ConfigurationListWidget library;
        auto* tree = library.findChild<QTreeWidget*>("cfgs_list");
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 1);
        auto* item = dynamic_cast<ConfigurationItem*>(tree->topLevelItem(0));
        QVERIFY(item);
        const auto local = library.CreateConfiguration(*item);
        const auto local_args = BuildEmulatorArgs(*local);
        const auto catalog = library.CreateStreamingCatalog(temp.filePath("kyty_emulator"));
        QCOMPARE(catalog.size(), 1);
        QCOMPARE(catalog.first().arguments, local_args);
        QCOMPARE(Value(local_args, "--screen-width"), "1920");
        QCOMPARE(Value(local_args, "--audio-master-volume"), "0");
        QCOMPARE(Value(local_args, "--audio-main-volume"), "200");
        QCOMPARE(Value(local_args, "--audio-music-volume"), "137");
        QCOMPARE(Value(local_args, "--audio-pad-speaker-volume"), "43");
        QCOMPARE(Value(local_args, "--audio-objects-volume"), "181");
        QCOMPARE(Value(local_args, "--audio-objects"), "off");
        QCOMPARE(Value(local_args, "--gpu-occlusion"), "on");
        QCOMPARE(Value(local_args, "--controller-volume"), "67");
        QCOMPARE(Value(local_args, "--controller-vibration"), "83");
        QCOMPARE(Value(local_args, "--mic"), "Unavailable fixture microphone");
        QVERIFY(local_args.contains("--hdr"));
        QCOMPARE(Value(local_args, "--perf-overlay"), "true");
        QCOMPARE(Value(local_args, "--network-connected"), "true");
        QCOMPARE(Value(local_args,"--support-zmask"),"true");
        QCOMPARE(Value(local_args,"--trophy-duration"),"8");
        QCOMPARE(Value(local_args, "--keyboard-player"), "3");
        QCOMPARE(Value(local_args, "--addcont-dir"), temp.filePath("Additional Content"));
        QCOMPARE(local_args.count("--keymap"), 2);
        QCOMPARE(Value(local_args, "--game-patch"), patch);
        const auto manifest = StreamingCatalog::Manifest(catalog, "/private/socket", "private-sink", "/private/lock", "1080p", 60, false);
        QCOMPARE(manifest.value("games").toObject().value(catalog.first().id).toObject().value("arguments").toArray(), QJsonArray::fromStringList(local_args));
        QCOMPARE(manifest.value("environment").toObject().value("KYTY_STREAM_ONLY").toString(), "1");
    }

    void streamingTabRetainsPairingAudioAndHeadlessPreferences() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        WorkingDirectory cwd(temp.path());
        QVERIFY(Write("Kyty.ini", "[Launcher]\ngame_dirs=\n"));
        QVERIFY(Write("compatibility_db.json", "{}"));
        const auto previous = qgetenv("KYTY_STREAMING_CONFIG_DIR");
        qputenv("KYTY_STREAMING_CONFIG_DIR", temp.filePath("streaming").toUtf8());
        struct Restore { QByteArray old; ~Restore() { old.isNull() ? qunsetenv("KYTY_STREAMING_CONFIG_DIR") : qputenv("KYTY_STREAMING_CONFIG_DIR", old); } } restore {previous};
        MainDialog dialog;
        auto* streaming = dialog.findChild<StreamingWidget*>("streamingWidget");
        QVERIFY(streaming);
        bool library_tab = false, stream_tab = false;
        for (auto* tabs: dialog.findChildren<QTabWidget*>())
            for (int i = 0; i < tabs->count(); ++i) {
                library_tab |= tabs->tabText(i) == "Library";
                stream_tab |= tabs->tabText(i) == "Streaming";
            }
        QVERIFY(library_tab && stream_tab);
        if (!qEnvironmentVariableIsEmpty("KYTY_PRESERVATION_SCREENSHOT")) {
            auto* tabs = qobject_cast<QTabWidget*>(streaming->parentWidget()->parentWidget());
            QVERIFY(tabs);
            tabs->setCurrentIndex(0);
            QVERIFY(Capture(dialog, "-library"));
            tabs->setCurrentWidget(streaming);
            QVERIFY(Capture(dialog, "-streaming"));
        }
        for (const auto* name: {"streamBackendPath", "streamStart", "streamStop", "streamPair", "streamPin",
                               "streamClients", "streamUnpair", "streamPort", "streamControllerAudio",
                               "streamControllerAudioPort", "streamControllerAudioReceivers", "streamLocalAudio"})
            QVERIFY2(streaming->findChild<QWidget*>(name), name);
        streaming->findChild<QCheckBox*>("streamControllerAudio")->setChecked(true);
        streaming->findChild<QSpinBox*>("streamControllerAudioPort")->setValue(49001);
        streaming->findChild<QCheckBox*>("streamLocalAudio")->setChecked(false);
        streaming->findChild<QCheckBox*>("streamShowLocal")->setChecked(false);
        const auto settings = streaming->Settings();
        QVERIFY(settings.controller_audio);
        QCOMPARE(settings.controller_audio_port, 49001);
        QVERIFY(!settings.local_audio);
        QVERIFY(!settings.show_local);
        QVERIFY(!QFile::exists(temp.filePath("streaming/host-key.pem")));
    }

    void cheatImportRejectsWrongIdentityExecutableAndUnsupportedAddressing_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QVariant>("value");
        QTest::newRow("wrong-title") << "id" << QVariant("PPSA00001");
        QTest::newRow("wrong-version") << "version" << QVariant("01.000.000");
        QTest::newRow("wrong-process") << "process" << QVariant("other.bin");
        QTest::newRow("master-codes") << "master" << QVariant(QVariantList {});
        QTest::newRow("module-name") << "mod.module_name" << QVariant("module.prx");
        QTest::newRow("absolute-address") << "memory.absolute" << QVariant(true);
        QTest::newRow("wrong-expected") << "memory.expected" << QVariant("FF");
        QTest::newRow("section") << "memory.section" << QVariant(".text");
        QTest::newRow("bad-offset") << "memory.offset" << QVariant("-12");
        QTest::newRow("odd-bytes") << "memory.on" << QVariant("A");
        QTest::newRow("unequal-write") << "memory.on" << QVariant("FFFF");
    }
    void cheatImportRejectsWrongIdentityExecutableAndUnsupportedAddressing() {
        QFETCH(QString, field);
        QFETCH(QVariant, value);
        auto root = CheatDocument().object();
        auto mods = root["mods"].toArray();
        auto mod = mods[0].toObject();
        auto memory = mod["memory"].toArray();
        auto write = memory[0].toObject();
        if (field.startsWith("memory.")) {
            write[field.mid(7)] = QJsonValue::fromVariant(value);
            memory[0] = write;
            mod["memory"] = memory;
            mods[0] = mod;
            root["mods"] = mods;
        } else if (field.startsWith("mod.")) {
            mod[field.mid(4)] = QJsonValue::fromVariant(value);
            mods[0] = mod;
            root["mods"] = mods;
        } else root[field] = QJsonValue::fromVariant(value);
        QVERIFY(!Cheats::ValidateImport(QJsonDocument(root), "PPSA99998", "01.018.000", "eboot.bin").isEmpty());
    }

    void cheatSelectionRoundTripsAndRejectsConcurrentReplacement() {
        QTemporaryDir temp;
        const auto path = temp.filePath("_Patches/PPSA99998.json");
        const auto original = CheatDocument();
        QVERIFY(Cheats::ValidateImport(original, "ppsa99998", "01.018.000", "EBOOT.BIN").isEmpty());
        QVERIFY(Cheats::SaveDocument(path, original, {}).isEmpty());
        const auto bytes = Read(path);
        const auto changed = Cheats::WithSelection(original, {true, false});
        QVERIFY(!changed.isNull());
        QVERIFY(Cheats::WithSelection(original, {true}).isNull());
        QVERIFY(Cheats::SaveDocument(path, changed, bytes).isEmpty());
        QJsonDocument loaded;
        QVERIFY(Cheats::ParseDocument(Read(path), &loaded).isEmpty());
        const auto mods = loaded.object()["mods"].toArray();
        QCOMPARE(mods[0].toObject()["enabled"].toBool(), true);
        QCOMPARE(mods[1].toObject()["enabled"].toBool(), false);
        QCOMPARE(loaded.object()["version"].toString(), "01.018.000");
        QCOMPARE(mods[0].toObject()["memory"], original.object()["mods"].toArray()[0].toObject()["memory"]);
        const auto external = QByteArray("external editor's replacement\n");
        QVERIFY(Write(path, external));
        QVERIFY(!Cheats::SaveDocument(path, original, bytes).isEmpty());
        QCOMPARE(Read(path), external);
        QVERIFY(!Cheats::SaveDocument(path, original, {}).isEmpty());
        QCOMPARE(Read(path), external);
        QVERIFY(!Cheats::ParseDocument("{broken", &loaded).isEmpty());
        QVERIFY(loaded.isNull());
    }

    void cheatCatalogFiltersMalformedDuplicateAndUnsupportedRemoteFiles() {
        const auto result = Cheats::ParseIndex(
            "PPSA99998_01.018.000_A.json=First\nPPSA99998_01.018.000_A.json=Duplicate\n"
            "PPSA00001_01.018.000_B.json=Other title\n../PPSA99998_01.018.000_C.json=Traversal\n"
            "PPSA99998_01.018.000_D.shn=Wrong extension\n", "json", "ppsa99998");
        QCOMPARE(result.files.size(), 1);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(result.files[0].IsSupported());
        QCOMPARE(result.files[0].Url().path(), "/TeeKay87/HEN-Cheats-Collection/master/cheats/json/PPSA99998_01.018.000_A.json");
        QVERIFY((!Cheats::RemoteFile {"PPSA99998_01.018.000_A.shn", {}, {}, "shn"}.IsSupported()));
        QVERIFY((Cheats::RemoteFile {"../file.json", {}, {}, "json"}.Url().isEmpty()));
    }
};

int main(int argc, char** argv) {
    // --local selects the real offline compatibility database seam. Keep it out of QtTest's CLI.
    std::vector<char*> app_args(argv, argv + argc);
    char local[] = "--local";
    app_args.push_back(local);
    const bool headless_fixture=argc==2 && QByteArray(argv[1])=="--headless-ui-startup-fixture";
    char start_host[]="--start-streaming-host";
    if(headless_fixture) app_args.push_back(start_host);
    app_args.push_back(nullptr);
    int app_argc = argc + 1 + int(headless_fixture);
    QApplication app(app_argc, app_args.data());
    QStandardPaths::setTestModeEnabled(true);
    if(qEnvironmentVariable("KYTY_PRESERVATION_DARK")=="1") {
        auto palette=app.palette();
        palette.setColor(QPalette::Window,QColor("#24272b"));
        palette.setColor(QPalette::WindowText,QColor("#eff0f1"));
        palette.setColor(QPalette::Base,QColor("#1b1e20"));
        palette.setColor(QPalette::Text,QColor("#eff0f1"));
        palette.setColor(QPalette::Button,QColor("#31363b"));
        palette.setColor(QPalette::ButtonText,QColor("#eff0f1"));
        palette.setColor(QPalette::Mid,QColor("#60656a"));
        palette.setColor(QPalette::Highlight,QColor("#2d74ad"));
        palette.setColor(QPalette::HighlightedText,Qt::white);
        app.setPalette(palette);
    }
    const bool real_prepare_fixture=argc==2 && QByteArray(argv[1])=="--precompile-real-ui-fixture";
    if((argc==2 && QByteArray(argv[1])=="--precompile-ui-fixture") || real_prepare_fixture) {
        LauncherTheme::Initialize(app);
        MainDialog dialog;dialog.show();dialog.Start();
        auto* tree=dialog.findChild<QTreeWidget*>("cfgs_list");
        auto* button=dialog.findChild<QPushButton*>("precompileShadersButton");
        auto* tabs=dialog.findChild<QTabWidget*>();
        if(!tree || !button || !tabs || tree->topLevelItemCount()<(real_prepare_fixture ? 1 : 3))return 21;
        tree->setCurrentItem(tree->topLevelItem(real_prepare_fixture ? 0 : 2));
        bool clicked=false,observed=false;
        QElapsedTimer deadline;deadline.start();
        while(deadline.elapsed()<(real_prepare_fixture ? 180000 : 7000)) {
            QCoreApplication::processEvents(QEventLoop::AllEvents,20);
            if(!clicked && button->isEnabled()) {
                if(!Capture(dialog,"-precompile-library"))return 22;
                QTest::mouseClick(button,Qt::LeftButton);clicked=true;
            }
            auto* progress=dialog.findChild<ShaderPrecompileDialog*>("shaderPrecompileDialog");
            if(!progress)continue;
            auto* bar=progress->findChild<QProgressBar*>("precompileProgress");
            if(real_prepare_fixture && !progress->IsRunning()) {
                if(!progress->findChild<QLabel*>("precompileStatus")->text().contains("Ready to play"))return 26;
                if(!Capture(*progress,"-precompile-selected-complete",false))return 27;
                return 0;
            }
            if((bar->value()==250 || real_prepare_fixture) && !observed) {
                if(button->isEnabled() || !progress->IsRunning())return 23;
                if(!Capture(*progress,"-precompile-selected",false))return 24;
                if(!real_prepare_fixture && !Write(QCoreApplication::applicationDirPath()+"/ui-ack",""))return 25;
                observed=true;
            }
            if(observed && !progress->IsRunning()) {
                if(!button->isEnabled() || !progress->findChild<QLabel*>("precompileStatus")->text().contains("Ready to play"))return 26;
                if(!Capture(*progress,"-precompile-selected-complete",false))return 27;
                return 0;
            }
        }
        return 28;
    }
    if(headless_fixture) {
        MainDialog dialog;
        auto* streaming=dialog.findChild<StreamingWidget*>("streamingWidget");
        auto* title=static_cast<TitleSound*>(dialog.findChild<QObject*>("titleSound"));
        auto* preview=dialog.findChild<ControllerPreview*>("controllerPreview");
        auto* backend=dialog.findChild<StreamingBackend*>();
        if(!streaming || !title || !preview || !backend) return 11;
        auto* tabs=qobject_cast<QTabWidget*>(streaming->parentWidget()->parentWidget());
        if(!tabs || tabs->currentWidget()!=streaming || !title->Title().isEmpty()) return 12;
        bool paused=false,quiet=true;
        QObject::connect(backend,&StreamingBackend::Changed,&dialog,[&] {
            quiet &= title->Title().isEmpty();
            if(backend->GetState()==StreamingBackend::State::Starting)
                paused=preview->Error().contains("paused");
        });
        dialog.Start();
        QElapsedTimer deadline; deadline.start();
        while(deadline.elapsed()<3000 && backend->GetState()!=StreamingBackend::State::Error)
            QCoreApplication::processEvents(QEventLoop::AllEvents,20);
        return paused && quiet && backend->GetState()==StreamingBackend::State::Error ? 0 : 13;
    }
    LauncherPreservationTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "LauncherPreservationTests.moc"
