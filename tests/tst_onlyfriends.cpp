// 2.7 F2: "Only from friends" and Blocked for automatic link previews (design 6.2, design_features 2). The one gate
// (contacts::allows: an unknown sender, the first read's moment, a list never read, a good list), why a held card is
// held (contacts::holdOf, linkstate::holdReason "not-friend" / "blocked", the card's tool tip first sentence), the
// sender as ChatLinkPreviews reads it (L4: a stranger whose message starts with a friend's client link stays a stranger),
// and the wiring in chatlinkpreviews.cpp that the plugin-only module can't run here (source checks: the gate at
// autoEligible, at the sink of automatic starts and in the cards; the cancel pass before restartWebCardsLoads, M1; the
// diagnostics never name or count people, L2), plus the QtSql rule of contacts.cpp (every call names its connection).

#include <QFile>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QtTest>

#include "contacts.h"
#include "linkpreview/linkcard.h"
#include "linkpreview/linkstate.h"
#include "testmain.h"
#include "textreactdoc.h"
#include "tschat.h"

namespace {

using contacts::Hold;
using contacts::Mark;
using contacts::State;

QString source(const char* relative)
{
    QFile f(QStringLiteral(TSMEDIA_SOURCE_DIR "/") + QLatin1String(relative));
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// The body of the function that starts with signature (to its first "\n}\n").
QString functionBody(const QString& file, const QString& signature)
{
    const int start = file.indexOf(signature);
    if (start < 0)
        return {};
    const int end = file.indexOf(QStringLiteral("\n}\n"), start);
    return end < 0 ? QString() : file.mid(start, end - start);
}

linkcard::Card clickCard(linkcard::AutoHold hold)
{
    linkcard::Card c;
    c.look     = linkcard::Look::Click;
    c.kind     = linkpreview::Kind::Image;
    c.cls      = linkpreview::LinkClass::Direct;
    c.host     = QStringLiteral("upload.wikimedia.org");
    c.autoHold = hold;
    return c;
}

} // namespace

class TestOnlyFriends : public QObject
{
    Q_OBJECT

  private slots:
    // The rule, for every state (design 6.2 and H5).
    void theGate()
    {
        // Read (Ok, and Stale: the last good list): Blocked never, with or without Only friends; Only friends: a Friend only.
        for (const State st : {State::Ok, State::Stale}) {
            QVERIFY(contacts::allows(st, Mark::None, false, true));
            QVERIFY(contacts::allows(st, Mark::Friend, false, true));
            QVERIFY(!contacts::allows(st, Mark::Blocked, false, true));
            QVERIFY(!contacts::allows(st, Mark::None, true, true));
            QVERIFY(contacts::allows(st, Mark::Friend, true, true));
            QVERIFY(!contacts::allows(st, Mark::Blocked, true, true));
            QVERIFY(!contacts::allows(st, Mark::Neutral, true, true));
        }
        // The first read's moment: everybody waits (with or without Only friends).
        QVERIFY(!contacts::allows(State::Checking, Mark::None, false, true));
        QVERIFY(!contacts::allows(State::Checking, Mark::Friend, true, true));
        // Never read (a file that can't be read, a TeamSpeak without Qt5Sql): 2.6's loads without Only friends, none with it.
        for (const State st : {State::Unreadable, State::Unavailable, State::Idle}) {
            QVERIFY(contacts::allows(st, Mark::None, false, true));
            QVERIFY(!contacts::allows(st, Mark::None, true, true));
            QVERIFY(!contacts::allows(st, Mark::Friend, true, true)); // nothing is known: nobody
        }
        // A header that can't be read, a ServerQuery login (no 28-character ID ever matches a contact): never a friend.
        QVERIFY(contacts::allows(State::Ok, Mark::None, false, false));
        QVERIFY(!contacts::allows(State::Ok, Mark::None, true, false));
        QVERIFY(contacts::allows(State::Checking, Mark::None, false, false));
    }

