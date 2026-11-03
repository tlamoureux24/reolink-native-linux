#include "media/BcMediaParser.h"
#include "protocol/BcCrypto.h"
#include "protocol/BcDevice.h"
#include "protocol/BcRecordings.h"
#include "protocol/ReolinkApi.h"

#include "media/ImaAdpcm.h"
#include "protocol/BaichuanControl.h"

#include <QtTest>
#include <cmath>

static void putLE32(QByteArray &b, quint32 v)
{
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
    b.append(char((v >> 16) & 0xFF));
    b.append(char((v >> 24) & 0xFF));
}

using namespace rl;

class TestProtocol : public QObject
{
    Q_OBJECT

private slots:
    void bcRecordings()
    {
        // Trimmed cmd 142 / cmd 15 replies of an RLC-823A.
        const QByteArray days =
            "<DayRecords version=\"1.1\"><DayRecordList><DayRecord><index>0</index><channelId>0</channelId>"
            "<dayTypeList><dayType><index>0</index><type>normal</type></dayType>"
            "<dayType><index>1</index><type>normal</type></dayType></dayTypeList></DayRecord></DayRecordList></DayRecords>";
        QCOMPARE(bcrec::parseRecordDays(days), (QVector<int>{1, 2}));
        QVERIFY(bcrec::parseRecordDays("<DayRecords><dayTypeList /></DayRecords>").isEmpty());

        const QByteArray page =
            "<FileInfoList version=\"1.1\"><FileInfo><channelId>0</channelId><handle>3</handle>"
            "<name>0120261102072209</name><Id>/mnt/sda/Mp4Record/2026-11-02/RecS02_20261102_072209_072239_1_2.mp4</Id>"
            "<streamType>subStream</streamType><recordType>people</recordType>"
            "<startTime><year>2026</year><month>11</month><day>2</day><hour>7</hour><minute>22</minute><second>9</second></startTime>"
            "<endTime><year>2026</year><month>11</month><day>2</day><hour>7</hour><minute>22</minute><second>39</second></endTime>"
            "</FileInfo><FileInfo><name>x</name><Id>/y.mp4</Id><recordType>md, sched</recordType>"
            "<startTime><year>2026</year><month>11</month><day>2</day><hour>8</hour><minute>0</minute><second>0</second></startTime>"
            "<endTime><year>2026</year><month>11</month><day>2</day><hour>8</hour><minute>5</minute><second>0</second></endTime>"
            "</FileInfo></FileInfoList>";
        const QVector<bcrec::Recording> files = bcrec::parseFileList(page);
        QCOMPARE(files.size(), 2);
        QCOMPARE(files[0].name, QStringLiteral("0120261102072209"));
        QVERIFY(files[0].id.endsWith(QLatin1String("RecS02_20261102_072209_072239_1_2.mp4")));
        QCOMPARE(files[0].start, QDateTime(QDate(2026, 11, 2), QTime(7, 22, 9)));
        QCOMPARE(files[0].end, QDateTime(QDate(2026, 11, 2), QTime(7, 22, 39)));
        QCOMPARE(files[0].triggers, int(api::trigger::Person));
        QCOMPARE(files[1].triggers, int(api::trigger::Motion | api::trigger::Timer));
        QVERIFY(bcrec::parseFileList("<FileInfoList></FileInfoList>").isEmpty());
    }

    void bcAlarmEvents()
    {
        // A cmd 33 push: channel 0 sees motion and a person, channel 1 nothing.
        const QByteArray xml =
            "<body><AlarmEventList version=\"1.1\"><AlarmEvent version=\"1.1\"><channelId>0</channelId>"
            "<status>MD</status><AItype>people, dog_cat</AItype><recording>0</recording></AlarmEvent>"
            "<AlarmEvent version=\"1.1\"><channelId>1</channelId><status>none</status>"
            "<AItype>none</AItype></AlarmEvent></AlarmEventList></body>";
        const auto ev = bcdev::parseAlarmEvents(xml);
        QCOMPARE(ev.size(), 2);
        QCOMPARE(ev[0].first, 0);
        QVERIFY(ev[0].second.motion && ev[0].second.person && ev[0].second.pet && !ev[0].second.vehicle);
        QCOMPARE(ev[1].first, 1);
        QVERIFY(!ev[1].second.motion && !ev[1].second.person);
    }

