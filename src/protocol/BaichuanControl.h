#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantMap>

#include <cstdint>
#include <memory>

namespace rl { class BcSession; }

// Synchronous Baichuan settings/config requests inside a device's session
// (BcSession), as the official client sends every command: get a command's
// XML, read-modify-write its fields, and two-way talk.
//
// Blocking request/response, for a worker thread.
//
// Clean-room: wire format (24-byte class 0x6414 header, ch_id=channel+1,
// AES-CFB segments, read-modify-write SET) from the documented protocol + MIT
// reolink_aio. No AGPL neolink code.
namespace rl {

class BaichuanControl
{
public:
    struct Params {
        QString host;
        int port = 9000;
        QString username;
        QString password;
    };

    explicit BaichuanControl(Params params);
    // Inside the device's session (BcSession), as the official client sends
    // every command: no connection of its own.
    explicit BaichuanControl(std::shared_ptr<BcSession> session);
    ~BaichuanControl();

    // Wait for the session to be logged in; false if it is not.
    bool open();
    void close();

    // GET a command's config XML for a channel (channel < 0 => host-level).
    // Returns the decrypted reply XML (may span the device's own root element),
    // or empty on failure. `statusOut` (optional) receives the BC status code.
    QByteArray get(quint32 cmdId, int channel, quint16 *statusOut = nullptr);

    // Read the recursive <enable> flag from a GET response (-1 if absent/failed).
    int readEnable(quint32 getCmdId, int channel);

    // Read-modify-write the <enable> flag: GET getCmdId, flip <enable>, SET setCmdId.
    // Returns true on a success status (200/201/300).
    bool writeEnable(quint32 getCmdId, quint32 setCmdId, int channel, bool enable);

    // Parse a command's config XML into a flat { leafTag: value } map (first
    // occurrence of each tag wins — fine because a device config's scalar tags
    // are unique). reqBody is an optional request XML some GETs need (e.g. AI
    // detection selects an ai_type in an AiDetectCfg body).
    QVariantMap getConfigFlat(quint32 cmdId, int channel, const QByteArray &reqBody = {});

    // Read-modify-write several leaf tags in one config: GET, replace each
    // <tag>…</tag> (first occurrence) with its new value, SET. Returns true on a
    // success status. getBody is the optional GET request body (see above).
    bool writeFields(quint32 getCmdId, quint32 setCmdId, int channel,
                     const QVariantMap &changes, const QByteArray &getBody = {});

    // Low-level: send a raw (plaintext) XML body to cmdId and return the decrypted
    // reply. bodyXml empty => header-only GET. Used by the helpers above.
    QByteArray transact(quint32 cmdId, int channel, const QByteArray &bodyXml,
                        quint16 *statusOut = nullptr);

    bool isOpen() const;

    // ---- Two-way talk ----------------------------------------------------
    // Wire facts (determined from the official Windows client's SDK for
    // interoperability; written from scratch): cmd 10 returns <TalkAbility>,
    // cmd 201 opens a talk session with a <TalkConfig>, cmd 202 carries audio
    // (an <Extension> with <binaryData>1</binaryData>, then one "01wb" IMA
    // ADPCM frame per message, sent without waiting for a reply), cmd 11 ends it.
    struct TalkParams {
        QString duplex = QStringLiteral("FDX");
        QString audioStreamMode = QStringLiteral("followVideoStream");
        QString audioType = QStringLiteral("adpcm");
        int sampleRate = 16000;
        int samplePrecision = 16;
        int lengthPerEncoder = 1024; // samples per frame
        QString soundTrack = QStringLiteral("mono");
    };
    // First duplex / stream mode / audio config the camera lists in cmd 10.
    static TalkParams parseTalkAbility(const QByteArray &xml, bool *ok = nullptr);
    static QByteArray talkConfigXml(int channel, const TalkParams &p);
    // One "01wb" frame: magic, size twice, a 00 01 LL 00 block header, the
    // encoder's start state (predictor, index), then the ADPCM bytes.
    static QByteArray talkFrame(const QByteArray &adpcm, qint16 predictor, quint8 index);

    bool talkStart(int channel, const TalkParams &p, quint16 *statusOut = nullptr);
    bool talkSend(int channel, const QByteArray &frame); // fire-and-forget
    void talkStop(int channel);

private:
    std::shared_ptr<BcSession> m_session;
    QByteArray m_aesKey;
    quint32 m_messId = 0;     // id of the last request
    quint32 m_talkMessId = 0; // mess_id of the cmd 201 that opened the talk session
};

} // namespace rl
