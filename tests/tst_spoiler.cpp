// 2.2 spoiler: unit tests for src/spoiler.* — which keys are covered (SpoilerState), the cover picture
// (no detail survives, the BlurHash is preferred), the cover drawing without text, the contrast of what
// is written on covers, and the reveal crossfade. The pill and the previews themselves are drawn and
// checked by tools/render_gallery (fonts need a GUI application).

#include <QPainter>
#include <QSignalSpy>
#include <QtTest>

#include "blurhash.h"
#include "medialink.h"
#include "spoiler.h"
#include "testmain.h"
#include "uiutil.h"

namespace {

const QString kHash = QStringLiteral("LEHV6nWB2yk8pyo0adR*.7kCMdnj"); // the example on blurha.sh

MediaLink spoilerLink(const QString& fileName, bool spoiler)
{
    MediaLink link;
    link.host      = QStringLiteral("voice.example.org");
    link.port      = 9987;
    link.serverUid = QStringLiteral("uid");
    link.channelId = 3;
    link.path      = QStringLiteral("/tsmedia");
    link.fileName  = fileName;
    link.size      = 1000;
    link.dateTime  = 1700000000;
    link.protocol  = MediaLink::kProtocol;
    link.width     = 400;
    link.height    = 300;
    link.spoiler   = spoiler;
    return link;
}

// A picture that is nothing but detail: one-pixel black and white checks.
QImage detailed(const QSize& size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x)
            line[x] = ((x + y) % 2) ? qRgb(255, 255, 255) : qRgb(0, 0, 0);
    }
    return image;
}

QImage flat(const QSize& size, const QColor& color)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    return image;
}

// The largest difference of any channel between two images of the same size.
int maxDifference(const QImage& a, const QImage& b)
{
    const QImage x = a.convertToFormat(QImage::Format_ARGB32);
    const QImage y = b.convertToFormat(QImage::Format_ARGB32);
    if (x.size() != y.size())
        return 256;
    int worst = 0;
    for (int row = 0; row < x.height(); ++row) {
        const auto* p = reinterpret_cast<const QRgb*>(x.constScanLine(row));
        const auto* q = reinterpret_cast<const QRgb*>(y.constScanLine(row));
        for (int col = 0; col < x.width(); ++col) {
            worst = qMax(worst, qAbs(qRed(p[col]) - qRed(q[col])));
            worst = qMax(worst, qAbs(qGreen(p[col]) - qGreen(q[col])));
            worst = qMax(worst, qAbs(qBlue(p[col]) - qBlue(q[col])));
            worst = qMax(worst, qAbs(qAlpha(p[col]) - qAlpha(q[col])));
        }
    }
    return worst;
}

// drawCover without the pill (no fonts here) over an image filled with underneath.
QImage coverOver(const QImage& underneath, const QImage& still, bool blurHash, qreal opacity)
{
    QImage   out = underneath.copy();
    QPainter p(&out);
    spoiler::CoverLook look;
    look.placeholder = QColor(0x23, 0x24, 0x28);
    look.opacity     = opacity;
    look.withPill    = false;
    spoiler::drawCover(p, QRectF(QPointF(0, 0), QSizeF(out.size())), still, blurHash, look);
    p.end();
    return out;
}

} // namespace

class TestSpoiler : public QObject
{
    Q_OBJECT

  private slots:
    // SpoilerState
    void stateStickyPerKey();
    void stateRevealAndHide();
    void stateSettingShowsAll();
    void stateRepostCantCoverWhatWasSeen();
    void stateHiddenOnPurposeStaysHidden();
    void stateIgnoresEmptyKeys();
    void stateHeldDownloads();
    void keyIgnoresSpoilerFlag();

    // kinds and labels
    void appliesToPicturesGifsAndVideos();
    void labelsMatchLinkLabel();

    // the cover picture
    void blurredStillKeepsNoDetail();
    void blurredStillKeepsShapeAndColour();
    void blurredStillPassesBlurHashOn();
    void coverSourcePrefersBlurHash();

    // drawing (without the pill)
    void coverHidesWhatIsUnder();
    void coverWithoutPicture();
    void coverOpacity();

