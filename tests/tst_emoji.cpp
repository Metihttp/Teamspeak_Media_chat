// 2.2 emoji: unit tests for the emoji table (src/emojidata.*), the grapheme-cluster segmenter
// (src/emojisegment.*), the picture cache (src/emojicache.h) and the renderer (src/emojirender.*: on a PC
// without colour emoji only the QPainter text engine is checked).

#include <QSignalSpy>
#include <QtTest>

#include <initializer_list>

#include "emojicache.h"
#include "emojidata.h"
#include "emojirender.h"
#include "emojisegment.h"
#include "testmain.h"

namespace {

// A string from code points.
QString cps(std::initializer_list<uint> points)
{
    QString out;
    for (uint cp : points) {
        if (QChar::requiresSurrogates(cp)) {
            out += QChar(QChar::highSurrogate(cp));
            out += QChar(QChar::lowSurrogate(cp));
        } else {
            out += QChar(static_cast<ushort>(cp));
        }
    }
    return out;
}

const QString kGrin   = cps({0x1F600});
const QString kJoy    = cps({0x1F602});
const QString kThumbs = cps({0x1F44D});
const QString kHeart  = cps({0x2764, 0xFE0F});
const QString kFire   = cps({0x1F525});

bool hasColourPixels(const QImage& image)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < argb.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            const QColor c = QColor::fromRgba(line[x]);
            if (c.alpha() > 128 && c.hsvSaturation() > 80)
                return true;
        }
    }
    return false;
}

} // namespace

class TestEmoji : public QObject
{
    Q_OBJECT

  private slots:
    // ---- the table ------------------------------------------------------------------------------
    void tableIsLargeAndConsistent()
    {
        QVERIFY2(emoji::count() > 2500, qPrintable(QString::number(emoji::count())));
        int bases = 0;
        for (int id = 0; id < emoji::count(); ++id) {
            QVERIFY(emoji::isValid(id));
            QVERIFY(!emoji::text(id).isEmpty());
            QVERIFY2(!emoji::name(id).isEmpty(), qPrintable(QString::number(id)));
            QCOMPARE(emoji::find(emoji::text(id)), id); // every sequence is unique
            QCOMPARE(emoji::fromWireCode(emoji::wireCode(id)), id);
            QVERIFY(emoji::isWireCode(emoji::wireCode(id)));
            const int g = static_cast<int>(emoji::group(id));
            QVERIFY(g >= 0 && g < emoji::kGroupCount);
            if (emoji::tone(id) == 0)
                ++bases;
            else
                QCOMPARE(emoji::group(id), emoji::group(emoji::baseOf(id)));
        }
        QVERIFY2(bases > 1500, qPrintable(QString::number(bases)));
        QVERIFY(!emoji::isValid(-1));
        QVERIFY(!emoji::isValid(emoji::count()));
        QVERIFY(emoji::text(-1).isEmpty());
        QVERIFY(emoji::name(emoji::count()).isEmpty());
    }

    void groupsHoldEveryBaseOnce()
    {
        QSet<int> seen;
        for (int g = 0; g < emoji::kGroupCount; ++g) {
            const QVector<int> members = emoji::members(static_cast<emoji::Group>(g));
            QVERIFY2(!members.isEmpty(), qPrintable(emoji::groupName(static_cast<emoji::Group>(g))));
            QVERIFY(!emoji::groupName(static_cast<emoji::Group>(g)).isEmpty());
            for (int id : members) {
                QCOMPARE(emoji::tone(id), 0);
                QCOMPARE(static_cast<int>(emoji::group(id)), g);
                QVERIFY(!seen.contains(id));
                seen.insert(id);
            }
        }
        int bases = 0;
        for (int id = 0; id < emoji::count(); ++id)
            bases += emoji::tone(id) == 0 ? 1 : 0;
        QCOMPARE(seen.size(), bases);
        // The picker opens on the smileys: the first one is the grinning face.
        QCOMPARE(emoji::members(emoji::Group::SmileysPeople).value(0), emoji::find(kGrin));
    }

