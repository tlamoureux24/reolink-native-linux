#include "AppSettings.h"

#include "Log.h"

#include <QCoreApplication>
#include <QDir>
#include <QLocale>
#include <QSettings>

namespace rl {

std::atomic<bool> AppSettings::s_hardwareDecoding{true};
std::atomic<bool> AppSettings::s_desktopNotifications{true};
std::atomic<int> AppSettings::s_talkGain{100};

namespace {
const auto kLanguage = QStringLiteral("app/language");
const auto kTheme = QStringLiteral("app/theme");

QString validTheme(const QString &t)
{
    return t == QLatin1String("light") || t == QLatin1String("system") ? t : QStringLiteral("dark");
}
const auto kHwDecode = QStringLiteral("app/hardwareDecoding");
const auto kRecordings = QStringLiteral("app/recordingsDir");
const auto kNotify = QStringLiteral("app/desktopNotifications");
const auto kUpdates = QStringLiteral("app/checkUpdates");
const auto kTour = QStringLiteral("app/tourSeconds");
const auto kTalkGain = QStringLiteral("app/talkGain");
const auto kTalkMute = QStringLiteral("app/muteWhileTalking");
} // namespace

AppSettings::AppSettings(QObject *parent)
    : QObject(parent)
{
    QSettings s;
    m_language = s.value(kLanguage).toString();
    m_theme = validTheme(s.value(kTheme).toString());
    // RL_HWDECODE=0 still forces software decode (diagnostics), as before.
    s_hardwareDecoding = s.value(kHwDecode, true).toBool() && qgetenv("RL_HWDECODE") != "0";
    s_desktopNotifications = s.value(kNotify, true).toBool();
    m_checkUpdates = s.value(kUpdates, true).toBool();
    m_tourSeconds = qBound(3, s.value(kTour, 10).toInt(), 300);
    s_talkGain = qBound(10, s.value(kTalkGain, 100).toInt(), 200);
    m_muteWhileTalking = s.value(kTalkMute, true).toBool();
}

QVariantList AppSettings::languages() const
{
    // Native names, so a user can find their language whatever the current one.
    return {
        QVariantMap{{QStringLiteral("code"), QString()}, {QStringLiteral("name"), QString()}},
        QVariantMap{{QStringLiteral("code"), QStringLiteral("en")}, {QStringLiteral("name"), QStringLiteral("English")}},
        QVariantMap{{QStringLiteral("code"), QStringLiteral("fr")}, {QStringLiteral("name"), QStringLiteral("Français")}},
        QVariantMap{{QStringLiteral("code"), QStringLiteral("es")}, {QStringLiteral("name"), QStringLiteral("Español")}},
    };
}

QString AppSettings::localeName() const
{
    return (m_language.isEmpty() ? QLocale::system() : QLocale(m_language)).name();
}

QString AppSettings::systemLanguageName() const
{
    return QLocale::system().nativeLanguageName();
}

bool AppSettings::loadTranslation()
{
    if (m_translatorInstalled) {
        QCoreApplication::removeTranslator(&m_translator);
        m_translatorInstalled = false;
    }
    // qt_add_translations embeds the .qm files under :/i18n.
    const QLocale locale = m_language.isEmpty() ? QLocale::system() : QLocale(m_language);
    QLocale::setDefault(locale); // QLocale() in C++ formats dates in this language
    if (m_translator.load(locale, QStringLiteral("reolink-client"), QStringLiteral("_"),
                          QStringLiteral(":/i18n"))) {
        QCoreApplication::installTranslator(&m_translator);
        m_translatorInstalled = true;
    }
    return m_translatorInstalled;
}

void AppSettings::installTranslator(std::function<void()> retranslate)
{
    m_retranslate = std::move(retranslate);
    loadTranslation();
}

void AppSettings::setLanguage(const QString &code)
{
    if (m_language == code)
        return;
    m_language = code;
    QSettings().setValue(kLanguage, code);
    loadTranslation();
    if (m_retranslate)
        m_retranslate();
    emit languageChanged();
}

void AppSettings::setHardwareDecoding(bool on)
{
    if (s_hardwareDecoding == on)
        return;
    s_hardwareDecoding = on;
    QSettings().setValue(kHwDecode, on);
    emit hardwareDecodingChanged(); // applies to streams opened from now on
}

QString AppSettings::customRecordingsDir()
{
    return QSettings().value(kRecordings).toString();
}

QString AppSettings::recordingsDir() const
{
    const QString custom = customRecordingsDir();
    return custom.isEmpty() ? QString() : QDir::cleanPath(custom);
}

void AppSettings::setRecordingsDir(const QString &dir)
{
    const QString clean = dir.isEmpty() ? QString() : QDir::cleanPath(dir);
    if (clean == recordingsDir())
        return;
    if (clean.isEmpty())
        QSettings().remove(kRecordings);
    else
        QSettings().setValue(kRecordings, clean);
    emit recordingsDirChanged();
}

void AppSettings::setDesktopNotifications(bool on)
{
    if (s_desktopNotifications == on)
        return;
    s_desktopNotifications = on;
    QSettings().setValue(kNotify, on);
    emit desktopNotificationsChanged();
}

void AppSettings::setTheme(const QString &theme)
{
    const QString t = validTheme(theme);
    if (m_theme == t)
        return;
    m_theme = t;
    QSettings().setValue(kTheme, t);
    emit themeChanged();
}

void AppSettings::setCheckUpdates(bool on)
{
    if (m_checkUpdates == on)
        return;
    m_checkUpdates = on;
    QSettings().setValue(kUpdates, on);
    emit checkUpdatesChanged();
}

void AppSettings::setTourSeconds(int seconds)
{
    seconds = qBound(3, seconds, 300);
    if (m_tourSeconds == seconds)
        return;
    m_tourSeconds = seconds;
    QSettings().setValue(kTour, seconds);
    emit tourSecondsChanged();
}

void AppSettings::setTalkGain(int percent)
{
    percent = qBound(10, percent, 200);
    if (s_talkGain == percent)
        return;
    s_talkGain = percent; // read live by TalkSession
    QSettings().setValue(kTalkGain, percent);
    emit talkGainChanged();
}

void AppSettings::setMuteWhileTalking(bool on)
{
    if (m_muteWhileTalking == on)
        return;
    m_muteWhileTalking = on;
    QSettings().setValue(kTalkMute, on);
    emit muteWhileTalkingChanged();
}

} // namespace rl