    void bcDeviceSupport()
    {
        // cmd 199 <item> values of an RLC-823A (pan/tilt) and an RLC-811A
        // (zoom/focus only), as the cameras sent them.
        const QByteArray ptz =
            "<body><Support version=\"1.1\"><channelNum>1</channelNum><audioNum>1</audioNum>"
            "<audioTalk>1</audioTalk><smartHome><item><name>googleHome</name></item></smartHome>"
            "<item><chnID>0</chnID><ptzType>2</ptzType><noAudio>0</noAudio><autoFocus>1</autoFocus>"
            "<battery>0</battery><audioVersion>31</audioVersion><ledCtrl>38</ledCtrl>"
            "<ptzControl>229</ptzControl><ptzPreset>3</ptzPreset><ptzPatrol>1</ptzPatrol>"
            "<aitype>30647</aitype></item></Support></body>";
        const api::ChannelCaps p = bcdev::parseSupport(ptz, 1, false).value(0);
        QVERIFY(p.ptz && p.panTilt && p.ptzPreset && p.ptzPatrol && p.ptzGuard && p.ptzSpeed);
        QVERIFY(p.zoom && p.focus && p.zoomFocusPos && p.autoFocus && p.autoTrack);
        QVERIFY(p.ai && p.aiPeople && p.aiVehicle && p.aiDogCat);
        QVERIFY(p.audio && p.talk && p.siren && p.floodlight && !p.battery);

        const QByteArray cam =
            "<body><Support version=\"1.1\"><channelNum>1</channelNum><audioNum>1</audioNum>"
            "<audioTalk>1</audioTalk><item><chnID>0</chnID><ptzType>1</ptzType><noAudio>0</noAudio>"
            "<autoFocus>1</autoFocus><audioVersion>31</audioVersion><ledCtrl>38</ledCtrl>"
            "<ptzControl>1</ptzControl><ptzPreset>0</ptzPreset><aitype>1847</aitype></item></Support></body>";
        const api::ChannelCaps c = bcdev::parseSupport(cam, 1, false).value(0);
        QVERIFY(c.ptz && !c.panTilt && !c.ptzPreset && !c.ptzGuard && c.zoom && c.zoomFocusPos);
        QVERIFY(!c.autoTrack && c.aiPeople && c.floodlight);

        bcdev::Device d;
        bcdev::parseLoginInfo("<body><DeviceInfo><type>nvr</type><channelNum>8</channelNum></DeviceInfo></body>", d);
        QVERIFY(d.isNvr);
        QCOMPARE(d.channelNum, 8);
        QVERIFY(bcdev::parseIsAdmin("<AbilityInfo><system><subModule><abilityValue>general_rw, "
                                    "reboot_rw</abilityValue></subModule></system></AbilityInfo>"));
        QVERIFY(!bcdev::parseIsAdmin("<AbilityInfo><system><subModule><abilityValue>general_ro"
                                     "</abilityValue></subModule></system></AbilityInfo>"));
    }