    void skinTones()
    {
        const int thumbs = emoji::find(kThumbs);
        QVERIFY(thumbs >= 0);
        QVERIFY(emoji::hasTones(thumbs));
        for (int t = 1; t <= emoji::kToneCount; ++t) {
            const int variant = emoji::withTone(thumbs, t);
            QCOMPARE(variant, thumbs + t);
            QCOMPARE(emoji::tone(variant), t);
            QCOMPARE(emoji::baseOf(variant), thumbs);
            QCOMPARE(emoji::text(variant), kThumbs + cps({0x1F3FA + static_cast<uint>(t)}));
            QVERIFY(emoji::name(variant).startsWith(emoji::name(thumbs) + QStringLiteral(": ")));
            QVERIFY(emoji::name(variant).endsWith(QStringLiteral("skin tone")));
            QCOMPARE(emoji::withTone(variant, 0), thumbs);
            QCOMPARE(emoji::withTone(variant, 2), thumbs + 2);
        }
        // No tones on a face; the request gives the emoji itself.
        const int grin = emoji::find(kGrin);
        QVERIFY(!emoji::hasTones(grin));
        QCOMPARE(emoji::withTone(grin, 3), grin);
        // A ZWJ profession with tones: the tone goes after the person.
        const int coder = emoji::find(cps({0x1F9D1, 0x200D, 0x1F4BB}));
        QVERIFY(coder >= 0);
        QVERIFY(emoji::hasTones(coder));
        QCOMPARE(emoji::text(emoji::withTone(coder, 5)), cps({0x1F9D1, 0x1F3FF, 0x200D, 0x1F4BB}));
        // Couples take the tone on both people.
        const int holding = emoji::find(cps({0x1F9D1, 0x200D, 0x1F91D, 0x200D, 0x1F9D1}));
        if (holding >= 0 && emoji::hasTones(holding))
            QCOMPARE(emoji::text(emoji::withTone(holding, 1)), cps({0x1F9D1, 0x1F3FB, 0x200D, 0x1F91D, 0x200D, 0x1F9D1, 0x1F3FB}));
    }

    void variationSelectorsAndTextDefault()
    {
        const int heart = emoji::find(kHeart);
        QVERIFY(heart >= 0);
        QCOMPARE(emoji::find(cps({0x2764})), heart); // without U+FE0F
        QCOMPARE(emoji::find(cps({0x2764, 0xFE0E})), heart);
        QCOMPARE(emoji::text(heart), kHeart);     // the fully qualified form
        QVERIFY(emoji::isTextDefault(heart));
        QVERIFY(!emoji::isTextDefault(emoji::find(kGrin)));
        QCOMPARE(emoji::lookupKey(kHeart), cps({0x2764}));
        QCOMPARE(emoji::find(QString()), -1);
        QCOMPARE(emoji::find(QStringLiteral("abc")), -1);
        QCOMPARE(emoji::find(QString(200, QChar(0x2764))), -1);
        QVERIFY(emoji::mayStart(0x1F600));
        QVERIFY(!emoji::mayStart('a'));
        QVERIFY(emoji::maxKeyUnits() >= 8);
    }

    void search()
    {
        QCOMPARE(emoji::search(QStringLiteral("joy")).value(0), emoji::find(kJoy));
        QCOMPARE(emoji::search(QStringLiteral(":joy:")).value(0), emoji::find(kJoy));
        QVERIFY(emoji::search(QStringLiteral("lol")).mid(0, 3).contains(emoji::find(kJoy)));
        QCOMPARE(emoji::search(QStringLiteral("+1")).value(0), emoji::find(kThumbs));
        QVERIFY(emoji::search(QStringLiteral("thumbs")).contains(emoji::find(kThumbs)));
        QVERIFY(emoji::search(QStringLiteral("Thumbs Up")).contains(emoji::find(kThumbs)));
        QVERIFY(emoji::search(QStringLiteral("heart")).contains(emoji::find(kHeart)));
        QVERIFY(emoji::search(QStringLiteral("fire")).mid(0, 3).contains(emoji::find(kFire)));
        QVERIFY(emoji::search(QStringLiteral("zzqqxx")).isEmpty());
        QVERIFY(emoji::search(QString()).isEmpty());
        QVERIFY(emoji::search(QStringLiteral("   ")).isEmpty());
        QCOMPARE(emoji::search(kGrin).value(0), emoji::find(kGrin)); // an emoji finds itself
        QVERIFY(emoji::search(QStringLiteral("face"), 10).size() == 10);
        for (int id : emoji::search(QStringLiteral("hand")))
            QCOMPARE(emoji::tone(id), 0); // no tone variants in results
    }

