// bc-probe: open one device's Baichuan session and print the decrypted replies
// to the commands given — for checking a device's real answers while moving a
// feature to Baichuan. Read-only unless told to send a Set command.
//
//   RL_PASSWORD=… bc-probe --host 192.168.1.20 --cmd 80:250 --cmd 199:250 \
//       --cmd 56:1 --listen 5
//
// --cmd ID[:CH] sends command ID with header channel byte CH (default 250 =
// host level); for a channel-scoped command pass CH = channel + 1, and the
// Extension with the 0-based channel id is added. The password comes from the
// environment only, so it never shows on a command line or in logs.

#include "protocol/BcCrypto.h"
#include "protocol/BcDevice.h"
#include "protocol/BcRecordings.h"
#include "protocol/BcSettings.h"
#include "protocol/BaichuanClient.h"
#include "media/BcMediaParser.h"
#include "media/BcClipWriter.h"
#include "protocol/ReolinkApi.h"
#include "protocol/BcSession.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QFile>
#include <QJsonDocument>
#include <QMutex>

#include <cstdio>

using namespace rl;

static QByteArray decrypt(const BcMessage &m, const QByteArray &key, QByteArray *ext)
{
    const int off = int(m.payloadOffset);
    const QByteArray e = off > 0 ? m.body.left(off) : QByteArray();
    const QByteArray b = off > 0 ? m.body.mid(off) : m.body;
    if (ext)
        *ext = e.isEmpty() ? e : bc::aesCfb(e, key, true);
    return b.isEmpty() ? b : bc::aesCfb(b, key, true);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser cli;
    cli.addHelpOption();
    cli.addOptions({
        {"host", "Device address (TCP 9000).", "ip"},
        {"uid", "Device UID (Baichuan over UDP); --host is then a LAN hint.", "uid"},
        {"user", "User name.", "name", "admin"},
        {"cmd", "Send command ID with header channel CH (repeatable).", "id[:ch]"},
        {"ext", "Extension XML for every --cmd (default: <channelId> for CH < 250).", "xml"},
        {"body", "Body XML for every --cmd.", "xml"},
        {"listen", "Then print device pushes for this many seconds.", "s", "0"},
        {"login", "Print the login reply."},
        {"channel", "Channel (0-based) for --bc-get / --bc-set.", "n", "0"},
        {"bc-get", "Print this HTTP Get command name's value and range as the app turns the "
                   "Baichuan answer into the HTTP JSON (repeatable).", "cmd"},
        {"bc-set", "Write this HTTP Set command through the app's translation, with "
                   "--param as its JSON parameter (changes the device!).", "cmd"},
        {"param", "JSON parameter for --bc-set.", "json"},
    });
    cli.process(app);
    const QString password = qEnvironmentVariable("RL_PASSWORD");
    if ((!cli.isSet("host") && !cli.isSet("uid")) || password.isEmpty()) {
        std::fprintf(stderr, "need --host or --uid, and RL_PASSWORD in the environment\n");
        return 2;
    }

    BcSession::Params sp;
    sp.target = {cli.value("host"), 9000, cli.value("uid"), cli.isSet("uid")};
    sp.username = cli.value("user");
    sp.password = password;
    QElapsedTimer t;
    t.start();
    const std::shared_ptr<BcSession> s = BcSession::acquire(sp);
    if (!s->waitReady(30000)) {
        std::printf("FAILED: %s\n", qPrintable(s->error()));
        return 1;
    }
    std::printf("session ready in %lld ms (%s)\n", t.elapsed(), qPrintable(s->linkDescription()));
    if (cli.isSet("login"))
        std::printf("--- login reply\n%s\n", s->loginInfo().constData());
    const QByteArray key = s->aesKey();

    const int token = s->addPushHandler([key](const BcMessage &m) {
        QByteArray ext;
        const QByteArray body = decrypt(m, key, &ext);
        std::printf("--- push cmd %u ch %u status %u\n%s%s\n", m.cmd, m.channel, m.status,
                    ext.constData(), body.constData());
        std::fflush(stdout);
    });

    for (const QString &spec : cli.values("cmd")) {
        const quint32 cmd = spec.section(u':', 0, 0).toUInt();
        const int ch = spec.contains(u':') ? spec.section(u':', 1, 1).toInt() : 250;
        QByteArray extXml = cli.value("ext").toUtf8();
        if (extXml.isEmpty() && ch < 250)
            extXml = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<Extension version=\"1.1\">\n"
                     "<channelId>" + QByteArray::number(ch - 1) + "</channelId>\n</Extension>\n";
        const QByteArray bodyXml = cli.value("body").toUtf8();
        BcMessage reply;
        t.restart();
        const bool ok = s->request(cmd, quint8(ch), extXml.isEmpty() ? QByteArray() : bc::aesCfb(extXml, key, false),
                                   bodyXml.isEmpty() ? QByteArray() : bc::aesCfb(bodyXml, key, false),
                                   &reply);
        if (!ok) {
            std::printf("--- cmd %u ch %d: no reply\n", cmd, ch);
            continue;
        }
        QByteArray ext;
        const QByteArray body = decrypt(reply, key, &ext);
        std::printf("--- cmd %u ch %d: status %u, %lld ms\n%s%s\n", cmd, ch, reply.status, t.elapsed(),
                    ext.constData(), body.constData());
    }

    const int channel = cli.value("channel").toInt();
    for (const QString &cmd : cli.values("bc-get")) {
        QVariant value, range;
        QString error;
        const bool ok = bcset::get(*s, channel, cmd, &value, &range, &error, sp.username);
        std::printf("--- %s: %s\n%s\nrange %s\n", qPrintable(cmd), ok ? "ok" : qPrintable("FAILED " + error),
                    QJsonDocument::fromVariant(value).toJson().constData(),
                    QJsonDocument::fromVariant(range).toJson(QJsonDocument::Compact).constData());
    }
    if (cli.isSet("bc-set")) {
        const QString cmd = cli.value("bc-set");
        const QVariantMap param = QJsonDocument::fromJson(cli.value("param").toUtf8()).toVariant().toMap();
        QString error;
        const bool ok = !param.isEmpty() && bcset::set(*s, channel, cmd, param, &error, sp.username);
        std::printf("--- %s: %s\n", qPrintable(cmd),
                    ok ? "ok" : qPrintable("FAILED " + (param.isEmpty() ? QStringLiteral("bad --param") : error)));
    }

    const int listen = cli.value("listen").toInt();
    if (listen > 0)
        QThread::sleep(static_cast<unsigned long>(listen));
    s->removePushHandler(token);
    return 0;
}
