// 2.7 Part B: the live-test driver's commands for Only from friends (F2), the reply widgets on 4K (F4) and the chat's
// colours (F6). TESTHOOKS builds only (CMakeLists.txt); dispatched from SelfTest::run (selftest.cpp).
//   contacts.mark {uid}   friend | blocked | none for one unique ID the script passes (never a list, never a name)
//   contacts.state        the contacts list's state word, how many reads, the last read's time
//   chat.colors           the visible chat's effective base, text, link, dark, sampled, and TS Media's line colours
//   reply.bar             the reply bar's font (px), size hint and scale; the open replies list's font and size

#include <QApplication>
#include <QFontInfo>
#include <QJsonObject>
#include <QTextBrowser>
#include <QWidget>

#include <functional>

#include "chatcolors.h"
#include "chatintegration.h"
#include "chatlinkpreviews.h"
#include "contacts.h"
#include "replybar.h"
#include "repliespopup.h"
#include "ts3api.h"

namespace {

QString yesNo(bool v)
{
    return v ? QString::fromLatin1("yes") : QString::fromLatin1("no");
}

} // namespace

bool selfTestV27b(const QString& cmd, const QJsonObject& c, ChatIntegration* chat, const std::function<void(const QString&)>& say)
{
    if (cmd == QLatin1String("contacts.mark") || cmd == QLatin1String("contacts.state")) {
        ContactList* list = chat && chat->linkPreviews() ? chat->linkPreviews()->contacts() : nullptr;
        if (!list) {
            say(QString::fromLatin1("%1: no contacts list").arg(cmd));
            return true;
        }
        if (cmd == QLatin1String("contacts.state")) {
            say(QString::fromLatin1("contacts.state: %1, active %2, reads %3, last read %4 ms")
                    .arg(contacts::stateWord(list->state()), yesNo(list->active()))
                    .arg(list->reads())
                    .arg(list->lastReadMs()));
            return true;
        }
        const QString uid = c.value(QString::fromLatin1("uid")).toString();
        QString       mark;
        switch (list->markOf(uid)) {
        case contacts::Mark::Friend:
            mark = QString::fromLatin1("friend");
            break;
        case contacts::Mark::Blocked:
            mark = QString::fromLatin1("blocked");
            break;
        case contacts::Mark::None:
        case contacts::Mark::Neutral:
            mark = QString::fromLatin1("none");
            break;
        }
        say(QString::fromLatin1("contacts.mark: %1").arg(mark)); // the answer only: the UID is the script's own
        return true;
    }
    if (cmd == QLatin1String("chat.colors")) {
        if (!chat) {
            say(QString::fromLatin1("chat.colors: no chat integration"));
            return true;
        }
        const chatcolors::Colors     colors = chat->visibleChatColors();
        const chatcolors::LineColors lines  = ts3::lineColors();
        say(QString::fromLatin1("chat.colors: base %1 text %2 link %3 dark %4 sampled %5; lines accent %6 warning %7 body %8 wrap %9; zoom %10 (%11%)")
                .arg(colors.base.name(), colors.text.name(), colors.link.name(), yesNo(colors.dark), yesNo(colors.sampled), lines.accent, lines.warning,
                     lines.body, yesNo(lines.wrapBody))
                .arg(chat->chatZoom())
                .arg(chat->chatZoomPercent()));
        return true;
    }
    if (cmd == QLatin1String("reply.bar")) {
        QStringList parts;
        for (QWidget* w : QApplication::allWidgets()) {
            if (auto* bar = qobject_cast<ReplyBar*>(w); bar && bar->objectName() == QLatin1String("tsmediaReplyBar")) {
                parts << QString::fromLatin1("bar %1: font %2 px, size hint %3x%4, height %5, scale %6")
                             .arg(bar->isVisible() ? QString::fromLatin1("shown") : QString::fromLatin1("hidden"))
                             .arg(QFontInfo(bar->font()).pixelSize())
                             .arg(bar->sizeHint().width())
                             .arg(bar->sizeHint().height())
                             .arg(bar->height())
                             .arg(bar->scale(), 0, 'f', 2);
            } else if (auto* popup = qobject_cast<RepliesPopup*>(w); popup && popup->isVisible()) {
                parts << QString::fromLatin1("replies list: font %1 px, size %2x%3, scale %4")
                             .arg(QFontInfo(popup->font()).pixelSize())
                             .arg(popup->width())
                             .arg(popup->height())
                             .arg(popup->scale(), 0, 'f', 2);
            }
        }
        say(QString::fromLatin1("reply.bar: ") + (parts.isEmpty() ? QString::fromLatin1("none") : parts.join(QString::fromLatin1("; "))));
        return true;
    }
    return false;
}
