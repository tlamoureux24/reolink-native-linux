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
#include "protocol/BaichuanClient.h"
#include "media/BcMediaParser.h"
#include "protocol/ReolinkApi.h"
#include "protocol/ReolinkHttpClient.h"
#include "protocol/BcSession.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QFile>
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
        {"search-day", "List a day's recordings (cmd 14/15) and the month's days (cmd 142).", "yyyy-mm-dd"},
        {"main", "With --search-day / --replay: the main stream (default sub)."},
        {"replay", "With --search-day: play the day's first file (cmd 5) for 12 s with this "
                   "<playSpeed> (0 = none) and report how fast the device sends it.", "speed"},
        {"play", "With --search-day: play from this time of day through BaichuanClient for "
                 "--seconds at --rate, as the app does, and report the pace.", "hh:mm:ss"},
        {"seconds", "With --play: how long.", "s", "30"},
        {"rate", "With --play: playback speed.", "x", "1"},
        {"seek", "With --play: halfway, seek to this time of day.", "hh:mm:ss"},
        {"snapshot", "Take a snapshot (cmd 109) into this file (--main for full size).", "file"},
        {"compare", "Read the device over Baichuan and over HTTP (GetDevInfo, GetAbility, "
                    "GetEnc) and print both, field by field."},
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

    if (cli.isSet("compare")) {
        bcdev::Device d;
        t.restart();
        const bool ok = bcdev::readDevice(*s, sp.username, &d);
        std::printf("baichuan read: %s in %lld ms\n", ok ? "ok" : "FAILED", t.elapsed());
        ReolinkHttpClient http(cli.value("host"), 443, true, sp.username, password);
        const api::BatchResult b = http.call(Json::array({
            api::command(QStringLiteral("GetDevInfo")),
            api::command(QStringLiteral("GetEnc"), Json{{"channel", 0}}, 1),
            api::command(QStringLiteral("GetAbility"),
                         Json{{"User", {{"userName", sp.username.toStdString()}}}}),
        }));
        QString hName, hModel;
        QSize hMain, hSub;
        api::Capabilities hc;
        for (const api::CommandResult &r : b.results) {
            if (!r.ok)
                continue;
            if (r.cmd == QLatin1String("GetDevInfo")) {
                const Json &i = r.value["DevInfo"];
                hName = QString::fromStdString(i.value("name", ""));
                hModel = QString::fromStdString(i.value("model", ""));
            } else if (r.cmd == QLatin1String("GetEnc")) {
                const Json &e = r.value["Enc"];
                auto sz = [&](const char *k) {
                    const QString v = QString::fromStdString(e[k].value("size", ""));
                    return QSize(v.section(u'*', 0, 0).toInt(), v.section(u'*', 1, 1).toInt());
                };
                hMain = sz("mainStream");
                hSub = sz("subStream");
            } else if (r.cmd == QLatin1String("GetAbility")) {
                hc = api::parseAbility(r.value);
            }
        }
        auto row = [](const char *k, const QString &bcv, const QString &hv) {
            std::printf("  %-14s bc=%-22s http=%-22s %s\n", k, qPrintable(bcv), qPrintable(hv),
                        bcv == hv ? "" : "<-- differs");
        };
        auto yn = [](bool v) { return v ? QStringLiteral("yes") : QStringLiteral("no"); };
        auto sz = [](QSize z) { return QStringLiteral("%1x%2").arg(z.width()).arg(z.height()); };
        row("name", d.name, hName);
        row("model", d.model, hModel);
        row("admin", yn(d.isAdmin), yn(hc.isAdmin));
        const bcdev::Channel c0 = d.channels.value(0);
        const api::ChannelCaps h0 = hc.channels.value(0);
        row("main", sz(c0.mainSize), sz(hMain));
        row("sub", sz(c0.subSize), sz(hSub));
#define CAP(f) row(#f, yn(c0.caps.f), yn(h0.f))
        CAP(ptz); CAP(panTilt); CAP(ptzPreset); CAP(ptzPatrol); CAP(ptzGuard); CAP(ptzSpeed);
        CAP(ptzCheck); CAP(autoTrack); CAP(autoFocus); CAP(zoomFocusPos); CAP(zoom); CAP(focus);
        CAP(ai); CAP(aiPeople); CAP(aiVehicle); CAP(aiDogCat); CAP(audio); CAP(talk); CAP(siren);
        CAP(floodlight); CAP(battery); CAP(doorbell); CAP(supportsBalanced);
#undef CAP
        std::printf("  bc only: type=%s nvr=%d channels=%d uid=%s firmware=%s\n", qPrintable(d.type),
                    d.isNvr, d.channelNum, qPrintable(d.uid), qPrintable(d.firmware));
    }

    if (cli.isSet("snapshot")) {
        t.restart();
        QString err;
        const QByteArray jpeg = bcdev::snapshot(*s, 0, cli.isSet("main"), &err);
        std::printf("snapshot: %d bytes in %lld ms %s\n", int(jpeg.size()), t.elapsed(), qPrintable(err));
        QFile f(cli.value("snapshot"));
        if (!jpeg.isEmpty() && f.open(QIODevice::WriteOnly))
            f.write(jpeg);
    }
    if (cli.isSet("search-day")) {
        const QDate date = QDate::fromString(cli.value("search-day"), Qt::ISODate);
        const bool main = cli.isSet("main");
        QByteArray uidXml;
        quint16 st = 0;
        s->requestXml(114, 250, {}, {}, &uidXml, &st);
        const int a = uidXml.indexOf("<uid>"), z = uidXml.indexOf("</uid>");
        const QString uid = a >= 0 && z > a ? QString::fromUtf8(uidXml.mid(a + 5, z - a - 5)) : QString();
        t.restart();
        QVector<bcrec::Recording> files;
        const bool ok = bcrec::searchDay(*s, 0, uid, date, main, &files);
        std::printf("search %s %s: %s %s, %d files in %lld ms\n", qPrintable(date.toString(Qt::ISODate)),
                    main ? "main" : "sub", ok ? "ok" : "FAILED", qPrintable(s->error()), int(files.size()), t.elapsed());
        for (int i = 0; i < files.size() && i < 3; ++i)
            std::printf("  %s - %s triggers %d %s\n", qPrintable(files[i].start.toString(Qt::ISODate)),
                        qPrintable(files[i].end.time().toString()), files[i].triggers,
                        qPrintable(files[i].name));
        t.restart();
        const QVector<int> days = bcrec::monthDays(*s, 0, uid, date.year(), date.month());
        QStringList dl;
        for (int d : days)
            dl << QString::number(d);
        std::printf("month days (%lld ms): %s\n", t.elapsed(), qPrintable(dl.join(u' ')));

        if (cli.isSet("play") && !files.isEmpty()) {
            BaichuanClient::Params v;
            v.host = cli.value("host");
            v.username = sp.username;
            v.password = password;
            v.uid = uid;
            v.mainStream = main;
            for (const auto &f : files)
                v.replayFiles.append({f.start.toSecsSinceEpoch(), f.end.toSecsSinceEpoch(), f.name, f.id});
            v.startEpoch = QDateTime(date, QTime::fromString(cli.value("play"))).toSecsSinceEpoch();
            BaichuanClient client(v);
            client.setRate(cli.value("rate").toDouble());
            client.start();
            const int secs = cli.value("seconds").toInt();
            QElapsedTimer w;
            w.start();
            qint64 bytes = 0, firstAt = -1;
            bool sought = false;
            QByteArray buf(1 << 20, Qt::Uninitialized);
            while (w.elapsed() < secs * 1000LL) {
                if (cli.isSet("seek") && !sought && w.elapsed() > secs * 500LL) {
                    sought = true;
                    const qint64 to = QDateTime(date, QTime::fromString(cli.value("seek"))).toSecsSinceEpoch();
                    std::printf("seek at %.1f s: %s\n", w.elapsed() / 1000.0, client.seek(to) ? "ok" : "refused");
                }
                const int n = client.read(reinterpret_cast<unsigned char *>(buf.data()), buf.size());
                if (n <= 0) {
                    std::printf("stream ended after %.1f s\n", w.elapsed() / 1000.0);
                    break;
                }
                if (firstAt < 0) {
                    firstAt = w.elapsed();
                    std::printf("first bytes after %lld ms (%s)\n", firstAt, qPrintable(client.ffmpegFormat()));
                }
                bytes += n;
            }
            client.stop();
            std::printf("play %s x%s: %.2f MB in %.1f s = %.2f Mbit/s\n", main ? "main" : "sub",
                        qPrintable(cli.value("rate")), bytes / 1e6, w.elapsed() / 1000.0,
                        bytes * 8 / (w.elapsed() / 1000.0) / 1e6);
        }
        if (cli.isSet("replay") && !files.isEmpty()) {
            // Pick a file long enough to measure (>= 30 s), else the first.
            bcrec::Recording f = files.first();
            for (const auto &c : files)
                if (c.start.secsTo(c.end) >= 120) { f = c; break; }
            const int speed = cli.value("replay").toInt();
            const QByteArray stream = main ? "mainStream" : "subStream";
            const QByteArray play = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<FileInfoList version=\"1.1\">\n<FileInfo>\n<channelId>0</channelId>\n<Id>"
                + f.id.toUtf8() + "</Id>\n<uid>" + uid.toUtf8() + "</uid>\n<supportSub>" + QByteArray(main ? "0" : "1")
                + "</supportSub>\n" + (speed > 0 ? "<playSpeed>" + QByteArray::number(speed) + "</playSpeed>\n" : QByteArray())
                + "<streamType>" + stream + "</streamType>\n</FileInfo>\n</FileInfoList>\n</body>\n";
            BcMediaParser parser;
            std::atomic<qint64> bytes{0};
            quint32 firstUs = 0, lastUs = 0;
            int frames = 0;
            QMutex pm;
            parser.onVideo = [&](const BcMediaParser::VideoFrame &v) {
                if (!firstUs) firstUs = v.microseconds;
                lastUs = v.microseconds;
                ++frames;
            };
            const quint32 id = s->newMessId();
            const QByteArray k = key;
            s->route(id, [&](const BcMessage &m) {
                if (m.cmd != 5 || m.payloadOffset == 0)
                    return;
                QByteArray ext = bc::aesCfb(m.body.left(int(m.payloadOffset)), k, true);
                int encLen = 0;
                const int p = ext.indexOf("<encryptLen>");
                if (p >= 0) encLen = ext.mid(p + 12, ext.indexOf("</encryptLen>", p) - p - 12).toInt();
                const QByteArray payload = m.body.mid(int(m.payloadOffset));
                bytes += payload.size();
                QMutexLocker l(&pm);
                parser.append(encLen > 0 ? bc::aesCfb(payload.left(encLen), k, true) + payload.mid(encLen) : payload);
            });
            const QByteArray ext = BcSession::channelExtension(0);
            s->send(5, 1, id, bc::aesCfb(ext, k, false), bc::aesCfb(play, k, false));
            QElapsedTimer w;
            w.start();
            QThread::sleep(12);
            const double wall = w.elapsed() / 1000.0;
            s->unroute(id);
            const QByteArray stop = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<FileInfoList version=\"1.1\">\n<FileInfo>\n<uid>"
                + uid.toUtf8() + "</uid>\n<channelId>0</channelId>\n<name>" + f.name.toUtf8() + "</name>\n<streamType>"
                + stream + "</streamType>\n<Id>" + f.id.toUtf8() + "</Id>\n</FileInfo>\n</FileInfoList>\n</body>\n";
            s->send(7, 0, id, {}, bc::aesCfb(stop, k, false));
            QMutexLocker l(&pm);
            const double media = (lastUs - firstUs) / 1e6;
            std::printf("replay playSpeed=%d %s (%lld s file): %d frames, %.1f s of video in %.1f s "
                        "(x%.2f), %.2f Mbit/s\n", speed, stream.constData(), f.start.secsTo(f.end), frames,
                        media, wall, media / wall, bytes * 8 / wall / 1e6);
        }
    }

    const int listen = cli.value("listen").toInt();
    if (listen > 0)
        QThread::sleep(static_cast<unsigned long>(listen));
    s->removePushHandler(token);
    return 0;
}