    void wireCodes()
    {
        QCOMPARE(emoji::wireCode(emoji::find(kThumbs)), QByteArray("1f44d"));
        QCOMPARE(emoji::wireCode(emoji::find(kHeart)), QByteArray("2764-fe0f"));
        QCOMPARE(emoji::fromWireCode("2764"), emoji::find(kHeart)); // unqualified is understood
        QVERIFY(emoji::isWireCode("1f468-200d-1f469-200d-1f467"));
        for (const QByteArray& bad : {QByteArray(), QByteArray("1F44D"), QByteArray("01f44d"), QByteArray("110000"), QByteArray("d800"),
                                      QByteArray("1f44d-"), QByteArray("-1f44d"), QByteArray("g1"), QByteArray("1f44d--fe0f"), QByteArray("1f44d.fe0f"),
                                      QByteArray("1234567"), QByteArray("1-2-3-4-5-6-7-8-9-a-b")}) {
            QVERIFY2(!emoji::isWireCode(bad), bad.constData());
            QCOMPARE(emoji::fromWireCode(bad), -1);
        }
        QCOMPARE(emoji::fromWireCode("41"), -1);    // valid form, not an emoji ("A")
        QCOMPARE(emoji::fromWireCode("1faff"), -1); // valid form, not in the table
        QVERIFY(emoji::wireCode(-1).isEmpty());
    }

    void teamSpeakEmoticons()
    {
        QCOMPARE(emoji::forTeamSpeakEmoticon(QStringLiteral("emoticons:smile.svg")), emoji::find(cps({0x1F642})));
        QCOMPARE(emoji::forTeamSpeakEmoticon(QStringLiteral("emoticons:SMILE.PNG")), emoji::find(cps({0x1F642})));
        QCOMPARE(emoji::forTeamSpeakEmoticon(QStringLiteral("emoticons:twinkle.svg")), emoji::find(cps({0x1F609})));
        QCOMPARE(emoji::forTeamSpeakEmoticon(QStringLiteral("emoticons:sub/laugh.svg")), emoji::find(cps({0x1F603})));
        QCOMPARE(emoji::forTeamSpeakEmoticon(QStringLiteral("emoticons:unknown.svg")), -1);
        QCOMPARE(emoji::forTeamSpeakEmoticon(QStringLiteral("iconpath:MESSAGE_INCOMING?size=13x13")), -1);
        QCOMPARE(emoji::forTeamSpeakEmoticon(QStringLiteral("tsmedia:0123456789abcdef0123")), -1);
        QCOMPARE(emoji::teamSpeakEmoticonCode(QStringLiteral("emoticons:skeptical.svg")), QStringLiteral(":/"));
        QCOMPARE(emoji::teamSpeakEmoticonCode(QStringLiteral("emoticons:tongue.png")), QStringLiteral(":P"));
        QVERIFY(emoji::teamSpeakEmoticonCode(QStringLiteral("emoticons:x.svg")).isEmpty());
        // Every emoticon of TeamSpeak's packs has its emoji in the table.
        for (const QString& file : emoji::teamSpeakEmoticonFiles())
            QVERIFY2(emoji::forTeamSpeakEmoticon(QStringLiteral("emoticons:") + file + QStringLiteral(".svg")) >= 0, qPrintable(file));
        QCOMPARE(emoji::teamSpeakEmoticonFiles().size(), 10);
    }

    // ---- segmentation --------------------------------------------------------------------------------
    void findsSimpleEmoji()
    {
        const QVector<emoji::Match> m = emoji::findEmoji(QStringLiteral("hi ") + kGrin + QStringLiteral(" there ") + kFire);
        QCOMPARE(m.size(), 2);
        QCOMPARE(m.at(0).start, 3);
        QCOMPARE(m.at(0).length, 2);
        QCOMPARE(m.at(0).id, emoji::find(kGrin));
        QCOMPARE(m.at(1).id, emoji::find(kFire));
        QVERIFY(emoji::findEmoji(QStringLiteral("plain text, no emoji at all: (c) 12 # * ")).isEmpty());
        QVERIFY(emoji::findEmoji(QString()).isEmpty());
    }

