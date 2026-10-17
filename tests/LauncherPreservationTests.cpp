#include "cheatFile.h"
#include "cheatRepository.h"
#include "configuration.h"
#include "configurationEditDialog.h"
#include "configurationItem.h"
#include "configurationListWidget.h"
#include "emulatorCommand.h"
#include "mainDialog.h"
#include "streamingCatalog.h"
#include "streamingWidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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
                                "button_controller_color"})
            QVERIFY2(dialog.findChild<QWidget*>(name), name);
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
            file.beginGroup("GlobalConfiguration");
            global.WriteSettings(&file);
            global.controller.WriteSettings(&file);
            global.WriteGlobalOnlySettings(&file);
            file.endGroup();
            file.beginWriteArray("GameConfigurations");
            file.setArrayIndex(0);
            Configuration custom;
            custom.game_path = game;
            custom.screen_resolution = Configuration::Resolution::R1920X1080;
            custom.audio_mix.master = 1; // A stale per-game global key must be ignored.
            custom.WriteSettings(&file);
            custom.WriteGlobalOnlySettings(&file);
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
    app_args.push_back(nullptr);
    int app_argc = argc + 1;
    QApplication app(app_argc, app_args.data());
    QStandardPaths::setTestModeEnabled(true);
    LauncherPreservationTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "LauncherPreservationTests.moc"
