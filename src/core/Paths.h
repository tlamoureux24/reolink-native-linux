#pragma once

#include <QString>

namespace rl {

// Application directories (XDG-compliant via QStandardPaths). Created on first use.
struct Paths {
    static QString dataDir();      // ~/.local/share/reolink-client
    static QString configDir();    // ~/.config/reolink-client
    static QString databaseFile(); // <dataDir>/reolink.db
    static QString recordingsDir(); // ~/Videos/Reolink unless set in client settings
    static QString thumbnailsDir(); // <dataDir>/thumbnails (event snapshots)
};

} // namespace rl
