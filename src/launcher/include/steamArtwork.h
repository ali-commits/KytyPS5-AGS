#pragma once

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QStringList>
#ifdef __unix__
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Public client cache only. Configured root aliases are resolved once; every component
// below that boundary is opened without following links. No account/profile paths escape.
namespace SteamArtwork {
inline QByteArray Read(const QString& root, const QString& relative) {
    constexpr qsizetype limit = 16 * 1024 * 1024;
    const auto canonical = QFileInfo(root).canonicalFilePath();
    if (canonical.isEmpty()) return {};
#ifdef __unix__
    int fd = ::open(QFile::encodeName(canonical).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return {};
    const auto parts = relative.split('/');
    for (int i = 0; i < parts.size(); ++i) {
        if (parts[i].isEmpty() || parts[i] == "." || parts[i] == "..") { ::close(fd); return {}; }
        const int next = ::openat(fd, QFile::encodeName(parts[i]).constData(),
            O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW | (i + 1 < parts.size() ? O_DIRECTORY : 0));
        ::close(fd);
        if (next < 0) return {};
        fd = next;
    }
    struct stat st {};
    if (::fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > limit) { ::close(fd); return {}; }
    QByteArray data;
    char buffer[16384];
    while (data.size() <= limit) {
        const auto n = ::read(fd, buffer, qMin<qsizetype>(sizeof(buffer), limit + 1 - data.size()));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ::close(fd); return n == 0 && data.size() <= limit ? data : QByteArray {}; }
        data.append(buffer, n);
    }
    ::close(fd);
    return {};
#else
    QString path = canonical;
    for (const auto& part : relative.split('/')) {
        if (part.isEmpty() || part == "." || part == "..") return {};
        path += '/' + part;
        if (QFileInfo(path).isSymLink()) return {};
    }
    QFile f(path);
    if (!QFileInfo(path).isFile() || !f.open(QIODevice::ReadOnly) || f.size() > limit) return {};
    const auto data = f.read(limit + 1);
    return data.size() <= limit ? data : QByteArray {};
#endif
}
inline QByteArray Load(const QStringList& roots, const QString& app, bool background = false) {
    static const QRegularExpression id("\\A[0-9]{1,10}\\z");
    if (!id.match(app).hasMatch()) return {};
    const QStringList kinds = background ? QStringList {"library_hero", "library_600x900"}
                                         : QStringList {"library_600x900"};
    for (const auto& kind : kinds) for (const auto& root : roots) {
        const QStringList paths = {"appcache/librarycache/" + app + '/' + kind + ".jpg",
                                   "appcache/librarycache/" + app + '_' + kind + ".jpg"};
        for (const auto& path : paths) {
            auto data = Read(root, path);
            QBuffer buffer(&data);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer);
            const auto format = reader.format();
            if (format != "jpeg" && format != "png") continue;
            const auto size = reader.size();
            if (!size.isValid() || size.width() > 8192 || size.height() > 8192 ||
                qint64(size.width()) * size.height() > 32 * 1024 * 1024) continue;
            if (!reader.read().isNull()) return data;
        }
    }
    return {};
}
} // namespace SteamArtwork