    void whyHeld()
    {
        QCOMPARE(contacts::holdOf(State::Ok, Mark::Friend, true, true), Hold::None);
        QCOMPARE(contacts::holdOf(State::Ok, Mark::None, true, true), Hold::NotFriend);
        QCOMPARE(contacts::holdOf(State::Ok, Mark::Blocked, false, true), Hold::Blocked);
        QCOMPARE(contacts::holdOf(State::Ok, Mark::Blocked, true, true), Hold::Blocked);
        QCOMPARE(contacts::holdOf(State::Stale, Mark::None, true, true), Hold::NotFriend);
        QCOMPARE(contacts::holdOf(State::Unreadable, Mark::None, true, true), Hold::Unreadable);
        QCOMPARE(contacts::holdOf(State::Unavailable, Mark::None, true, true), Hold::Unreadable);
        QCOMPARE(contacts::holdOf(State::Unavailable, Mark::None, false, true), Hold::None);
        QCOMPARE(contacts::holdOf(State::Checking, Mark::None, false, true), Hold::Checking);
        QCOMPARE(contacts::holdOf(State::Ok, Mark::None, true, false), Hold::NotFriend);

        // The held card's reason (diagnostics count it; the queue never starts it).
        linkstate::Decision d;
        d.look = linkcard::Look::Click;
        linkstate::LocalState idle;
        linkstate::Context    c;
        c.autoHold = linkcard::AutoHold::NotFriend;
        QCOMPARE(linkstate::holdReason(d, idle, c, true, true, true, linkpreview::HostGate::Verdict::Go), linkstate::HoldReason::NotFriend);
        QCOMPARE(QString::fromLatin1(linkstate::holdReasonName(linkstate::HoldReason::NotFriend)), QStringLiteral("not-friend"));
        c.autoHold = linkcard::AutoHold::Unreadable;
        QCOMPARE(linkstate::holdReason(d, idle, c, true, true, true, linkpreview::HostGate::Verdict::Go), linkstate::HoldReason::NotFriend);
        c.autoHold = linkcard::AutoHold::Blocked;
        QCOMPARE(linkstate::holdReason(d, idle, c, true, true, true, linkpreview::HostGate::Verdict::Go), linkstate::HoldReason::Blocked);
        QCOMPARE(QString::fromLatin1(linkstate::holdReasonName(linkstate::HoldReason::Blocked)), QStringLiteral("blocked"));
        c.autoHold = linkcard::AutoHold::Checking;
        QCOMPARE(linkstate::holdReason(d, idle, c, true, true, true, linkpreview::HostGate::Verdict::Go), linkstate::HoldReason::Waiting);
        // The setting off says Setting first (the contacts don't matter then).
        c.autoOn = false;
        c.autoHold = linkcard::AutoHold::Blocked;
        QCOMPARE(linkstate::holdReason(d, idle, c, true, true, true, linkpreview::HostGate::Verdict::Go), linkstate::HoldReason::Setting);
        // Not held: the 2.6 reasons.
        c.autoOn   = true;
        c.autoHold = linkcard::AutoHold::None;
        QCOMPARE(linkstate::holdReason(d, idle, c, false, true, true, linkpreview::HostGate::Verdict::Go), linkstate::HoldReason::Offscreen);
    }

    // The card's tool tip says why first, then whom a click contacts (design 4.4 texts).
    void cardToolTips()
    {
        const QString plain = linkcard::toolTip(clickCard(linkcard::AutoHold::None), linkcard::Zone::Load);
        QVERIFY(plain.startsWith(QLatin1String("Loading the preview connects your PC to upload.wikimedia.org")));
        const QString notFriend = linkcard::toolTip(clickCard(linkcard::AutoHold::NotFriend), linkcard::Zone::Load);
        QCOMPARE(notFriend, QStringLiteral("Loads by itself only for your TeamSpeak friends. ") + plain);
        const QString blocked = linkcard::toolTip(clickCard(linkcard::AutoHold::Blocked), linkcard::Zone::Body);
        QCOMPARE(blocked, QStringLiteral("You blocked this person in TeamSpeak: their links never load by themselves. ") + plain);
        const QString unreadable = linkcard::toolTip(clickCard(linkcard::AutoHold::Unreadable), linkcard::Zone::Load);
        QCOMPARE(unreadable, QStringLiteral("TS Media couldn't read your TeamSpeak contacts. Click to load. ") + plain);
        QCOMPARE(linkcard::toolTip(clickCard(linkcard::AutoHold::Checking), linkcard::Zone::Load), plain);
        // Above the automatic limit too.
        linkcard::Card big = clickCard(linkcard::AutoHold::NotFriend);
        big.size           = 38 * 1024 * 1024;
        QVERIFY(linkcard::toolTip(big, linkcard::Zone::Load).startsWith(QLatin1String("Loads by itself only for your TeamSpeak friends. Larger than")));
        // The hide button keeps its own tip.
        QCOMPARE(linkcard::toolTip(clickCard(linkcard::AutoHold::Blocked), linkcard::Zone::Hide), QStringLiteral("Hide preview"));
    }

