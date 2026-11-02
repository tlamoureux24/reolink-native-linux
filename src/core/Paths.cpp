#include "Paths.h"

#include "AppSettings.h"
#include <QDir>
#include <QStandardPaths>

namespace rl {

static QString ensured(const QString &path)
{
    QDir().mkpath(path);
    return path;
}

QString Paths::dataDir()
{
    return ensured(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
}

QString Paths::configDir()
{
    return ensured(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
}

QString Paths::databaseFile()
{
    return dataDir() + QStringLiteral("/reolink.db");
}

QString Paths::thumbnailsDir()
{
    return ensured(dataDir() + QStringLiteral("/thumbnails"));
}

QString Paths::recordingsDir()
{
    const QString custom = AppSettings::customRecordingsDir(); // Settings > Client
    if (!custom.isEmpty())
        return ensured(custom);
    return ensured(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)
                   + QStringLiteral("/Reolink"));
}

} // namespace rl