    // contrast of what is written on covers
    void pillAndHintContrast();

    // reveal crossfade
    void fadeRunsAndEndsUncovered();
    void revealWithoutFade();
    void cancelEndsFade();
};

void TestSpoiler::stateStickyPerKey()
{
    SpoilerState s;
    QVERIFY(!s.noteSighting(QStringLiteral("a"), false));
    QVERIFY(!s.isSpoiler(QStringLiteral("a")));
    QVERIFY(s.noteSighting(QStringLiteral("a"), true)); // made it a spoiler: its previews change
    QVERIFY(!s.noteSighting(QStringLiteral("a"), true)); // nothing new
    QVERIFY(!s.noteSighting(QStringLiteral("a"), false)); // a repost without sp doesn't uncover it
    QVERIFY(s.isSpoiler(QStringLiteral("a")));
    QVERIFY(s.isHidden(QStringLiteral("a"), false));
    QVERIFY(!s.isHidden(QStringLiteral("b"), false));
    QCOMPARE(s.spoilers().size(), 1); // (no QList compare: MSVC's deprecated checked iterators)
    QCOMPARE(s.spoilers().first(), QStringLiteral("a"));
    s.clear();
    QVERIFY(!s.isSpoiler(QStringLiteral("a")));
}

void TestSpoiler::stateRevealAndHide()
{
    SpoilerState s;
    QVERIFY(!s.setRevealed(QStringLiteral("a"), true)); // not a spoiler: nothing to reveal
    s.noteSighting(QStringLiteral("a"), true);
    s.noteSighting(QStringLiteral("b"), true);
    QVERIFY(s.setRevealed(QStringLiteral("a"), true));
    QVERIFY(!s.setRevealed(QStringLiteral("a"), true));
    QVERIFY(!s.isHidden(QStringLiteral("a"), false));
    QVERIFY(s.isRevealed(QStringLiteral("a")));
    QVERIFY(s.isHidden(QStringLiteral("b"), false)); // per item
    QVERIFY(s.setRevealed(QStringLiteral("a"), false));
    QVERIFY(!s.setRevealed(QStringLiteral("a"), false));
    QVERIFY(s.isHidden(QStringLiteral("a"), false));
}

void TestSpoiler::stateSettingShowsAll()
{
    SpoilerState s;
    s.noteSighting(QStringLiteral("a"), true);
    QVERIFY(!s.isHidden(QStringLiteral("a"), true));
    QVERIFY(s.isHidden(QStringLiteral("a"), false)); // the setting reveals nothing for good
}

void TestSpoiler::stateRepostCantCoverWhatWasSeen()
{
    SpoilerState s;
    s.noteShownOpen(QStringLiteral("a"));                 // shown uncovered first
    QVERIFY(s.noteSighting(QStringLiteral("a"), true));   // then reposted with sp=1
    QVERIFY(s.isSpoiler(QStringLiteral("a")));
    QVERIFY(!s.isHidden(QStringLiteral("a"), false));     // stays uncovered
    // Not seen before: a repost with sp=1 covers it.
    QVERIFY(s.noteSighting(QStringLiteral("b"), true));
    QVERIFY(s.isHidden(QStringLiteral("b"), false));
}

void TestSpoiler::stateHiddenOnPurposeStaysHidden()
{
    SpoilerState s;
    s.noteShownOpen(QStringLiteral("a"));
    s.noteSighting(QStringLiteral("a"), true);
    QVERIFY(s.setRevealed(QStringLiteral("a"), false)); // Hide spoiler
    QVERIFY(!s.noteSighting(QStringLiteral("a"), true));
    QVERIFY(s.isHidden(QStringLiteral("a"), false));
}

void TestSpoiler::stateIgnoresEmptyKeys()
{
    SpoilerState s;
    QVERIFY(!s.noteSighting(QString(), true));
    s.noteShownOpen(QString());
    QVERIFY(!s.isHidden(QString(), false));
    QVERIFY(s.spoilers().isEmpty());
}

