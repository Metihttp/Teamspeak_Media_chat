// 2.5 zoom (design2.md 1, 4.1 Z-T1..Z-T12): previews follow the chat's text size, Windows scaling and the Preview size
// setting. The scale formula and its vectors, the Preview size setting, layouts in du at every scale (sizes x s, the
// same line breaks), hit tests at s 1.5 and 2, never wider than the column, the coordinator (one pass per gesture, one
// edit block, the reader's place, no full layout during a gesture), the picture budget, the GIF frame rule, the
// fonts at 11 du and the source rules. Offscreen, no network, no TeamSpeak.

#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontMetricsF>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QtTest>

#include "albums.h"
#include "audiocard.h"
#include "chatscale.h"
#include "chatscroll.h"
#include "linkpreview/linkcard.h"
#include "linkpreview/linkdoc.h"
#include "linkpreview/linkurl.h"
#include "pollart.h"
#include "previewpaint.h"
#include "previewrenderer.h"
#include "previewscale.h"
#include "reactionart.h"
#include "replyart.h"
#include "settings.h"
#include "testmain.h"
#include "tschat.h"
#include "voicecard.h"
#include "linkpreview/previewmaker.h"

namespace {

// (chatPx, preset, columnL, docL, bubbleL) -> s20, design2 1.2.
int s20Of(int chatPx, int preset, qreal columnL, qreal docL = 4, int bubbleL = 0)
{
    return previewscale::computeScale(chatPx, preset, columnL, docL, bubbleL, 1.0).s20;
}

PreviewScale scaleAt(int s20, qreal dpr = 1.0)
{
    PreviewScale s;
    s.s20 = s20;
    s.c20 = s20;
    s.dpr = dpr;
    return s;
}

PreviewStyle styleAt(const PreviewScale& scale, int maxWidth = 400, int maxHeight = 300)
{
    PreviewStyle style;
    style.font      = QFont(QStringLiteral("Segoe UI"));
    style.font.setPointSizeF(8.25);
    style.dpr       = scale.ratio();
    style.maxWidth  = maxWidth;
    style.maxHeight = maxHeight;
    style.animate   = false;
    return style;
}

MediaEntry pictureEntry(int w, int h, MediaKind kind = MediaKind::Image)
{
    MediaEntry e;
    e.kind          = kind;
    e.state         = MediaState::Ready;
    e.link.fileName = QStringLiteral("picture.png");
    e.link.width  = w;
    e.link.height = h;
    e.link.size   = 123456;
    return e;
}

// The chat's text size as the document's font asks for it (96 dpi): the offscreen platform has no font files, so
// QFontInfo doesn't follow the size there (previewscale::chatPxOf does in TeamSpeak).
int chatPxOf(QTextBrowser& b)
{
    const QFont f = b.document()->defaultFont();
    return f.pixelSize() > 0 ? f.pixelSize() : qRound(f.pointSizeF() * 4.0 / 3.0);
}

QString source(const char* relative)
{
    QFile f(QStringLiteral(TSMEDIA_SOURCE_DIR "/") + QString::fromLatin1(relative));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

// A chat filled like TeamSpeak's, with picture objects of ours (du sizes at the scale) between the lines.
struct Chat {
    QTextBrowser browser;
    QStringList  objects;

    explicit Chat(int lines, int objectsEvery = 5)
    {
        browser.resize(520, 400);
        browser.setFont(QFont(QStringLiteral("Segoe UI"), 8));
        QTextDocument* doc = browser.document();
        // TeamSpeak's message icons are resources of its chat (else QTextBrowser looks for files on every layout).
        QPixmap icon(13, 13);
        icon.fill(Qt::gray);
        for (const char* kind : {"MESSAGE_INCOMING", "MESSAGE_OUTGOING", "MESSAGE_INFO"})
            doc->addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("iconpath:%1?size=13x13").arg(QString::fromLatin1(kind))), icon);
        const tschat::Skin s = tschat::darkSkin();
        for (int i = 0; i < lines; ++i) {
            tschat::append(doc, tschat::messageHtml(s, QStringLiteral("16:00:%1").arg(i % 60, 2, 10, QLatin1Char('0')), 7,
                                                    QString::fromLatin1(tschat::kKaiUid), QStringLiteral("Kai"),
                                                    QStringLiteral("line %1 with some words to wrap a little in a narrow chat").arg(i)));
            if (objectsEvery > 0 && i % objectsEvery == 0) {
                QTextCursor c(doc);
                c.movePosition(QTextCursor::End);
                const QString name = QStringLiteral("tsmedia:obj%1").arg(i);
                QTextImageFormat f;
                f.setName(name);
                previewscale::setObjectSize(f, QSizeF(300, 200), PreviewScale());
                c.insertText(QString(QChar::LineSeparator));
                c.insertImage(f);
                QImage img(300, 200, QImage::Format_ARGB32_Premultiplied);
                img.fill(Qt::darkCyan);
                doc->addResource(QTextDocument::ImageResource, QUrl(name), QPixmap::fromImage(img));
                objects << name;
            }
        }
        browser.show();
        doc->documentLayout()->documentSize();
        QCoreApplication::processEvents();
    }

    // Every object of ours at scale (what a module's applyScale does), no drawing.
    void apply(const PreviewScale& scale)
    {
        QTextDocument* doc = browser.document();
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                const QTextFragment f = it.fragment();
                if (!f.isValid() || !f.charFormat().isImageFormat() || !f.charFormat().toImageFormat().name().startsWith(QLatin1String("tsmedia:")))
                    continue;
                QTextImageFormat format = f.charFormat().toImageFormat();
                previewscale::setObjectSize(format, previewscale::duSizeOf(format), scale);
                QTextCursor c(doc);
                c.setPosition(f.position());
                c.setPosition(f.position() + 1, QTextCursor::KeepAnchor);
                c.setCharFormat(format);
            }
        }
    }

    // The top of block n in the viewport.
    qreal blockTop(int n)
    {
        QTextDocument* doc = browser.document();
        return doc->documentLayout()->blockBoundingRect(doc->findBlockByNumber(n)).top() - browser.verticalScrollBar()->value();
    }
};

} // namespace

class TestPreviewScale : public QObject
{
    Q_OBJECT