    void zwjSequencesAreOneCluster()
    {
        const QString family = cps({0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467});
        QVector<emoji::Match> m = emoji::findEmoji(QStringLiteral("a") + family + QStringLiteral("b"));
        QCOMPARE(m.size(), 1);
        QCOMPARE(m.at(0).start, 1);
        QCOMPARE(m.at(0).length, family.size());
        QCOMPARE(m.at(0).id, emoji::find(family));
        // An unknown combination falls back to its parts (the joiner is left as text).
        m = emoji::findEmoji(kGrin + cps({0x200D}) + kFire);
        QCOMPARE(m.size(), 2);
        QCOMPARE(m.at(0).id, emoji::find(kGrin));
        QCOMPARE(m.at(1).id, emoji::find(kFire));
        // Rainbow flag (with selectors inside).
        const QString rainbow = cps({0x1F3F3, 0xFE0F, 0x200D, 0x1F308});
        m                     = emoji::findEmoji(rainbow);
        QCOMPARE(m.size(), 1);
        QCOMPARE(m.at(0).length, rainbow.size());
        QVERIFY(m.at(0).selector);
        // ... and written without them.
        m = emoji::findEmoji(cps({0x1F3F3, 0x200D, 0x1F308}));
        QCOMPARE(m.size(), 1);
        QCOMPARE(m.at(0).id, emoji::find(rainbow));
    }

    void skinTonesStayWithTheirBase()
    {
        const QString medium = kThumbs + cps({0x1F3FD});
        QVector<emoji::Match> m = emoji::findEmoji(medium + medium);
        QCOMPARE(m.size(), 2);
        QCOMPARE(m.at(0).length, 4);
        QCOMPARE(m.at(0).id, emoji::withTone(emoji::find(kThumbs), 3));
        QCOMPARE(m.at(1).start, 4);
        // A tone on something that can't have one: the base, and the swatch on its own.
        m = emoji::findEmoji(kGrin + cps({0x1F3FD}));
        QCOMPARE(m.value(0).id, emoji::find(kGrin));
        QCOMPARE(m.value(0).length, 2);
    }

    void keycapsAndDigits()
    {
        const QString one = cps({'1', 0xFE0F, 0x20E3});
        QVector<emoji::Match> m = emoji::findEmoji(QStringLiteral("press ") + one);
        QCOMPARE(m.size(), 1);
        QCOMPARE(m.at(0).start, 6);
        QCOMPARE(m.at(0).length, 3);
        m = emoji::findEmoji(cps({'#', 0x20E3}));
        QCOMPARE(m.size(), 1);
        QCOMPARE(m.at(0).id, emoji::find(cps({'#', 0xFE0F, 0x20E3})));
        QVERIFY(emoji::findEmoji(QStringLiteral("1 10 100 #tag *star*")).isEmpty());
        QVERIFY(emoji::findEmoji(cps({'1', 0xFE0F})).isEmpty()); // a selector alone makes no keycap
    }

    void flagsNeverSplit()
    {
        // Regional indicator pairs are not in the table on Windows (no flag pictures): nothing matches,
        // and no single letter is ever taken out of a pair.
        const QString de = cps({0x1F1E9, 0x1F1EA});
        for (const emoji::Match& m : emoji::findEmoji(de + de))
            QCOMPARE(m.length, 4); // a whole pair if the table has it
        // A subdivision flag the font lacks: the black flag, its tags left as (invisible) text.
        const QString england = cps({0x1F3F4, 0xE0067, 0xE0062, 0xE0065, 0xE006E, 0xE0067, 0xE007F});
        const QVector<emoji::Match> m = emoji::findEmoji(england);
        QVERIFY(!m.isEmpty());
        QCOMPARE(m.at(0).start, 0);
        QVERIFY(m.at(0).id == emoji::find(england) || m.at(0).id == emoji::find(cps({0x1F3F4})));
    }

    void textPresentation()
    {
        // ❤ without a selector counts (people mean it); © doesn't; U+FE0E asks for text.
        QVector<emoji::Match> m = emoji::findEmoji(cps({0x2764}));
        QCOMPARE(m.size(), 1);
        QVERIFY(!m.at(0).selector);
        QVERIFY(emoji::showsAsPicture(m.at(0)));
        m = emoji::findEmoji(kHeart);
        QVERIFY(m.at(0).selector);
        QVERIFY(emoji::showsAsPicture(m.at(0)));
        QVERIFY(emoji::findEmoji(cps({0x2764, 0xFE0E})).isEmpty());
        const int copyright = emoji::find(cps({0xA9}));
        if (copyright >= 0) {
            m = emoji::findEmoji(QStringLiteral("(c) ") + cps({0xA9}) + QStringLiteral(" 2026"));
            QCOMPARE(m.size(), 1);
            QVERIFY(!emoji::showsAsPicture(m.at(0)));
            m = emoji::findEmoji(cps({0xA9, 0xFE0F}));
            QVERIFY(emoji::showsAsPicture(m.at(0)));
        }
        QVERIFY(emoji::showsAsPicture(emoji::findEmoji(kGrin).at(0)));
        QVERIFY(!emoji::showsAsPicture(emoji::Match()));
    }