void TestSpoiler::stateHeldDownloads()
{
    // Core holds a hidden spoiler's automatic download and starts it when it is revealed, once.
    SpoilerState s;
    QVERIFY(!s.releaseDownload(QStringLiteral("a")));
    s.holdDownload(QStringLiteral("a"));
    s.holdDownload(QStringLiteral("a"));
    s.holdDownload(QString());
    QVERIFY(s.releaseDownload(QStringLiteral("a")));
    QVERIFY(!s.releaseDownload(QStringLiteral("a")));
    QVERIFY(!s.releaseDownload(QString()));
    s.holdDownload(QStringLiteral("b"));
    s.clear();
    QVERIFY(!s.releaseDownload(QStringLiteral("b")));
}

void TestSpoiler::keyIgnoresSpoilerFlag()
{
    // The same file posted with and without sp shares one entry: the state has to be sticky.
    const MediaLink open    = spoilerLink(QStringLiteral("cat_3f9a1c2e.jpg"), false);
    const MediaLink covered = spoilerLink(QStringLiteral("cat_3f9a1c2e.jpg"), true);
    QCOMPARE(open.key(), covered.key());
    QVERIFY(MediaLink::parse(covered.toUrl()).spoiler);
    QVERIFY(!MediaLink::parse(open.toUrl()).spoiler);
    // Only pictures, GIFs and videos keep the flag.
    QVERIFY(!MediaLink::parse(spoilerLink(QStringLiteral("files_3f9a1c2e.zip"), true).toUrl()).spoiler);
    QVERIFY(MediaLink::parse(spoilerLink(QStringLiteral("clip_3f9a1c2e.mp4"), true).toUrl()).spoiler);
}

void TestSpoiler::appliesToPicturesGifsAndVideos()
{
    QVERIFY(spoiler::appliesTo(MediaKind::Image));
    QVERIFY(spoiler::appliesTo(MediaKind::AnimatedImage));
    QVERIFY(spoiler::appliesTo(MediaKind::Video));
    QVERIFY(!spoiler::appliesTo(MediaKind::Audio));
    QVERIFY(!spoiler::appliesTo(MediaKind::Archive));
    QVERIFY(!spoiler::appliesTo(MediaKind::Document));
    QVERIFY(!spoiler::appliesTo(MediaKind::Other));
}

void TestSpoiler::labelsMatchLinkLabel()
{
    for (const char* name : {"cat_3f9a1c2e.jpg", "funny_3f9a1c2e.gif", "clip_3f9a1c2e.mp4"}) {
        const MediaLink link = spoilerLink(QString::fromLatin1(name), true);
        QCOMPARE(spoiler::label(kindForFileName(link.fileName)), linkLabel(link));
        QVERIFY(!spoiler::label(kindForFileName(link.fileName)).contains(QLatin1String("3f9a1c2e")));
    }
    QCOMPARE(spoiler::label(MediaKind::Image), QStringLiteral("Spoiler (image)"));
    QCOMPARE(spoiler::label(MediaKind::AnimatedImage), QStringLiteral("Spoiler (GIF)"));
    QCOMPARE(spoiler::label(MediaKind::Video), QStringLiteral("Spoiler (video)"));
}

void TestSpoiler::blurredStillKeepsNoDetail()
{
    // Checks and a flat grey of the same average must give the same cover: the detail is gone.
    const QImage checks = spoiler::blurredStill(detailed(QSize(400, 300)), false);
    const QImage grey   = spoiler::blurredStill(flat(QSize(400, 300), QColor(128, 128, 128)), false);
    QVERIFY(!checks.isNull());
    QVERIFY(maxDifference(checks, grey) <= 3);

    // A sharp feature smaller than a twelfth of the picture leaves no sharp edge: no two
    // neighbouring pixels of the cover differ by much.
    QImage dot = flat(QSize(480, 480), Qt::white);
    {
        QPainter p(&dot);
        p.fillRect(QRect(220, 220, 40, 40), Qt::black);
    }
    const QImage soft = spoiler::blurredStill(dot, false).convertToFormat(QImage::Format_ARGB32);
    int          step = 0;
    for (int y = 0; y < soft.height(); ++y) {
        for (int x = 1; x < soft.width(); ++x)
            step = qMax(step, qAbs(qGray(soft.pixel(x, y)) - qGray(soft.pixel(x - 1, y))));
    }
    QVERIFY2(step <= 24, qPrintable(QStringLiteral("largest step %1").arg(step)));
}