    // L4: the sender is the message header's client link (the way ChatLinkPreviews reads it: textreactdoc::identify), so a
    // stranger whose message starts with a friend's client link and a picture link is the stranger, and stays held.
    void spoofedHeaderStaysHeld()
    {
        const tschat::Skin s       = tschat::darkSkin();
        const QString      friendU = QStringLiteral("Zm9vYmFyYmF6cXV4cXV1eHh4eHg=");
        const QString      stranger = QStringLiteral("c3RyYW5nZXJzdHJhbmdlcnN0cmE=");
        QTextDocument      doc;
        const QString body = tschat::clientHtml(s, 4, friendU, QStringLiteral("Alice")) + QStringLiteral(" ")
                             + tschat::linkHtml(s, QStringLiteral("https://upload.wikimedia.org/wikipedia/commons/a/a9/Example.jpg"));
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("12:00:01"), 9, stranger, QStringLiteral("Mallory"), body));
        const textreactdoc::Identity id = textreactdoc::identify(doc.lastBlock());
        QVERIFY(id.message);
        QCOMPARE(id.uid, stranger);
        contacts::Lists lists;
        lists.friends.insert(friendU);
        QVERIFY(!contacts::allows(State::Ok, lists.friends.contains(id.uid) ? Mark::Friend : Mark::None, true, !id.uid.isEmpty()));
        // The friend's own message does load.
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("12:00:02"), 4, friendU, QStringLiteral("Alice"),
                                                 tschat::linkHtml(s, QStringLiteral("https://upload.wikimedia.org/x.jpg"))));
        const textreactdoc::Identity real = textreactdoc::identify(doc.lastBlock());
        QCOMPARE(real.uid, friendU);
        QVERIFY(contacts::allows(State::Ok, lists.friends.contains(real.uid) ? Mark::Friend : Mark::None, true, true));
        // A ServerQuery sender: its login is no unique ID, never a contact.
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("12:00:03"), 5, QStringLiteral("serveradmin"), QStringLiteral("QueryBob"),
                                                 tschat::linkHtml(s, QStringLiteral("https://upload.wikimedia.org/y.jpg"))));
        QVERIFY(!contacts::parse({QStringLiteral("Friend=0\nIDS=serveradmin")}).friends.contains(textreactdoc::identify(doc.lastBlock()).uid));
    }

    // The wiring in the plugin-only ChatLinkPreviews (M1, design 6.2): one gate everywhere, and the order of settingsChanged.
    void wiringInChatLinkPreviews()
    {
        const QString clp = source("src/chatlinkpreviews.cpp");
        QVERIFY(!clp.isEmpty());
        // autoEligible: the gate (your own links pass).
        const QString eligible = functionBody(clp, QStringLiteral("bool ChatLinkPreviews::autoEligible("));
        QVERIFY(eligible.contains(QLatin1String("if (!contactAllows(ref.senderUid) && !isOwn(ref, ctx))")));
        // The sink: an automatic start is refused for a sender the contacts don't allow; the run keeps the sender's ID.
        const QString start = functionBody(clp, QStringLiteral("void ChatLinkPreviews::startFetch("));
        QVERIFY(start.contains(QLatin1String("if (origin == Origin::Auto && !own && !contactAllows(senderUid))")));
        QVERIFY(start.contains(QLatin1String("run.senderUid  = senderUid;")));
        QVERIFY(start.indexOf(QLatin1String("!contactAllows(senderUid)")) < start.indexOf(QLatin1String("m_loader(target, caps, dir)")));
        // Every automatic start passes the sender's ID.
        const QRegularExpression autoStart(QStringLiteral("startFetch\\(([^;]*)Origin::Auto([^;]*)\\);"));
        int                      starts = 0;
        for (auto it = autoStart.globalMatch(clp); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            ++starts;
            QVERIFY2(m.captured(0).contains(QLatin1String("senderUid")), qPrintable(m.captured(0)));
        }
        QCOMPARE(starts, 2); // runAutoLoads and restartWebCardsLoads
        // settingsChanged: the contacts follow the switch, and the cancel pass runs before any load starts again (M1).
        const QString changed = functionBody(clp, QStringLiteral("void ChatLinkPreviews::settingsChanged()"));
        QVERIFY(changed.contains(QLatin1String("m_contacts->setActive(enabled() && s.autoLoadLinkPreviews)")));
        const int cancel  = changed.indexOf(QLatin1String("cancelDisallowedAutoRuns();"));
        const int restart = changed.indexOf(QLatin1String("restartWebCardsLoads();"));
        QVERIFY(cancel > 0 && restart > 0 && cancel < restart);
        // The cancel pass spares clicked loads and our own.
        const QString pass = functionBody(clp, QStringLiteral("void ChatLinkPreviews::cancelDisallowedAutoRuns()"));
        QVERIFY(pass.contains(QLatin1String("run.clicked || run.own || contactAllows(run.senderUid)")));
        // A changed list: the same pass, then the cards again.
        const QString contactsChanged = functionBody(clp, QStringLiteral("void ChatLinkPreviews::contactsChanged()"));
        QVERIFY(contactsChanged.contains(QLatin1String("cancelDisallowedAutoRuns();")) && contactsChanged.contains(QLatin1String("scheduleRefresh();")));
        // The card says why (contextFor -> the Click card).
        QVERIFY(functionBody(clp, QStringLiteral("linkstate::Context ChatLinkPreviews::contextFor(")).contains(QLatin1String("c.autoHold = contactHold(ref.senderUid);")));
        QVERIFY(functionBody(clp, QStringLiteral("linkcard::Card ChatLinkPreviews::cardFor(")).contains(QLatin1String("c.autoHold      = contextFor(ref).autoHold;")));
        // L2: the diagnostics say a state word and one combined count, never "blocked N", never an ID.
        const QString diag = functionBody(clp, QStringLiteral("QStringList ChatLinkPreviews::diagnosticLines() const"));
        QVERIFY(diag.contains(QLatin1String("held by your contacts setting %6")));
        QVERIFY(diag.contains(QLatin1String("Contacts list: %1")));
        QVERIFY(diag.contains(QLatin1String("only from friends: %4")));
        QVERIFY(!diag.contains(QLatin1String("blocked %")));
        QVERIFY(!diag.contains(QLatin1String("markOf")));
        // Unload: the reader stops with the fetches.
        QVERIFY(functionBody(clp, QStringLiteral("void ChatLinkPreviews::prepareShutdown()")).contains(QLatin1String("m_contacts->shutdown();")));
    }

    // H4 / ts_facts 3.5 in the reader's source: every QtSql call names its connection (a default argument reads the data
    // export defaultConnection, which a delay-loaded Qt5Sql can't give: the link fails, LNK1194), read-only options, no
    // write, no transaction.
    void readerSourceRules()
    {
        const QString c = source("src/contacts.cpp");
        QVERIFY(!c.isEmpty());
        QVERIFY(c.contains(QLatin1String("QSqlDatabase::addDatabase(QString::fromLatin1(\"QSQLITE\"), name)")));
        QVERIFY(c.contains(QLatin1String("QSqlDatabase::contains(name)")));
        QVERIFY(c.contains(QLatin1String("QSqlDatabase::removeDatabase(name)")));
        QVERIFY(c.contains(QLatin1String("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=100")));
        QVERIFY(!c.contains(QLatin1String("QSqlDatabase::database(")));
        QVERIFY(!c.contains(QLatin1String("defaultConnection")));
        QVERIFY(!c.contains(QLatin1String("QStringLiteral")));
        for (const char* word : {"INSERT", "UPDATE", "DELETE", "BEGIN", "PRAGMA", "transaction()", "commit()"})
            QVERIFY2(!c.contains(QLatin1String(word)), word);
        QVERIFY(c.contains(QLatin1String("delayload::bindQtSql()")));
        // Nothing of a contact is ever logged.
        QVERIFY(!c.contains(QLatin1String("ts3::log")) && !c.contains(QLatin1String("qDebug")) && !c.contains(QLatin1String("qWarning")));
    }
};

TSMEDIA_REGISTER_TEST(TestOnlyFriends)

#include "tst_onlyfriends.moc"