    void jsonVariantRoundTrip()
    {
        const QByteArray body = R"({"Enc":{"channel":0,"audio":1,"mainStream":{"size":"2560*1440",
            "bitRate":4096,"frameRate":25,"vType":"h265"},"flags":[1,2,3]}})";
        const Json j = Json::parse(body.constData(), body.constData() + body.size(), nullptr, false);
        const QVariant v = api::toVariant(j);
        const QVariantMap enc = v.toMap().value("Enc").toMap();
        QCOMPARE(enc.value("audio").toInt(), 1);
        QCOMPARE(enc.value("mainStream").toMap().value("bitRate").toInt(), 4096);
        QCOMPARE(enc.value("mainStream").toMap().value("vType").toString(), QStringLiteral("h265"));
        QCOMPARE(enc.value("flags").toList().size(), 3);

        // Round-trip back to JSON preserves types.
        const Json back = api::toJson(v);
        QCOMPARE(back["Enc"]["mainStream"]["bitRate"].get<int>(), 4096);
        QCOMPARE(QString::fromStdString(back["Enc"]["mainStream"]["vType"].get<std::string>()),
                 QStringLiteral("h265"));
        QVERIFY(back["Enc"]["flags"].is_array());
    }

    void toJsonSendsIntegralDoublesAsIntegers()
    {
        // QML passes Math.round() results as double; firmware wants 207, not 207.0.
        const Json j = api::toJson(QVariantMap{{QStringLiteral("pos"), 207.0},
                                               {QStringLiteral("ratio"), 0.5}});
        QVERIFY(j["pos"].is_number_integer());
        QCOMPARE(j["pos"].get<int>(), 207);
        QVERIFY(j["ratio"].is_number_float());
        QCOMPARE(j.dump(), std::string(R"({"pos":207,"ratio":0.5})"));
    }







    void imaAdpcmRoundTrip()
    {
        // 440 Hz at 16 kHz, half scale: encode then decode with the start state.
        QVector<qint16> pcm(1024);
        for (int i = 0; i < pcm.size(); ++i)
            pcm[i] = static_cast<qint16>(16000 * std::sin(2 * M_PI * 440 * i / 16000.0));
        ImaAdpcm enc;
        const QByteArray data = enc.encode(pcm.constData(), static_cast<int>(pcm.size()));
        QCOMPARE(data.size(), 512);
        const QVector<qint16> back = ImaAdpcm::decode(data, 0, 0);
        QCOMPARE(back.size(), pcm.size());
        double sig = 0, err = 0;
        for (int i = 64; i < pcm.size(); ++i) { // skip the adaptation ramp
            sig += double(pcm[i]) * pcm[i];
            err += double(pcm[i] - back[i]) * (pcm[i] - back[i]);
        }
        QVERIFY2(10 * std::log10(sig / err) > 20, "ADPCM round trip below 20 dB SNR");
    }

    void imaAdpcmDviNibbleOrder()
    {
        // First sample positive and large -> code 7 (0111) in the HIGH nibble;
        // second sample back to 0 -> a negative code (bit 3 set) in the low one.
        ImaAdpcm enc;
        const qint16 two[2] = {32000, 0};
        const QByteArray b = enc.encode(two, 2);
        QCOMPARE(b.size(), 1);
        QCOMPARE(quint8(b[0]) >> 4, 7);
        QVERIFY((quint8(b[0]) & 0x08) != 0);
    }

    void talkFrameLayout()
    {
        const QByteArray adpcm(512, '\x5a');
        const QByteArray f = BaichuanControl::talkFrame(adpcm, -2, 7);
        QCOMPARE(f.size(), 16 + 512);
        QCOMPARE(f.left(4), QByteArray("01wb"));
        QCOMPARE(quint8(f[4]) | (quint8(f[5]) << 8), 520); // size: data + 8
        QCOMPARE(quint8(f[6]) | (quint8(f[7]) << 8), 520); // repeated
        QCOMPARE(quint8(f[8]), 0x00);
        QCOMPARE(quint8(f[9]), 0x01);
        QCOMPARE(quint8(f[10]), (512 / 2 + 2) & 0xFF);       // words, truncated
        QCOMPARE(quint8(f[11]), 0x00);
        QCOMPARE(qint16(quint8(f[12]) | (quint8(f[13]) << 8)), qint16(-2)); // predictor
        QCOMPARE(quint8(f[14]) | (quint8(f[15]) << 8), 7);                  // index
        QCOMPARE(f.mid(16), adpcm);
    }

    void talkAbilityAndConfig()
    {
        // Real reply from an RLC-823A / RLC-811A / Duo 2 PoE (cmd 10).
        const QByteArray ability = R"(<?xml version="1.0" encoding="UTF-8" ?>
<body>
<TalkAbility version="1.1">
<duplexList><duplex>FDX</duplex></duplexList>
<audioStreamModeList><audioStreamMode>followVideoStream</audioStreamMode></audioStreamModeList>
<audioConfigList><audioConfig><priority>0</priority><audioType>adpcm</audioType>
<sampleRate>16000</sampleRate><samplePrecision>16</samplePrecision>
<lengthPerEncoder>1024</lengthPerEncoder><soundTrack>mono</soundTrack></audioConfig></audioConfigList>
</TalkAbility>
</body>)";
        bool ok = false;
        const BaichuanControl::TalkParams p = BaichuanControl::parseTalkAbility(ability, &ok);
        QVERIFY(ok);
        QCOMPARE(p.duplex, QStringLiteral("FDX"));
        QCOMPARE(p.audioStreamMode, QStringLiteral("followVideoStream"));
        QCOMPARE(p.sampleRate, 16000);
        QCOMPARE(p.lengthPerEncoder, 1024);
        const QByteArray cfg = BaichuanControl::talkConfigXml(0, p);
        QVERIFY(cfg.contains("<TalkConfig version=\"1.1\">"));
        QVERIFY(cfg.contains("<channelId>0</channelId>"));
        QVERIFY(cfg.contains("<duplex>FDX</duplex>"));
        QVERIFY(cfg.contains("<audioType>adpcm</audioType>"));
        QVERIFY(cfg.contains("<lengthPerEncoder>1024</lengthPerEncoder>"));
        QVERIFY(cfg.contains("<soundTrack>mono</soundTrack>"));
        bool none = true;
        BaichuanControl::parseTalkAbility("<body/>", &none);
        QVERIFY(!none);
    }


    void baichuanCrypto()
    {
        // Modern-login hash: uppercase-hex MD5(value+nonce), first 31 chars.
        // Known-answer vector (admin + a sample nonce).
        QCOMPARE(bc::modernHash(QStringLiteral("admin"), QStringLiteral("9E6D1FCB9E69846D")),
                 QByteArray("9F07915E819A076E2E14169830769D6"));
        QCOMPARE(bc::modernHash(QStringLiteral("admin"), QStringLiteral("9E6D1FCB9E69846D")).size(),
                 31);

        // BCEncrypt XOR: encrypt == decrypt (round-trips), for several offsets.
        const QByteArray plain = QByteArrayLiteral("<?xml version=\"1.0\"?><body/>");
        for (quint8 off : {quint8(0), quint8(1), quint8(7), quint8(255)}) {
            const QByteArray enc = bc::xorCrypt(plain, off);
            QVERIFY(enc != plain);
            QCOMPARE(bc::xorCrypt(enc, off), plain); // involution
        }
        // Offset 0 is a plain repeating 8-byte key XOR: first byte 'A'(0x41)^0x1F.
        QCOMPARE(static_cast<quint8>(bc::xorCrypt(QByteArray("A"), 0).at(0)),
                 quint8(0x41 ^ 0x1F));

        // AES key derivation (nonce-first, dash separator, 16 ASCII chars).
        const QByteArray key = bc::aesKey(QStringLiteral("9E6D1FCB9E69846D"),
                                          QStringLiteral("123456"));
        QCOMPARE(key, QByteArray("0D119EB629684BDF"));

        // AES-128-CFB against an openssl-generated known-answer vector.
        const QByteArray pt = QByteArrayLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" ?><body><Preview/></body>");
        const QByteArray ct = QByteArray::fromHex(
            "c640e9ad98c8769f3cdb1a55cfd4985710bee2e828f6d0d6cd7b76bc8c7cc3fe"
            "19a62165915a810dd1f9842104dbe1b4147050eee074ee5eb1228d9cd8e9");
        QCOMPARE(bc::aesCfb(pt, key, /*decrypt=*/false), ct);       // encrypt
        QCOMPARE(bc::aesCfb(ct, key, /*decrypt=*/true), pt);        // decrypt round-trips
    }

    void bcMediaParser()
    {
        // Info V1 (32 bytes): sets declared size. Then an H.264 I-frame carrying a
        // 5-byte Annex-B payload. Frame spans the two appends to exercise buffering.
        QByteArray info;
        info.append("1001");            // magic
        putLE32(info, 32);              // header_size
        putLE32(info, 1536);            // width
        putLE32(info, 432);             // height
        info.append(QByteArray(16, 0)); // rest of the 32-byte header

        const QByteArray nal = QByteArrayLiteral("\x00\x00\x00\x01\x65"); // 5 bytes
        QByteArray iframe;
        iframe.append("00dc");          // I-frame magic (channel 0)
        iframe.append("H264");          // video_type
        putLE32(iframe, nal.size());    // payload_size = 5
        putLE32(iframe, 0);             // additional_header_size = 0 (NAL at 24)
        putLE32(iframe, 12345);         // microseconds
        putLE32(iframe, 0);             // unknown_b  -> base header = 24 bytes
        iframe.append(nal);             // payload
        iframe.append(QByteArray(3, 0)); // pad to 8: (8 - 5%8)%8 = 3

        rl::BcMediaParser mp;
        QList<rl::BcMediaParser::VideoFrame> frames;
        mp.onVideo = [&](const rl::BcMediaParser::VideoFrame &f) { frames.append(f); };

        mp.append(info);
        QCOMPARE(mp.declaredWidth(), 1536);
        QCOMPARE(mp.declaredHeight(), 432);
        QCOMPARE(frames.size(), 0);

        // Split the I-frame across two appends; no frame until it's complete.
        mp.append(iframe.left(10));
        QCOMPARE(frames.size(), 0);
        mp.append(iframe.mid(10));
        QCOMPARE(frames.size(), 1);
        QVERIFY(frames[0].keyFrame);
        QCOMPARE(int(frames[0].codec), int(rl::BcMediaParser::Codec::H264));
        QCOMPARE(frames[0].microseconds, quint32(12345));
        QCOMPARE(frames[0].annexB, nal);
    }
};

QTEST_GUILESS_MAIN(TestProtocol)
#include "test_protocol.moc"