void TestSpoiler::blurredStillKeepsShapeAndColour()
{
    QVERIFY(spoiler::blurredStill(QImage(), false).isNull());
    const QImage wide = spoiler::blurredStill(flat(QSize(1600, 400), QColor(200, 40, 40)), false);
    QCOMPARE(wide.width(), spoiler::kCoverDetail * 4);
    QCOMPARE(wide.height(), spoiler::kCoverDetail); // 12 x 3 enlarged 4x
    const QColor centre = wide.pixelColor(wide.width() / 2, wide.height() / 2);
    QVERIFY(qAbs(centre.red() - 200) <= 2 && qAbs(centre.green() - 40) <= 2 && qAbs(centre.blue() - 40) <= 2);
    // Never larger than the tiny copy enlarged, whatever the input.
    const QImage huge = spoiler::blurredStill(flat(QSize(4000, 3000), Qt::blue), false);
    QVERIFY(huge.width() <= spoiler::kCoverDetail * 4 && huge.height() <= spoiler::kCoverDetail * 4);
}

void TestSpoiler::blurredStillPassesBlurHashOn()
{
    const QImage hash = blurhash::decode(kHash, QSize(32, 24));
    QCOMPARE(spoiler::blurredStill(hash, true).cacheKey(), hash.cacheKey());
}

void TestSpoiler::coverSourcePrefersBlurHash()
{
    const QImage sharp = detailed(QSize(400, 300));
    bool         isHash = false;
    const QImage fromHash = spoiler::coverSource(kHash, QSizeF(400, 100), sharp, false, &isHash);
    QVERIFY(isHash);
    QCOMPARE(fromHash.width(), 32);
    QCOMPARE(fromHash.height(), 8); // the box's shape
    QVERIFY(fromHash.cacheKey() != sharp.cacheKey());

    const QImage noHash = spoiler::coverSource(QString(), QSizeF(400, 300), sharp, false, &isHash);
    QVERIFY(!isHash);
    QCOMPARE(noHash.cacheKey(), sharp.cacheKey());
    const QImage badHash = spoiler::coverSource(QStringLiteral("not a hash"), QSizeF(400, 300), sharp, true, &isHash);
    QVERIFY(isHash); // the still was a BlurHash decode itself
    QCOMPARE(badHash.cacheKey(), sharp.cacheKey());
    QVERIFY(spoiler::coverSource(QString(), QSizeF(400, 300), QImage(), false, nullptr).isNull());
}

void TestSpoiler::coverHidesWhatIsUnder()
{
    // Fully covered, the result doesn't depend on what was drawn underneath: nothing shows through.
    const QImage still = detailed(QSize(400, 300));
    const QImage a     = coverOver(detailed(QSize(200, 150)), still, false, 1.0);
    const QImage b     = coverOver(flat(QSize(200, 150), Qt::white), still, false, 1.0);
    QCOMPARE(maxDifference(a, b), 0);
    // And the sharp still under it isn't in it: same as covering a flat grey.
    const QImage c = coverOver(flat(QSize(200, 150), Qt::white), flat(QSize(400, 300), QColor(128, 128, 128)), false, 1.0);
    QVERIFY(maxDifference(a, c) <= 3);
    // Opaque everywhere.
    for (int y = 0; y < a.height(); y += 7) {
        for (int x = 0; x < a.width(); x += 7)
            QCOMPARE(qAlpha(a.pixel(x, y)), 255);
    }
}

void TestSpoiler::coverWithoutPicture()
{
    const QImage out = coverOver(flat(QSize(120, 90), Qt::white), QImage(), false, 1.0);
    QCOMPARE(out.pixelColor(60, 45), QColor(0x23, 0x24, 0x28));
}

