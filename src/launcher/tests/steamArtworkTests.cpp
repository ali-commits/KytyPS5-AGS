#include "steamArtwork.h"
#include <QCoreApplication>
#include <QImage>
#include <QTemporaryDir>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#ifdef __unix__
#include <sys/stat.h>
#endif
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    assert(temp.isValid());
    const auto root = temp.path();
    const auto cache = root + "/appcache/librarycache";
    QDir().mkpath(cache + "/620");
    QImage image(16, 24, QImage::Format_RGB32);
    image.fill(Qt::red);
    assert(image.save(cache + "/620/library_600x900.jpg", "JPG"));
    const auto cover = SteamArtwork::Load({root}, "620");
    assert(!cover.isEmpty());
    assert(SteamArtwork::Load({root}, "620", true) == cover);
    image.fill(Qt::blue);
    assert(image.save(cache + "/620/library_hero.jpg", "JPG"));
    assert(SteamArtwork::Load({root}, "620", true) != cover);
    assert(image.save(cache + "/730_library_600x900.jpg", "JPG"));
    assert(!SteamArtwork::Load({root}, "730").isEmpty());
    assert(SteamArtwork::Load({root}, "999").isEmpty());
    assert(SteamArtwork::Load({root}, "../620").isEmpty());
    QFile garbage(cache + "/999_library_600x900.jpg");
    assert(garbage.open(QIODevice::WriteOnly)); garbage.write("not an image"); garbage.close();
    assert(SteamArtwork::Load({root}, "999").isEmpty());
#ifdef __unix__
    assert(QFile::link(cache + "/620/library_600x900.jpg", cache + "/1_library_600x900.jpg"));
    assert(SteamArtwork::Load({root}, "1").isEmpty());
    assert(QFile::link(cache + "/620", cache + "/2"));
    assert(SteamArtwork::Load({root}, "2").isEmpty());
    assert(::mkfifo(QFile::encodeName(cache + "/3_library_600x900.jpg").constData(), 0600) == 0);
    assert(SteamArtwork::Load({root}, "3").isEmpty());
    assert(QFile::link(root, root + "-alias"));
    assert(SteamArtwork::Load({root + "-alias"}, "620") == cover);
    QFile::remove(root + "-alias");
#endif
    QFile huge(cache + "/4_library_600x900.jpg");
    assert(huge.open(QIODevice::WriteOnly)); assert(huge.resize(17 * 1024 * 1024)); huge.close();
    assert(SteamArtwork::Load({root}, "4").isEmpty());
    QImage wide(8193, 1, QImage::Format_RGB32); wide.fill(Qt::red);
    assert(wide.save(cache + "/5_library_600x900.jpg", "PNG"));
    assert(SteamArtwork::Load({root}, "5").isEmpty());
    return 0;
}