  private slots:
    // ---- Z-T1: the formula ----------------------------------------------------------------------------------------
    void scaleVectors_data()
    {
        QTest::addColumn<int>("chatPx");
        QTest::addColumn<int>("preset");
        QTest::addColumn<qreal>("column");
        QTest::addColumn<qreal>("docL");
        QTest::addColumn<int>("bubbleL");
        QTest::addColumn<int>("s20");
        const auto row = [](const char* name, int px, int preset, qreal column, qreal doc, int bubble, int s20) {
            QTest::newRow(name) << px << preset << column << doc << bubble << s20;
        };
        row("11/100/800", 11, 100, 800, 4, 0, 20);
        row("14/100/800", 14, 100, 800, 4, 0, 25);
        row("16/100/800", 16, 100, 800, 4, 0, 29);
        row("17/100/800", 17, 100, 800, 4, 0, 30);
        row("19/100/800", 19, 100, 800, 4, 0, 34);
        row("22/100/800", 22, 100, 800, 4, 0, 40);
        row("22/150/800", 22, 150, 800, 4, 0, 60);
        row("33/150/800", 33, 150, 800, 4, 0, 60);
        row("8/100/800", 8, 100, 800, 4, 0, 15);
        row("11/85/800", 11, 85, 800, 4, 0, 17);
        row("22/150/600", 22, 150, 600, 4, 0, 56);
        row("22/100/330", 22, 100, 330, 4, 0, 30);
        row("22/100/300", 22, 100, 300, 4, 0, 28);
        row("11/100/100", 11, 100, 100, 4, 0, 10);
        row("11/100/60", 11, 100, 60, 4, 0, 10);
        row("22/100/330 doc0", 22, 100, 330, 0, 0, 31);
        row("bubble 11/100/800", 11, 100, 800, 4, 17, 20);
        row("bubble 22/100/330", 22, 100, 330, 4, 33, 30);
        row("bubble 33/100/216", 33, 100, 216, 4, 50, 19);
    }
    void scaleVectors()
    {
        QFETCH(int, chatPx);
        QFETCH(int, preset);
        QFETCH(qreal, column);
        QFETCH(qreal, docL);
        QFETCH(int, bubbleL);
        QFETCH(int, s20);
        QCOMPARE(s20Of(chatPx, preset, column, docL, bubbleL), s20);
    }

    void chromeAndRatio()
    {
        // c follows the chat's text, never the preset.
        const PreviewScale big = previewscale::computeScale(22, 150, 800, 4, 0, 1.0);
        QCOMPARE(big.s20, 60);
        QCOMPARE(big.c20, 40);
        QCOMPARE(big.s(), 3.0);
        QCOMPARE(big.c(), 2.0);
        // R = min(dpr x s, cap): dpr 1 x 2.0 = 2; dpr 2 x 3.0 = 4 on x64, 3 on x86.
        PreviewScale two = scaleAt(40, 1.0);
        QCOMPARE(two.ratio(), 2.0);
        PreviewScale six = scaleAt(60, 2.0);
        QCOMPARE(six.ratio(), sizeof(void*) == 8 ? 4.0 : 3.0);
        six.maxRatio = 3.0; // the x86 value
        QCOMPARE(six.ratio(), 3.0);
        QCOMPARE(previewscale::kMaxRenderRatio, sizeof(void*) == 8 ? 4.0 : 3.0);
        // s 1 at the default font reproduces 2.4 exactly.
        const PreviewScale one = previewscale::computeScale(11, 100, 800, 4, 0, 1.0);
        QCOMPARE(one.s20, 20);
        QCOMPARE(one.ratio(), 1.0);
        QCOMPARE(one.toLogical(QSizeF(400, 300)), QSizeF(400, 300));
        // Fractional at dpr 1.25: whole device px.
        const PreviewScale frac = scaleAt(29, 1.25); // s 1.45
        const qreal        w    = frac.toLogical(333.0);
        QCOMPARE(qRound(w * 1.25) / 1.25, w);
        QCOMPARE(frac.duFromLogical(frac.toLogical(333.0)), 333);
        // The same scale: s20, c20 and R.
        QVERIFY(scaleAt(30).sameAs(scaleAt(30)));
        QVERIFY(!scaleAt(30).sameAs(scaleAt(31)));
        QVERIFY(!scaleAt(30, 1.0).sameAs(scaleAt(30, 2.0)));
        // Never drawn below ratio 1 (QPainter takes a device's ratio as at least 1): Small at 100% draws at 1 and the
        // picture is made smaller, smoothly, for the document.
        const PreviewScale small = previewscale::computeScale(11, 85, 800, 4, 0, 1.0);
        QCOMPARE(small.s20, 17);
        QCOMPARE(small.ratio(), 1.0);
        QCOMPARE(small.displayRatio(), 0.85);
        QImage drawn(400, 100, QImage::Format_ARGB32_Premultiplied);
        drawn.fill(Qt::red);
        const QImage shown = previewscale::forDocument(drawn, small.displayRatio());
        QCOMPARE(shown.size(), QSize(340, 85));
        QCOMPARE(shown.devicePixelRatio(), 0.85);
        drawn.setDevicePixelRatio(2.0);
        QCOMPARE(previewscale::forDocument(drawn, 2.0).size(), drawn.size()); // as it is when the screen shows it 1:1
        // The chrome scale as a scale of its own.
        QCOMPARE(big.chrome().s20, 40);
        QCOMPARE(big.chrome().ratio(), 2.0);
    }