void TestSpoiler::coverOpacity()
{
    const QImage under = flat(QSize(120, 90), Qt::white);
    const QImage still = flat(QSize(120, 90), Qt::black);
    QCOMPARE(maxDifference(coverOver(under, still, false, 0.0), under), 0);
    const int full = qGray(coverOver(under, still, false, 1.0).pixel(60, 45));
    const int half = qGray(coverOver(under, still, false, 0.5).pixel(60, 45));
    QVERIFY(full < 10);
    QVERIFY(qAbs(half - (255 + full) / 2) <= 3); // one layer at half strength, not dimmed twice
}

void TestSpoiler::pillAndHintContrast()
{
    // Worst case for white text: white pixels under the cover.
    const QColor white(Qt::white);
    for (const bool hash : {false, true}) {
        const QColor dimmed = ui::flatten(QColor(0, 0, 0, hash ? spoiler::kBlurHashDimAlpha : spoiler::kDimAlpha), white);
        // The chat's pill, at rest (hover and press only darken it).
        const QColor pill = ui::flatten(QColor(0, 0, 0, spoiler::kPillAlpha), dimmed);
        QVERIFY2(ui::contrastRatio(white, pill) >= 4.5, qPrintable(QString::number(ui::contrastRatio(white, pill))));
        // The viewer's hint, right on its darker cover.
        const QColor viewer = ui::flatten(QColor(0, 0, 0, hash ? spoiler::kViewerBlurHashDimAlpha : spoiler::kViewerDimAlpha), dimmed);
        const QColor hint   = ui::flatten(QColor(255, 255, 255, spoiler::kViewerHintAlpha), viewer);
        QVERIFY2(ui::contrastRatio(hint, viewer) >= 4.5, qPrintable(QString::number(ui::contrastRatio(hint, viewer))));
    }
}

void TestSpoiler::fadeRunsAndEndsUncovered()
{
    RevealFades fades;
    QSignalSpy  spy(&fades, &RevealFades::changed);
    QCOMPARE(fades.coverOpacity(QStringLiteral("a")), 0.0);
    fades.start(QStringLiteral("a"));
    QVERIFY(fades.coverOpacity(QStringLiteral("a")) > 0.9);
    QVERIFY(fades.isFading(QStringLiteral("a")));
    QCOMPARE(fades.coverOpacity(QStringLiteral("b")), 0.0); // per key

    qreal last = 1.0;
    bool  monotonic = true;
    connect(&fades, &RevealFades::changed, this, [&](const QString&) {
        const qreal now = fades.coverOpacity(QStringLiteral("a"));
        monotonic       = monotonic && now <= last + 1e-9;
        last            = now;
    });
    QTRY_VERIFY_WITH_TIMEOUT(!fades.isFading(QStringLiteral("a")), 2000);
    QTest::qWait(60); // the step after the end
    QVERIFY(monotonic);
    QVERIFY(spy.count() >= 3);
    QCOMPARE(spy.last().at(0).toString(), QStringLiteral("a"));
    QCOMPARE(last, 0.0); // the last redraw is uncovered
    QVERIFY(fades.revealedWithin(QStringLiteral("a"), 5000));
    QVERIFY(!fades.revealedWithin(QStringLiteral("a"), 1));
}

void TestSpoiler::revealWithoutFade()
{
    RevealFades fades;
    QSignalSpy  spy(&fades, &RevealFades::changed);
    fades.start(QStringLiteral("a"), false);
    QCOMPARE(fades.coverOpacity(QStringLiteral("a")), 0.0);
    QVERIFY(!fades.isFading(QStringLiteral("a")));
    QVERIFY(fades.revealedWithin(QStringLiteral("a"), 500)); // a double click still counts
    QCOMPARE(spy.count(), 1);
}

void TestSpoiler::cancelEndsFade()
{
    RevealFades fades;
    fades.start(QStringLiteral("a"));
    QSignalSpy spy(&fades, &RevealFades::changed);
    fades.cancel(QStringLiteral("a"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(fades.coverOpacity(QStringLiteral("a")), 0.0);
    QVERIFY(!fades.revealedWithin(QStringLiteral("a"), 5000));
    fades.cancel(QStringLiteral("a"));
    QCOMPARE(spy.count(), 1); // nothing left to cancel
}

TSMEDIA_REGISTER_TEST(TestSpoiler)

#include "tst_spoiler.moc"
