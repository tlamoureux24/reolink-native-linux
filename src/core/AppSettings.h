#pragma once

#include <QObject>
#include <QString>
#include <QTranslator>
#include <QVariantList>

#include <atomic>
#include <functional>

namespace rl {

// Client (app-level) preferences — the official client's top-right gear.
// Persisted in QSettings under "app/…". Exposed to QML as the AppSettings
// singleton; core code reads the few it needs through the static accessors.
class AppSettings : public QObject
{
    Q_OBJECT
    // "" follows the system locale; otherwise "en", "fr", "es".
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(QVariantList languages READ languages CONSTANT)
    // Locale actually in use ("fr_FR", "en_US"…): dates, day and month names
    // follow the chosen language, not only the system's (Qt.locale(localeName)).
    Q_PROPERTY(QString localeName READ localeName NOTIFY languageChanged)
    Q_PROPERTY(QString systemLanguageName READ systemLanguageName CONSTANT)
    // Colour theme: "dark" (default), "light", or "system" to follow the
    // desktop's light/dark preference. Theme.qml picks its palette from it.
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(bool hardwareDecoding READ hardwareDecoding WRITE setHardwareDecoding NOTIFY hardwareDecodingChanged)
    Q_PROPERTY(QString recordingsDir READ recordingsDir WRITE setRecordingsDir NOTIFY recordingsDirChanged)
    Q_PROPERTY(bool desktopNotifications READ desktopNotifications WRITE setDesktopNotifications NOTIFY desktopNotificationsChanged)
    Q_PROPERTY(bool checkUpdates READ checkUpdates WRITE setCheckUpdates NOTIFY checkUpdatesChanged)
    // Live view "tour": seconds each camera stays maximized while touring.
    Q_PROPERTY(int tourSeconds READ tourSeconds WRITE setTourSeconds NOTIFY tourSecondsChanged)
    // Two-way talk: microphone gain in percent (10..200), and muting the
    // camera's sound while talking so speaker->mic feedback can't build up.
    Q_PROPERTY(int talkGain READ talkGain WRITE setTalkGain NOTIFY talkGainChanged)
    Q_PROPERTY(bool muteWhileTalking READ muteWhileTalking WRITE setMuteWhileTalking NOTIFY muteWhileTalkingChanged)

public:
    explicit AppSettings(QObject *parent = nullptr);

    QString language() const { return m_language; }
    void setLanguage(const QString &code);
    QVariantList languages() const;
    QString localeName() const;
    QString systemLanguageName() const;

    QString theme() const { return m_theme; }
    void setTheme(const QString &theme);

    bool hardwareDecoding() const { return s_hardwareDecoding.load(); }
    void setHardwareDecoding(bool on);

    QString recordingsDir() const;
    void setRecordingsDir(const QString &dir);

    bool desktopNotifications() const { return s_desktopNotifications.load(); }
    void setDesktopNotifications(bool on);

    bool checkUpdates() const { return m_checkUpdates; }
    void setCheckUpdates(bool on);

    int tourSeconds() const { return m_tourSeconds; }
    void setTourSeconds(int seconds);

    int talkGain() const { return s_talkGain.load(); }
    void setTalkGain(int percent);
    bool muteWhileTalking() const { return m_muteWhileTalking; }
    void setMuteWhileTalking(bool on);
    static int talkGainPercent() { return s_talkGain.load(); }

    // Installs the translator for the current language. `retranslate` is called
    // after every later switch so QML re-evaluates its qsTr() bindings.
    void installTranslator(std::function<void()> retranslate);

    // Read from worker threads / core code without a QObject.
    static bool hardwareDecodingEnabled() { return s_hardwareDecoding.load(); }
    static bool desktopNotificationsEnabled() { return s_desktopNotifications.load(); }
    static QString customRecordingsDir(); // empty when the default applies

signals:
    void languageChanged();
    void themeChanged();
    void hardwareDecodingChanged();
    void recordingsDirChanged();
    void desktopNotificationsChanged();
    void checkUpdatesChanged();
    void tourSecondsChanged();
    void talkGainChanged();
    void muteWhileTalkingChanged();

private:
    bool loadTranslation();

    QString m_language;
    QString m_theme;
    bool m_checkUpdates = true;
    int m_tourSeconds = 10;
    bool m_muteWhileTalking = true;
    QTranslator m_translator;
    bool m_translatorInstalled = false;
    std::function<void()> m_retranslate;

    static std::atomic<bool> s_hardwareDecoding;
    static std::atomic<bool> s_desktopNotifications;
    static std::atomic<int> s_talkGain;
};

} // namespace rl