    // ---- Z-T2: the setting ----------------------------------------------------------------------------------------
    void previewSizeSetting()
    {
        QCOMPARE(Settings().previewScale, 100);
        QCOMPARE(previewscale::nearestPreset(85), 85);
        QCOMPARE(previewscale::nearestPreset(100), 100);
        QCOMPARE(previewscale::nearestPreset(125), 125);
        QCOMPARE(previewscale::nearestPreset(150), 150);
        QCOMPARE(previewscale::nearestPreset(0), 85);
        QCOMPARE(previewscale::nearestPreset(92), 85);   // 7 from 85, 8 from 100
        QCOMPARE(previewscale::nearestPreset(93), 100);
        QCOMPARE(previewscale::nearestPreset(112), 100); // a tie (12.5 is not an integer); 112 is nearer 100
        QCOMPARE(previewscale::nearestPreset(1000), 150);
        QCOMPARE(previewscale::nearestPreset(-5), 85);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("settings.ini"));
        Settings      s;
        s.previewScale = 125;
        s.save(file);
        Settings loaded;
        loaded.load(file);
        QCOMPARE(loaded.previewScale, 125);
        const auto readBack = [&](const QVariant& raw) {
            {
                QSettings ini(file, QSettings::IniFormat);
                ini.setValue(QStringLiteral("previewScale"), raw);
            }
            Settings l;
            l.load(file);
            return l.previewScale;
        };
        QCOMPARE(readBack(150), 150);
        QCOMPARE(readBack(140), 150);
        QCOMPARE(readBack(110), 100);
        QCOMPARE(readBack(QStringLiteral("garbage")), 100);
        QCOMPARE(readBack(90), 85); // 5 from 85, 10 from 100
        // Restore defaults: a fresh Settings.
        QCOMPARE(Settings().previewScale, 100);
    }

    // Live test (2.5 zoom): picking another Preview size alone left Apply greyed out, and OK applies only a changed
    // form, so the new size was lost. Every combo box of the dialog marks the form changed (SettingsDialog isn't part
    // of the test build: a source rule).
    void settingsCombosMarkTheFormChanged()
    {
        const QString dialog = source("src/settingsdialog.cpp");
        QVERIFY(!dialog.isEmpty());
        const QRegularExpression made(QString::fromLatin1(R"((m_\w+)\s*=\s*new QComboBox\()"));
        int combos = 0;
        for (auto it = made.globalMatch(dialog); it.hasNext();) {
            const QString member = it.next().captured(1);
            ++combos;
            const QRegularExpression wired(QString::fromLatin1(R"(connect\()") + member
                                           + QString::fromLatin1(R"(,\s*QOverload<int>::of\(&QComboBox::currentIndexChanged\),\s*this,\s*changed\))"));
            QVERIFY2(wired.match(dialog).hasMatch(), qPrintable(member + QStringLiteral(" doesn't mark the form changed")));
        }
        QVERIFY(combos >= 1); // Preview size
    }

    // Live test (2.5 zoom, 4K at 200%): hinted advances are whole du pixels, R doubled their rounding and 9-10 du card
    // text stood unevenly ("A nony mous", "1:0 2"). Above R 1 the chat font is laid out unhinted; at 1 as in 2.4.
    void fontForRatioHinting()
    {
        QFont chat(QStringLiteral("MS Shell Dlg 2"));
        chat.setPointSizeF(8.25);
        chat.setBold(true);
        QCOMPARE(previewpaint::fontForRatio(chat, 1.0).hintingPreference(), QFont::PreferDefaultHinting);
        QCOMPARE(previewpaint::fontForRatio(chat, 2.0).hintingPreference(), QFont::PreferNoHinting);
        QCOMPARE(previewpaint::fontForRatio(chat, 1.35).hintingPreference(), QFont::PreferNoHinting);
        QCOMPARE(previewpaint::fontForRatio(chat, 4.0).hintingPreference(), QFont::PreferNoHinting);
        // back at 1 (zoomed out again): TeamSpeak's own hinting
        QCOMPARE(previewpaint::fontForRatio(previewpaint::fontForRatio(chat, 2.0), 1.0).hintingPreference(), QFont::PreferDefaultHinting);
        // nothing else of the chat font changes; the art modules' fonts keep the preference
        const QFont two = previewpaint::fontForRatio(chat, 2.0);
        QCOMPARE(two.family(), chat.family());
        QCOMPARE(two.bold(), chat.bold());
        PreviewStyle style;
        style.font = two;
        style.dpr  = 2.0;
        QCOMPARE(previewpaint::font(style, -2, true).hintingPreference(), QFont::PreferNoHinting);
        QCOMPARE(previewpaint::emFont(style, 0.9).hintingPreference(), QFont::PreferNoHinting);
        // every style the chat modules build takes it (a source rule: ChatIntegration isn't in the test build)
        QVERIFY(source("src/chatintegration.cpp").contains(QStringLiteral("style.font               = previewpaint::fontForRatio(style.font, style.dpr);")));
        QVERIFY(source("src/chatreplies.cpp").contains(QStringLiteral("style.font  = previewpaint::fontForRatio(preview.font, style.dpr);")));
        QVERIFY(source("src/chattextreactions.cpp").contains(QStringLiteral("look.font     = previewpaint::fontForRatio(style.font, look.dpr);")));
    }

    // Live test (2.5 zoom): TeamSpeak crashed after Options > Addons > Reload at 200%. applyAlbums walked
    // positionsByKey and called refreshPreview inside the loop; a grid that got another size edited the document,
    // the positions became invalid and the next refreshPreview rebuilt the hash under the loop's iterator
    // (QString::startsWith on a freed key). A source rule (ChatIntegration isn't in the test build): no loop over
    // positionsByKey's iterators may draw, rebuild the positions or edit the document in its body.
    void noRefreshWhileWalkingPositions()
    {
        const char* files[] = {"src/chatalbums.cpp", "src/chatintegration.cpp", "src/chatintegration_spoiler.cpp", "src/chatreactions.cpp"};
        const QRegularExpression loop(QString::fromLatin1(R"(for\s*\(\s*auto\s+\w+\s*=\s*[\w.>-]*positionsByKey\.(const)?[bB]egin\(\))"));
        int loops = 0;
        for (const char* file : files) {
            const QString text = source(file);
            QVERIFY2(!text.isEmpty(), file);
            for (auto it = loop.globalMatch(text); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                // the loop's body: from the first '{' (or the single statement) after the for's closing parenthesis
                int at = m.capturedEnd(), depth = 1;
                while (at < text.size() && depth > 0) { // the for's own parentheses
                    depth += text.at(at) == QLatin1Char('(') ? 1 : text.at(at) == QLatin1Char(')') ? -1 : 0;
                    ++at;
                }
                int end = at;
                while (end < text.size() && text.at(end).isSpace())
                    ++end;
                if (end < text.size() && text.at(end) == QLatin1Char('{')) {
                    depth = 0;
                    do {
                        depth += text.at(end) == QLatin1Char('{') ? 1 : text.at(end) == QLatin1Char('}') ? -1 : 0;
                        ++end;
                    } while (end < text.size() && depth > 0);
                } else {
                    end = text.indexOf(QLatin1Char(';'), end) + 1;
                }
                const QString body = text.mid(at, end - at);
                ++loops;
                for (const char* call : {"refreshPreview(", "ensurePositions(", "renderFor(", "setCharFormat(", "insertPreview("})
                    QVERIFY2(!body.contains(QLatin1String(call)), qPrintable(QString::fromLatin1("%1: a loop over positionsByKey calls %2").arg(QLatin1String(file), QLatin1String(call))));
            }
        }
        QVERIFY(loops >= 3); // indexAlbums, refreshUnresourcedAlbums, applyScale, the visibility pass
    }

    // Live test (2.5 zoom, Z-L7): a scale pass kept every object's du size, so after the column in du shrank (a narrow
    // chat's fit, zooming in) far-away pictures stayed wider than the chat and it scrolled sideways until they came
    // near. ChatIntegration (not in the test build) collects what is over the column in applyScale and lays it out
    // again in afterScale, wherever it is: a source rule, plus the column rule it relies on.
    void scalePassLaysOutWhatOverflows()
    {
        const QString code = source("src/chatintegration.cpp");
        const int apply = code.indexOf(QLatin1String("void ChatIntegration::applyScale("));
        const int after = code.indexOf(QLatin1String("void ChatIntegration::afterScale("));
        const int next  = code.indexOf(QLatin1String("void ChatIntegration::pictureSwapped("));
        QVERIFY(apply > 0 && after > apply && next > after);
        const QString applyBody = code.mid(apply, after - apply);
        const QString afterBody = code.mid(after, next - after);
        QVERIFY(applyBody.contains(QLatin1String("if (scale.toLogical(QSizeF(it.value())).width() > room + 0.5)")));
        QVERIFY(applyBody.contains(QLatin1String("view->overColumn.insert(it.key());")));
        QVERIFY(afterBody.contains(QLatin1String("for (const QString& key : over) {")));
        QVERIFY(afterBody.contains(QLatin1String("refreshPreview(browser, key, false);")));
        // The column a scale pass compares against: at s 2 a 265 px chat allows less than the 400 du a wide chat had.
        const PreviewScale wide   = previewscale::computeScale(22, 100, 1465, 4, 0, 1.0);
        const PreviewScale narrow = previewscale::computeScale(22, 100, 265, 4, 0, 1.0);
        QVERIFY(previewscale::mediaMaxWidthDu(400, 265, 0, narrow) < previewscale::mediaMaxWidthDu(400, 1465, 0, wide));
        QVERIFY(previewscale::mediaMaxWidthDu(400, 265, 0, narrow) * narrow.s() <= 265);
    }

    // ---- Z-T12: the GIF frame rule ---------------------------------------------------------------------------------
    void gifFrames()
    {
        // A 100x80 GIF in a 400x320 du box at R 2: its own size (never above it).
        QCOMPARE(previewscale::animationFrameSize(QSize(100, 80), QSizeF(400, 320), 2.0), QSize(100, 80));
        // A 1000x800 GIF in the same box: the box at ratio 2.
        QCOMPARE(previewscale::animationFrameSize(QSize(1000, 800), QSizeF(400, 320), 2.0), QSize(800, 640));
        // R 3 is capped at 2.
        QCOMPARE(previewscale::animationFrameSize(QSize(2000, 1600), QSizeF(400, 320), 3.0), QSize(800, 640));
        // R 1: the box itself.
        QCOMPARE(previewscale::animationFrameSize(QSize(1000, 800), QSizeF(400, 320), 1.0), QSize(400, 320));
        // Aspect kept.
        const QSize wide = previewscale::animationFrameSize(QSize(1600, 400), QSizeF(400, 300), 2.0);
        QCOMPARE(wide, QSize(800, 200));
    }

    // ---- Z-T3 / Z-T4: every look laid out in du, shown at du x s ---------------------------------------------------
    void layoutsScaleLinearly()
    {
        const QList<int> scales = {20, 25, 30, 40, 60};
        // Uploaded media, cards, albums: their du size doesn't depend on the scale; on screen it is du x s (whole px).
        const MediaEntry picture = pictureEntry(1200, 800);
        const MediaEntry video   = pictureEntry(1920, 1080, MediaKind::Video);
        MediaEntry       file    = pictureEntry(0, 0, MediaKind::Other);
        file.link.fileName       = QStringLiteral("project_files.zip");
        MediaEntry audio         = pictureEntry(0, 0, MediaKind::Audio);
        audio.link.fileName      = QStringLiteral("summer_mix.mp3");
        audio.link.durationMs    = 205000;
        QSize firstPicture, firstVideo, firstFile, firstAudio, firstVoice, firstAlbum;
        for (const int s20 : scales) {
            const PreviewScale scale = scaleAt(s20);
            const PreviewStyle style = styleAt(scale);
            const QSize        p     = previewLogicalSize(picture, style);
            const QSize        v     = previewLogicalSize(video, style);
            const QSize        f     = previewLogicalSize(file, style);
            const QSize        a     = audioCardSize(audio, style);
            const QSize        vc    = voiceCardSize(style);
            const QSize        al    = albumLogicalSize(5, style);
            if (s20 == 20) {
                firstPicture = p, firstVideo = v, firstFile = f, firstAudio = a, firstVoice = vc, firstAlbum = al;
                QCOMPARE(p, QSize(400, 267)); // 2.4's numbers at s 1
                QCOMPARE(f, QSize(340, 64));
            }
            QCOMPARE(p, firstPicture);
            QCOMPARE(v, firstVideo);
            QCOMPARE(f, firstFile);
            QCOMPARE(a, firstAudio);
            QCOMPARE(vc, firstVoice);
            QCOMPARE(al, firstAlbum);
            for (const QSize& du : {p, v, f, a, vc, al}) {
                const QSizeF logical = scale.toLogical(QSizeF(du));
                QVERIFY(qAbs(logical.width() - du.width() * scale.s()) <= 0.5);
                QVERIFY(qAbs(logical.height() - du.height() * scale.s()) <= 0.5);
            }
            // The picture is drawn at R: du x R pixels.
            QSize        drawn;
            const QImage img = renderPreview(file, MediaStill(), style, &drawn);
            QCOMPARE(drawn, f);
            QCOMPARE(img.devicePixelRatio(), scale.ratio());
            QCOMPARE(img.size(), QSize(qRound(f.width() * scale.ratio()), qRound(f.height() * scale.ratio())));
        }
        // Link cards and polls: the same du layout (line breaks, rects) at every scale and for any chat font size.
        linkcard::Card yt;
        yt.look   = linkcard::Look::YouTube;
        yt.kind   = linkpreview::Kind::YouTube;
        yt.host   = QStringLiteral("youtube.com");
        yt.title  = QStringLiteral("Lakeside timelapse: a whole day in four minutes, from sunrise to the stars");
        yt.author = QStringLiteral("Quiet Places");
        linkcard::Card strip;
        strip.look = linkcard::Look::Click;
        strip.kind = linkpreview::Kind::Video;
        strip.host = QStringLiteral("files.example.com");
        pollart::Card poll;
        poll.question = QStringLiteral("Where do we eat tonight? A long question that wraps onto a second line");
        poll.options  = QStringList{QStringLiteral("Pizza"), QStringLiteral("Sushi"), QStringLiteral("Tacos")};
        const linkcard::Layout ytBase    = linkcard::layout(yt, styleAt(PreviewScale()));
        const linkcard::Layout stripBase = linkcard::layout(strip, styleAt(PreviewScale()));
        pollart::Style         ps;
        ps.font                          = styleAt(PreviewScale()).font;
        const pollart::Layout  pollBase  = pollart::layout(poll, ps);
        for (const int s20 : scales) {
            for (const qreal pt : {6.0, 8.25, 12.0, 20.0}) {
                PreviewStyle style = styleAt(scaleAt(s20));
                style.font.setPointSizeF(pt);
                const linkcard::Layout l = linkcard::layout(yt, style);
                QCOMPARE(l.size, ytBase.size);
                QCOMPARE(l.title, ytBase.title);
                QCOMPARE(l.thumb, ytBase.thumb);
                QCOMPARE(l.footer, ytBase.footer);
                QCOMPARE(linkcard::layout(strip, style).size, stripBase.size);
                QCOMPARE(linkcard::layout(strip, style).action, stripBase.action);
                pollart::Style p = ps;
                p.font           = style.font;
                p.dpr            = style.dpr;
                const pollart::Layout pl = pollart::layout(poll, p);
                QCOMPARE(pl.size, pollBase.size);
                QCOMPARE(pl.question, pollBase.question);
                QCOMPARE(pl.rows, pollBase.rows);
            }
        }
        // YouTube's maxres picture is 16:9 already: only a 4:3 (hqdefault) is cut (Z-T10).
        QCOMPARE(linkpreview::maker::crop16x9(QSize(1280, 720)), QRect(0, 0, 1280, 720));
        QCOMPARE(linkpreview::maker::crop16x9(QSize(480, 360)), QRect(0, 45, 480, 270));
    }

    // ---- Z-T5: hit tests at s 1.5 and 2 (pointer positions divided by s) -------------------------------------------
    void hitTestsUnderScale()
    {
        for (const int s20 : {30, 40}) {
            for (const qreal dpr : {1.0, 1.25}) {
                const PreviewScale scale = scaleAt(s20, dpr);
                const PreviewStyle style = styleAt(scale);
                // A point of a du layout, shown on screen (logical) and pointed at: back in du, the same zone.
                const auto viaScreen = [&](const QPointF& du) { return scale.toDu(QPointF(scale.toLogical(du.x()), scale.toLogical(du.y()))); };
                linkcard::Card strip;
                strip.look = linkcard::Look::Click;
                strip.kind = linkpreview::Kind::Video;
                strip.host = QStringLiteral("files.example.com");
                const linkcard::Layout l = linkcard::layout(strip, style);
                for (const QRectF& r : {l.action, l.hide, l.text}) {
                    if (r.isEmpty())
                        continue;
                    QCOMPARE(int(linkcard::zoneAt(strip, l, viaScreen(r.center()), true)), int(linkcard::zoneAt(strip, l, r.center(), true)));
                }
                // Video controls: the du size and a du position.
                const QSize video(400, 225);
                for (const QPointF& p : {QPointF(200, 112), QPointF(20, 213), QPointF(380, 213), QPointF(200, 188)}) {
                    QCOMPARE(int(videoZoneAt(video, viaScreen(p), false)), int(videoZoneAt(video, p, false)));
                    QVERIFY(qAbs(seekFractionAt(video, viaScreen(p)) - seekFractionAt(video, p)) < 0.01); // within a device px
                }
                // Polls.
                pollart::Card poll;
                poll.question = QStringLiteral("Where?");
                poll.options  = QStringList{QStringLiteral("Pizza"), QStringLiteral("Sushi")};
                pollart::Style ps;
                ps.font = style.font;
                ps.dpr  = style.dpr;
                const pollart::Layout pl = pollart::layout(poll, ps);
                for (const QRectF& r : pl.rows) {
                    const pollart::Hit a = pollart::hitTest(poll, pl, viaScreen(r.center()));
                    const pollart::Hit b = pollart::hitTest(poll, pl, r.center());
                    QCOMPARE(int(a.zone), int(b.zone));
                    QCOMPARE(a.index, b.index);
                }
                // Audio and voice cards.
                MediaEntry audio      = pictureEntry(0, 0, MediaKind::Audio);
                audio.link.fileName   = QStringLiteral("a.mp3");
                audio.link.durationMs = 60000;
                const QSize     card  = audioCardSize(audio, style);
                PlaybackOverlay o;
                for (const QPointF& p : {QPointF(32, 32), QPointF(card.width() / 2.0, 44), QPointF(card.width() - 20.0, 20)})
                    QCOMPARE(int(audioZoneAt(audio, o, style, card, viaScreen(p))), int(audioZoneAt(audio, o, style, card, p)));
                // Album tiles.
                const albums::Geometry g = albums::layout(5, 400, 300);
                for (int i = 0; i < g.tiles.size(); ++i)
                    QCOMPARE(albums::tileAt(g, viaScreen(QRectF(g.tiles.at(i)).center())), i);
            }
        }
    }

    // ---- Z-T6: never wider than the column ---------------------------------------------------------------------------
    void neverWiderThanColumn()
    {
        int checked = 0;
        for (const int chatPx : {22, 33}) {               // the font asks for s 2 and 3
            for (const bool bubbles : {false, true}) {
                const int   bubbleL = bubbles ? qRound(18 * chatPx / 12.0) : 0; // ChatLayout's scaled(18, f)
                const qreal docL    = 4;
                const int   floorL  = qCeil(2 * docL + qMax(104, 81 + bubbleL));
                for (int column = floorL; column <= 400; column += 7) {
                    const PreviewScale scale = previewscale::computeScale(chatPx, 100, column, docL, bubbleL, 1.0);
                    const qreal        s     = scale.s();
                    const int          media = previewscale::mediaMaxWidthDu(400, column, 0, scale);
                    const int          links = previewscale::cardMaxWidthDu(media, column, 0, docL, bubbleL, scale);
                    const int          polls = previewscale::cardMaxWidthDu(1 << 20, column, 0, docL, bubbleL, scale);
                    const qreal        right = qMax(12 * s, bubbles ? bubbleL + 2 * s : 0.0);
                    // Media: within the column less its 48 du; links and polls: within the margins.
                    QVERIFY2(scale.toLogical(media) + 48 * s <= column + 0.01, qPrintable(QStringLiteral("media %1 at %2").arg(media).arg(column)));
                    PreviewStyle style = styleAt(scale, links);
                    for (const linkcard::Look look : {linkcard::Look::Click, linkcard::Look::YouTube, linkcard::Look::Poster, linkcard::Look::Embed}) {
                        linkcard::Card card;
                        card.look            = look;
                        card.kind            = look == linkcard::Look::YouTube ? linkpreview::Kind::YouTube : linkpreview::Kind::Video;
                        card.host            = QStringLiteral("example.com");
                        card.title           = QStringLiteral("A title");
                        card.data.title      = QStringLiteral("A page");
                        card.data.layout     = linkpreview::CardLayout::Large;
                        card.data.pictureWidth  = 1200;
                        card.data.pictureHeight = 630;
                        const QSize du = linkcard::layout(card, style).size;
                        QVERIFY2(scale.toLogical(du.width()) + 2 * docL + right <= column + 0.01,
                                 qPrintable(QStringLiteral("look %1 %2 du at s %3 in %4 px (bubbles %5)").arg(int(look)).arg(du.width()).arg(s).arg(column).arg(bubbles)));
                    }
                    pollart::Style ps;
                    ps.font     = style.font;
                    ps.maxWidth = polls;
                    const QSize pd = pollart::layout(pollart::Card{QStringLiteral("Q"), {QStringLiteral("A"), QStringLiteral("B")}}, ps).size;
                    QVERIFY(scale.toLogical(pd.width()) + 2 * docL + right <= column + 0.01);
                    ++checked;
                }
            }
        }
        QVERIFY(checked > 100);
        // In a real chat: a slot of that width in a QTextBrowser of that width scrolls nothing sideways.
        for (const int column : {200, 260, 330}) {
            QTextBrowser browser;
            browser.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            browser.resize(column + 2 * browser.frameWidth(), 300);
            browser.show();
            QCOMPARE(browser.viewport()->width(), column);
            const tschat::Skin s = tschat::darkSkin();
            tschat::append(browser.document(), tschat::messageHtml(s, QStringLiteral("16:00:17"), 7, QString::fromLatin1(tschat::kKaiUid), QStringLiteral("Kai"),
                                                                   tschat::linkHtml(s, QStringLiteral("https://example.com/watch/a"))));
            const PreviewScale scale = previewscale::computeScale(22, 100, column, browser.document()->documentMargin(), 0, browser.devicePixelRatioF());
            const int          du    = previewscale::cardMaxWidthDu(400, column, 0, browser.document()->documentMargin(), 0, scale);
            const QVector<linkdoc::Link> links = linkdoc::findLinks(browser.document(), {});
            QCOMPARE(links.size(), 1);
            QVERIFY(linkdoc::insertSlot(browser.document(), links.first(), linkdoc::cardPrefix() + QStringLiteral("z"), QSizeF(du, 64), scale));
            browser.document()->documentLayout()->documentSize();
            QCoreApplication::processEvents();
            QCOMPARE(browser.horizontalScrollBar()->maximum(), 0);
        }
    }

    // ---- Z-T8 (part): sizes compared in du never flip-flop ------------------------------------------------------------
    void duComparisonIsStable()
    {
        PreviewScale scale = scaleAt(29, 1.25); // s 1.45 at dpr 1.25: fractional everywhere
        QTextDocument doc;
        tschat::append(&doc, tschat::messageHtml(tschat::darkSkin(), QStringLiteral("16:00:17"), 7, QString::fromLatin1(tschat::kKaiUid), QStringLiteral("Kai"),
                                                 tschat::linkHtml(tschat::darkSkin(), QStringLiteral("https://example.com/watch/a"))));
        const QVector<linkdoc::Link> links = linkdoc::findLinks(&doc, {});
        QVERIFY(linkdoc::insertSlot(&doc, links.first(), linkdoc::cardPrefix() + QStringLiteral("a"), QSizeF(333, 57), scale));
        const linkdoc::Slot slot = linkdoc::allSlots(&doc).first();
        QCOMPARE(slot.size, QSizeF(333, 57)); // du, read back exactly
        // The same du at the same scale: nothing to do (no edit, no relayout); twice.
        QVERIFY(!linkdoc::resizeObject(&doc, slot.object, QSizeF(333, 57), scale));
        QVERIFY(!linkdoc::resizeObject(&doc, slot.object, linkdoc::allSlots(&doc).first().size, scale));
        // A new scale: the same du, new logical sizes.
        const PreviewScale two = scaleAt(40, 1.0);
        QVERIFY(linkdoc::resizeObject(&doc, slot.object, QSizeF(333, 57), two));
        QCOMPARE(linkdoc::allSlots(&doc).first().size, QSizeF(333, 57));
        QTextCursor c(&doc);
        c.setPosition(slot.object);
        c.setPosition(slot.object + 1, QTextCursor::KeepAnchor);
        const QTextImageFormat f = c.charFormat().toImageFormat();
        QCOMPARE(f.width(), 666.0);
        QCOMPARE(f.height(), 114.0);
        // Logical sizes on whole device px.
        QTextImageFormat g;
        previewscale::setObjectSize(g, QSizeF(333, 57), scale);
        QCOMPARE(qRound(g.width() * 1.25) / 1.25, g.width());
        QCOMPARE(previewscale::duSizeOf(g), QSizeF(333, 57));
        // An object from before 2.5's zoom (no du property): its logical size.
        QTextImageFormat old;
        old.setWidth(120);
        old.setHeight(40);
        QCOMPARE(previewscale::duSizeOf(old), QSizeF(120, 40));
    }

    // ---- Z-T9: fonts and signatures ------------------------------------------------------------------------------------
    void fontsAndSignatures()
    {
        for (const qreal pt : {6.0, 8.25, 12.0, 20.0}) {
            QFont f(QStringLiteral("Segoe UI"));
            f.setPointSizeF(pt);
            QCOMPARE(previewpaint::basePixelSize(f), 11.0);
            PreviewStyle style;
            style.font = f;
            QCOMPARE(previewpaint::font(style, 0).pixelSize(), 11);
            QCOMPARE(previewpaint::font(style, -1.5).pixelSize(), 10); // qRound(9.5)
            QCOMPARE(previewpaint::emFont(style, 1.06).pixelSize(), 12);
            QCOMPARE(previewpaint::emFont(style, 1.06).family(), f.family());
            QCOMPARE(replyart::scaledFont(f, 1.0).pixelSize(), 11);
        }
        // A card's signature follows the canvas ratio (and so s at a given dpr) and the du column.
        linkcard::Card card;
        card.look = linkcard::Look::Click;
        card.host = QStringLiteral("example.com");
        const QString one = linkcard::signature(card, styleAt(scaleAt(20)), linkcard::Pointer());
        QVERIFY(one != linkcard::signature(card, styleAt(scaleAt(30)), linkcard::Pointer()));
        QVERIFY(one != linkcard::signature(card, styleAt(scaleAt(20, 2.0)), linkcard::Pointer()));
        QVERIFY(scaleAt(20).key() != scaleAt(30).key());
        QVERIFY(scaleAt(20, 1.0).key() != scaleAt(20, 2.0).key());
        // Source rule: no art module computes a size from points (they would scale twice under R).
        for (const char* file : {"src/previewrenderer.cpp", "src/linkpreview/linkcard.cpp", "src/reactionart.cpp", "src/pollart.cpp", "src/replyart.cpp",
                                 "src/audiocard.cpp", "src/voicecard.cpp"}) {
            const QString text = source(file);
            QVERIFY2(!text.isEmpty(), file);
            QVERIFY2(!text.contains(QLatin1String("pointSize")), file);
        }
        // The CRASH RULE and metatypes over the new files.
        for (const char* file : {"src/chatscale.cpp", "src/chatscale.h", "src/previewscale.h"}) {
            const QString text = source(file);
            QVERIFY2(!text.isEmpty(), file);
            QVERIFY2(!text.contains(QLatin1String("singleShot(")), file);
            QVERIFY2(!text.contains(QLatin1String("qRegisterMetaType")), file);
            QVERIFY2(!text.contains(QLatin1String("Q_DECLARE_METATYPE")), file);
        }
    }

    // ---- Z-T7: zoom keeps the reader's place; one pass per gesture; no full layout during it ---------------------
    void zoomKeepsPlace()
    {
        for (const bool atBottom : {false, true}) {
            Chat         chat(120);
            QTextBrowser& b = chat.browser;
            ChatScale::Host host;
            ChatScale*      scaler = nullptr;
            int             applied = 0;
            host.compute   = [&] { return previewscale::computeScale(chatPxOf(b), 100, b.viewport()->width(), 4, 0, 1.0); };
            host.apply     = [&](const PreviewScale& s) { chat.apply(s); ++applied; };
            host.fontEvent = [&] { scaler->request(); };
            scaler         = new ChatScale(&b, host);
            scaler->setScale(host.compute());
            QScrollBar* bar = b.verticalScrollBar();
            QVERIFY(bar->maximum() > 2000);
            const int block = 60;
            if (atBottom) {
                bar->setValue(bar->maximum());
            } else {
                bar->setValue(qRound(b.document()->documentLayout()->blockBoundingRect(b.document()->findBlockByNumber(block)).top()) + 1);
            }
            const qreal before = chat.blockTop(block);
            // Ctrl + wheel: ten notches 20 ms apart (QTextEdit::zoomInF sets the font: FontChange on the chat).
            QElapsedTimer clock;
            clock.start();
            for (int i = 0; i < 10; ++i) {
                QFont f = b.font();
                f.setPointSizeF(f.pointSizeF() + 0.5);
                b.setFont(f);
                QTest::qWait(20);
            }
            QVERIFY(scaler->pending());
            QTRY_VERIFY_WITH_TIMEOUT(!scaler->pending(), 2000);
            QCOMPARE(scaler->passes(), 1); // one pass for the whole gesture
            QCOMPARE(applied, 1);
            QVERIFY(scaler->scale().s20 > 20);
            QCoreApplication::processEvents();
            if (atBottom) {
                QVERIFY(bar->value() >= bar->maximum() - 4);
            } else {
                const qreal after = chat.blockTop(block);
                QVERIFY2(qAbs(after - before) <= 2.0, qPrintable(QStringLiteral("%1 -> %2").arg(before).arg(after)));
            }
            qInfo("gesture of 10 notches: %lld ms, %d holds", clock.elapsed(), scaler->holds());
            delete scaler;
        }
    }

    void zoomOnFullChatLaysOutOnce()
    {
        // A chat filled to TeamSpeak's limit (MaxLines): during the gesture no full layout (syncRange) runs; at settle one.
        Chat         chat(1000, 10);
        QTextBrowser& b = chat.browser;
        ChatScale::Host host;
        ChatScale*      scaler = nullptr;
        host.compute   = [&] { return previewscale::computeScale(chatPxOf(b), 100, b.viewport()->width(), 4, 0, 1.0); };
        host.apply     = [&](const PreviewScale& s) { chat.apply(s); };
        host.fontEvent = [&] { scaler->request(); };
        scaler         = new ChatScale(&b, host);
        scaler->setScale(host.compute());
        b.verticalScrollBar()->setValue(b.verticalScrollBar()->maximum() / 2);
        const int syncBefore = chatscroll::syncRangeCalls();
        QElapsedTimer clock;
        clock.start();
        for (int i = 0; i < 10; ++i) {
            QElapsedTimer one;
            one.start();
            QFont f = b.font();
            f.setPointSizeF(f.pointSizeF() + 0.5);
            b.setFont(f);
            const qint64 handler = one.nsecsElapsed() / 1000;
            QTest::qWait(20);
            qInfo("notch %d: %lld us in the handlers, %lld us with the event loop's turn", i, handler, one.nsecsElapsed() / 1000);
            QCOMPARE(chatscroll::syncRangeCalls(), syncBefore); // nothing laid the whole chat out yet
        }
        QTRY_VERIFY_WITH_TIMEOUT(!scaler->pending(), 3000);
        const qint64 total = clock.elapsed(); // the gesture and its settle time
        QCOMPARE(chatscroll::syncRangeCalls(), syncBefore + 1); // one, at settle
        // The hold ran at most once per 100 ms (not once per notch).
        QVERIFY2(scaler->holds() <= total / ChatScale::kHoldMs + 1, qPrintable(QStringLiteral("%1 holds in %2 ms").arg(scaler->holds()).arg(total)));
        QVERIFY(scaler->holds() < 10);
        QVERIFY(scaler->holds() >= 1);
        delete scaler;
    }

    // ---- Z-T8: one edit block per scale pass ----------------------------------------------------------------------------
    void oneEditBlockPerPass()
    {
        Chat         chat(60, 3);
        QTextBrowser& b = chat.browser;
        int           changes = 0;
        connect(b.document(), &QTextDocument::contentsChange, &b, [&changes](int, int, int) { ++changes; });
        ChatScale::Host host;
        PreviewScale    want = scaleAt(30);
        int             afters = 0;
        host.compute = [&] { return want; };
        host.apply   = [&](const PreviewScale& s) { chat.apply(s); };
        host.after   = [&] { ++afters; };
        ChatScale scaler(&b, host);
        scaler.setScale(PreviewScale());
        scaler.request();
        scaler.settle();
        QCOMPARE(changes, 1); // every object resized: one contentsChange, one relayout
        QCOMPARE(afters, 1);
        // An unchanged scale: only the place (no edit, nothing drawn).
        changes = 0;
        scaler.request();
        scaler.settle();
        QCOMPARE(changes, 0);
        QCOMPARE(afters, 1);
        QCOMPARE(scaler.passes(), 2);
        // Unload: nothing pending, no host call afterwards.
        want = scaleAt(40);
        scaler.request();
        scaler.prepareShutdown();
        QVERIFY(!scaler.pending());
        scaler.settle();
        QCOMPARE(afters, 1);
    }

    // ---- Z-T11: the picture budget -----------------------------------------------------------------------------------
    void pictureBudget()
    {
        Chat          chat(400, 2); // 200 previews of 300 x 200
        QTextBrowser& b = chat.browser;
        QStringList   swappedNames;
        ChatScale::Host host;
        host.swapped = [&](const QString& name, int) { swappedNames << name; };
        ChatScale scaler(&b, host);
        // 200 pictures at s 2 (600 x 400 px: 0.9 MiB each, 180 MiB in all), against a 48 MiB budget (the x86 value).
        scaler.setBudget(48LL * 1024 * 1024);
        QImage big(600, 400, QImage::Format_ARGB32_Premultiplied);
        big.fill(Qt::darkMagenta);
        const QPixmap pixmap = QPixmap::fromImage(big);
        QScrollBar*   bar    = b.verticalScrollBar();
        QTextDocument* doc   = b.document();
        // The objects within 800 px of the view (never swapped) and the ones the view shows.
        const auto near = [&](qreal margin) {
            QSet<QString> out;
            const qreal   top = bar->value() - margin, bottom = bar->value() + b.viewport()->height() + margin;
            for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
                const QRectF r = doc->documentLayout()->blockBoundingRect(block);
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const QTextFragment f = it.fragment();
                    if (f.isValid() && f.charFormat().isImageFormat() && r.bottom() >= top && r.top() <= bottom)
                        out.insert(f.charFormat().toImageFormat().name());
                }
            }
            return out;
        };
        const auto check = [&] {
            QVERIFY2(scaler.pictureBytes() <= scaler.budget(), qPrintable(QString::number(scaler.pictureBytes())));
            for (const QString& name : near(ChatScale::kSwapMargin - 50))
                QVERIFY2(!scaler.isSwapped(name), qPrintable(name));
        };
        // Top to bottom: pictures are drawn as they come near (as the modules do), the budget enforced after each scroll.
        for (int step = 0; step <= 10; ++step) {
            bar->setValue(bar->maximum() * step / 10);
            for (const QString& name : near(400))
                scaler.putPicture(name, pixmap, ChatScale::Media);
            scaler.enforceBudget();
            check();
        }
        QVERIFY(!swappedNames.isEmpty());
        // And back up, at five places: what the view shows is a full picture.
        for (int step = 4; step >= 0; --step) {
            bar->setValue(bar->maximum() * step / 4);
            for (const QString& name : near(400)) {
                const QPixmap now = qvariant_cast<QPixmap>(doc->resource(QTextDocument::ImageResource, QUrl(name)));
                if (scaler.isSwapped(name) || now.size() != pixmap.size())
                    scaler.putPicture(name, pixmap, ChatScale::Media); // drawn (again) before it is on screen
            }
            scaler.enforceBudget();
            check();
            for (const QString& name : near(0)) {
                const QPixmap shown = qvariant_cast<QPixmap>(doc->resource(QTextDocument::ImageResource, QUrl(name)));
                QCOMPARE(shown.size(), pixmap.size());
            }
        }
        QCOMPARE(ChatScale::defaultBudget(), sizeof(void*) == 8 ? 128LL * 1024 * 1024 : 48LL * 1024 * 1024);
        // forget(): a picture that left the document leaves the books.
        const qint64 before = scaler.pictureBytes();
        scaler.putPicture(QStringLiteral("tsmedia:extra"), pixmap, ChatScale::Links);
        QCOMPARE(scaler.pictureBytes(), before + 600LL * 400 * 4);
        scaler.forget(QStringLiteral("tsmedia:extra"));
        QCOMPARE(scaler.pictureBytes(), before);
    }

    void pictureBudgetLeftDocument()
    {
        // Pictures that left the document (blocks TeamSpeak's line limit trimmed, previews that went without a forget)
        // are the first to go, and their stand-in frees the document's copy; a chat stuck over its budget doesn't walk
        // the document for every frame of a GIF or a video (review fix).
        Chat          chat(60, 2); // 30 objects
        QTextBrowser& b = chat.browser;
        ChatScale::Host host;
        ChatScale       scaler(&b, host);
        scaler.setBudget(8LL * 1024 * 1024);
        QImage big(600, 400, QImage::Format_ARGB32_Premultiplied);
        big.fill(Qt::darkMagenta);
        const QPixmap pixmap = QPixmap::fromImage(big); // 0.9 MiB
        QTextDocument* doc   = b.document();
        // Ten pictures booked under names no fragment shows (their blocks were trimmed): 9 MiB over an 8 MiB budget.
        for (int i = 0; i < 10; ++i)
            scaler.putPicture(QStringLiteral("tsmedia:gone%1").arg(i), pixmap, ChatScale::Media);
        QVERIFY(scaler.pictureBytes() > scaler.budget());
        scaler.enforceBudget();
        QVERIFY(scaler.pictureBytes() <= scaler.budget() / 4 * 3);
        int blanked = 0;
        for (int i = 0; i < 10; ++i) {
            const QString name = QStringLiteral("tsmedia:gone%1").arg(i);
            if (scaler.isSwapped(name)) {
                ++blanked;
                QCOMPARE(qvariant_cast<QPixmap>(doc->resource(QTextDocument::ImageResource, QUrl(name))).size(), QSize(1, 1)); // memory freed
            }
        }
        QVERIFY(blanked >= 3);
        // Stuck: every picture is near the view (a 1 MiB budget, the view's own pictures over it).
        ChatScale tight(&b, host);
        tight.setBudget(1LL * 1024 * 1024);
        b.verticalScrollBar()->setValue(0);
        const QString first = QStringLiteral("tsmedia:obj0");
        tight.putPicture(first, pixmap, ChatScale::Media);
        tight.putPicture(QStringLiteral("tsmedia:obj2"), pixmap, ChatScale::Media);
        tight.enforceBudget();
        const int scans = tight.budgetScans();
        QVERIFY(tight.pictureBytes() > tight.budget());
        // A GIF's next frames: the same bytes again and again: no new scan (no timer, and the pass skips it).
        for (int i = 0; i < 30; ++i)
            tight.putPicture(first, pixmap, ChatScale::Media);
        QCoreApplication::processEvents();
        tight.enforceBudget(); // the 300 ms pass with nothing changed
        QCOMPARE(tight.budgetScans(), scans);
        // A scroll is a change: scanned again.
        b.verticalScrollBar()->setValue(40);
        tight.enforceBudget();
        QCOMPARE(tight.budgetScans(), scans + 1);
        // A new document (TeamSpeak swapped it): the books start again.
        tight.resetLedger();
        QCOMPARE(tight.pictureBytes(), qint64(0));
        QCOMPARE(tight.pictureCount(), 0);
    }

    void placeFollowsTrim()
    {
        // A zoom gesture holds the reader's place; meanwhile TeamSpeak's line limit trims blocks at the top (every
        // block renumbered): the same message comes back to the top (review fix: the anchor follows a cursor).
        Chat          chat(200, 0);
        QTextBrowser& b   = chat.browser;
        QScrollBar*   bar = b.verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        QTextDocument*          doc   = b.document();
        const chatscroll::Place place = chatscroll::capture(&b);
        QVERIFY(!place.bottom);
        const QString text = doc->findBlockByNumber(place.anchor.block).text();
        QVERIFY(!text.isEmpty());
        QTextCursor trim(doc);
        trim.setPosition(0);
        trim.setPosition(doc->findBlockByNumber(30).position(), QTextCursor::KeepAnchor);
        trim.removeSelectedText();
        trim.deleteChar(); // the emptied first block goes too
        chatscroll::restoreAnchor(&b, place);
        int at = doc->documentLayout()->hitTest(QPointF(doc->documentMargin() + 1, bar->value() + 1), Qt::FuzzyHit);
        QCOMPARE(doc->findBlock(at).text(), text);
        chatscroll::restore(&b, place);
        at = doc->documentLayout()->hitTest(QPointF(doc->documentMargin() + 1, bar->value() + 1), Qt::FuzzyHit);
        QCOMPARE(doc->findBlock(at).text(), text);
    }
};

TSMEDIA_REGISTER_TEST(TestPreviewScale)
#include "tst_previewscale.moc"