    void hostileInput()
    {
        // Lone surrogates, joiners, selectors and tags without a base: no crash, nothing found.
        QString junk;
        junk += QChar(0xD83D);
        junk += QStringLiteral("x");
        junk += QChar(0xDE00);
        junk += cps({0x200D, 0x200D, 0xFE0F, 0x20E3, 0xE0067, 0xE007F, 0x1F3FB});
        QVERIFY(emoji::findEmoji(junk).isEmpty() || emoji::findEmoji(junk).first().id >= 0);
        // A long run of joiners after an emoji stays bounded.
        QString chain = kGrin;
        for (int i = 0; i < 5000; ++i)
            chain += cps({0x200D}) + kGrin;
        const QVector<emoji::Match> m = emoji::findEmoji(chain, 100000);
        QCOMPARE(m.size(), 5001);
        // At most maxMatches.
        QString many;
        for (int i = 0; i < 2000; ++i)
            many += kGrin;
        QCOMPARE(emoji::findEmoji(many, 1000).size(), 1000);
        QCOMPARE(emoji::findEmoji(many, 0).size(), 0);
    }

    void pictureOnlyCount()
    {
        QCOMPARE(emoji::pictureOnlyCount(kGrin + kGrin + QStringLiteral(" ") + kGrin, 27), 3);
        QCOMPARE(emoji::pictureOnlyCount(QStringLiteral(" ") + kGrin + QChar(QChar::LineSeparator) + kFire + QStringLiteral(" "), 27), 2);
        QCOMPARE(emoji::pictureOnlyCount(kGrin + QStringLiteral(" hi"), 27), -1);
        QCOMPARE(emoji::pictureOnlyCount(QString(), 27), 0);
        QCOMPARE(emoji::pictureOnlyCount(QStringLiteral("   "), 27), 0);
        QCOMPARE(emoji::pictureOnlyCount(cps({0x2764}), 27), 1);
        if (emoji::find(cps({0xA9})) >= 0)
            QCOMPARE(emoji::pictureOnlyCount(cps({0xA9}), 27), -1);
        QString thirty;
        for (int i = 0; i < 30; ++i)
            thirty += kJoy;
        QCOMPARE(emoji::pictureOnlyCount(thirty, 27), 28); // more than the limit: max + 1
    }

    // ---- the cache --------------------------------------------------------------------------------------
    void cacheBounds()
    {
        emoji::ImageCache cache(3, 1024 * 1024);
        const QImage      small(8, 8, QImage::Format_ARGB32_Premultiplied);
        cache.insert(QStringLiteral("a"), 8, small);
        cache.insert(QStringLiteral("b"), 8, small);
        cache.insert(QStringLiteral("c"), 8, small);
        QVERIFY(!cache.find(QStringLiteral("a"), 8).isNull()); // a is now the most recent
        cache.insert(QStringLiteral("d"), 8, small);           // b goes
        QCOMPARE(cache.entries(), 3);
        QVERIFY(cache.find(QStringLiteral("b"), 8).isNull());
        QVERIFY(!cache.find(QStringLiteral("a"), 8).isNull());
        QVERIFY(cache.find(QStringLiteral("a"), 16).isNull()); // sizes are separate
        // Bytes.
        const qint64 cost = emoji::ImageCache::costOf(small);
        cache.setLimits(100, cost * 2);
        QCOMPARE(cache.entries(), 2);
        QVERIFY(cache.bytes() <= cost * 2);
        // Replacing keeps one entry and its bytes right.
        cache.insert(QStringLiteral("a"), 8, small);
        cache.insert(QStringLiteral("a"), 8, small);
        QVERIFY(cache.bytes() <= cost * 2);
        // Larger than the whole cache: not kept, nothing evicted.
        const int before = cache.entries();
        cache.insert(QStringLiteral("huge"), 512, QImage(512, 512, QImage::Format_ARGB32_Premultiplied));
        QCOMPARE(cache.entries(), before);
        QVERIFY(cache.find(QStringLiteral("huge"), 512).isNull());
        cache.insert(QStringLiteral("null"), 8, QImage());
        QVERIFY(cache.find(QStringLiteral("null"), 8).isNull());
        QVERIFY(cache.hits() > 0 && cache.misses() > 0);
        cache.clear();
        QCOMPARE(cache.entries(), 0);
        QCOMPARE(cache.bytes(), qint64(0));
    }

