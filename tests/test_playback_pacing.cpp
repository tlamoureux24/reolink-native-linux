// Playback controls on file sources: speed factor, pause and single-frame step.
//
// A 4 s, 25 fps clip is generated with the ffmpeg CLI (skipped when it isn't
// installed). Frames are counted through framesDecoded, which needs no sink.
#include "media/StreamPlayer.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using rl::StreamPlayer;

class TestPlaybackPacing : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_clip;      // video only
    QString m_audioClip; // video + AAC
    static constexpr int kFrames = 100; // 4 s at 25 fps

private slots:
    void initTestCase()
    {
        const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty())
            QSKIP("ffmpeg CLI not available to generate the test clip");
        QVERIFY(m_dir.isValid());
        m_clip = m_dir.filePath(QStringLiteral("clip.mp4"));
        QProcess p;
        p.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"),
                         QStringLiteral("-f"), QStringLiteral("lavfi"),
                         QStringLiteral("-i"), QStringLiteral("testsrc=size=160x90:rate=25"),
                         QStringLiteral("-frames:v"), QString::number(kFrames),
                         QStringLiteral("-c:v"), QStringLiteral("libx264"),
                         QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), m_clip});
        QVERIFY(p.waitForFinished(30000));
        QCOMPARE(p.exitCode(), 0);

        m_audioClip = m_dir.filePath(QStringLiteral("audio.mp4"));
        p.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"),
                         QStringLiteral("-f"), QStringLiteral("lavfi"),
                         QStringLiteral("-i"), QStringLiteral("testsrc=size=160x90:rate=25"),
                         QStringLiteral("-f"), QStringLiteral("lavfi"),
                         QStringLiteral("-i"), QStringLiteral("sine=frequency=440:sample_rate=16000"),
                         QStringLiteral("-t"), QStringLiteral("2"),
                         QStringLiteral("-c:v"), QStringLiteral("libx264"),
                         QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                         QStringLiteral("-c:a"), QStringLiteral("aac"), m_audioClip});
        QVERIFY(p.waitForFinished(30000));
        QCOMPARE(p.exitCode(), 0);
    }

    void detectsAudioTrack()
    {
        StreamPlayer withAudio;
        withAudio.setSource(m_audioClip);
        withAudio.setAudioEnabled(true); // decodes + resamples; no device needed
        withAudio.start();
        QTRY_VERIFY_WITH_TIMEOUT(withAudio.hasAudio(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(withAudio.framesDecoded() >= 10, 10000);
        withAudio.stop();

        StreamPlayer silent;
        silent.setSource(m_clip);
        silent.start();
        QTRY_VERIFY_WITH_TIMEOUT(silent.framesDecoded() >= 5, 10000);
        QVERIFY(!silent.hasAudio());
        silent.stop();
    }

    void fourTimesSpeedPlaysFaster()
    {
        StreamPlayer player;
        player.setSource(m_clip);
        player.setPlaybackRate(4.0);
        QElapsedTimer t;
        t.start();
        player.start();
        QTRY_VERIFY_WITH_TIMEOUT(player.framesDecoded() >= kFrames - 1, 10000);
        // Real time would take 4 s; at 4x about 1 s.
        QVERIFY2(t.elapsed() < 2500, qPrintable(QStringLiteral("took %1 ms").arg(t.elapsed())));
        player.stop();
    }

    void rateIsClamped()
    {
        StreamPlayer player;
        player.setPlaybackRate(100.0);
        QCOMPARE(player.playbackRate(), 16.0);
        player.setPlaybackRate(0.0);
        QCOMPARE(player.playbackRate(), 0.125);
    }

    void pauseHoldsAndStepAdvancesOneFrame()
    {
        StreamPlayer player;
        player.setSource(m_clip);
        player.start();
        QTRY_VERIFY_WITH_TIMEOUT(player.framesDecoded() >= 5, 10000);

        player.setPaused(true);
        QTest::qWait(200); // let the frame already in flight land
        const qint64 held = player.framesDecoded();
        QTest::qWait(600);
        QCOMPARE(player.framesDecoded(), held);

        player.stepFrame();
        QTRY_COMPARE_WITH_TIMEOUT(player.framesDecoded(), held + 1, 2000);
        QTest::qWait(300);
        QCOMPARE(player.framesDecoded(), held + 1);

        player.setPaused(false);
        QTRY_VERIFY_WITH_TIMEOUT(player.framesDecoded() > held + 3, 3000);
        player.stop();
    }

    void startClearsPause()
    {
        StreamPlayer player;
        player.setSource(m_clip);
        player.setPaused(true);
        player.start();
        QVERIFY(!player.paused());
        QTRY_VERIFY_WITH_TIMEOUT(player.framesDecoded() >= 3, 10000);
        player.stop();
    }
};

QTEST_MAIN(TestPlaybackPacing)
#include "test_playback_pacing.moc"