    // ---- the renderer -----------------------------------------------------------------------------------
    void renderInColour()
    {
        emoji::shutdown();
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here (Segoe UI Emoji with colour glyphs)");
        QVERIFY(emoji::engineDescription().contains(QStringLiteral("DirectWrite")));
        const QImage image = emoji::render(kGrin, 32, 2.0);
        QCOMPARE(image.size(), QSize(64, 64));
        QCOMPARE(image.devicePixelRatio(), 2.0);
        QVERIFY(hasColourPixels(image));
        QCOMPARE(qAlpha(image.pixel(0, 0)), 0); // air around it
        QCOMPARE(qAlpha(image.pixel(63, 63)), 0);
        QVERIFY(qAlpha(image.pixel(32, 32)) > 200); // the face
        const emoji::CacheInfo before = emoji::cacheInfo();
        QVERIFY(!emoji::render(kGrin, 32, 2.0).isNull());
        QVERIFY(emoji::cacheInfo().hits > before.hits);
        // Sizes at fractional ratios round to whole device pixels.
        QCOMPARE(emoji::render(kGrin, 22, 1.5).size(), QSize(33, 33));
        // What Windows draws as one colour picture.
        QVERIFY(emoji::drawsInColor(kGrin));
        QVERIFY(emoji::drawsInColor(cps({0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467})));
        QVERIFY(emoji::drawsInColor(kThumbs + cps({0x1F3FD})));
        QVERIFY(!emoji::drawsInColor(QStringLiteral("A")));
        QVERIFY(!emoji::drawsInColor(kGrin + kGrin));             // two emoji
        QVERIFY(!emoji::drawsInColor(kGrin + cps({0x1F3FD})));    // a face can't take a tone
        QVERIFY(!emoji::drawsInColor(cps({0x1F1E9, 0x1F1EA})));   // no flag pictures on Windows
        QVERIFY(emoji::supported(emoji::find(kGrin)));
        QVERIFY(!emoji::supported(-1));
        // Bad requests.
        QVERIFY(emoji::render(QString(), 32, 1.0).isNull());
        QVERIFY(emoji::render(kGrin, 0, 1.0).isNull());
        QVERIFY(emoji::render(QString(100, QChar(0x2764)), 32, 1.0).isNull());
        QCOMPARE(emoji::render(kGrin, 10000, 1.0).width(), 512); // bounded
    }

    void renderOnTheWorker()
    {
        emoji::shutdown();
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here");
        QVERIFY(emoji::requestImage(kFire, 40, 1.0).isNull()); // queued
        QVERIFY(emoji::cacheInfo().pending >= 1);
        QSignalSpy ready(emoji::notifier(), &emoji::ImageNotifier::imagesReady);
        QVERIFY(ready.wait(5000));
        const QImage image = emoji::requestImage(kFire, 40, 1.0);
        QCOMPARE(image.size(), QSize(40, 40));
        QVERIFY(hasColourPixels(image));
        QCOMPARE(emoji::cacheInfo().pending, 0);
        // Shutting down with work queued joins the worker cleanly.
        QVector<int> some = emoji::members(emoji::Group::FoodDrink).mid(0, 50);
        emoji::prefetch(some, 32, 1.0);
        emoji::shutdown();
        QCOMPARE(emoji::cacheInfo().entries, 0);
    }

    void textEngineFallback()
    {
        emoji::shutdown();
        emoji::setColorEngineAllowed(false);
        QCOMPARE(emoji::engine(), emoji::Engine::Text);
        QVERIFY(!emoji::hasColor());
        QVERIFY(emoji::engineDescription().startsWith(QStringLiteral("QPainter")));
        const QImage image = emoji::render(kGrin, 24, 1.0);
        QCOMPARE(image.size(), QSize(24, 24));
        QVERIFY(!emoji::requestImage(kFire, 24, 1.0).isNull()); // drawn at once
        QVERIFY(emoji::supported(emoji::find(kGrin)));
        QVERIFY(!emoji::drawsInColor(kGrin));
        emoji::shutdown();
        emoji::setColorEngineAllowed(true);
        QVERIFY(emoji::engine() != emoji::Engine::None);
        emoji::shutdown();
    }
};

TSMEDIA_REGISTER_TEST(TestEmoji)

#include "tst_emoji.moc"
