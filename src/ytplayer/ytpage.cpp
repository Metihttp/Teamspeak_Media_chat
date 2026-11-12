#include "ytplayer/ytpage.h"

#include <QRegularExpression>
#include <QUrlQuery>
#include <QtMath>

#include "i18n.h"
#include "tsmpmsg.h"

namespace ytpage {

namespace {

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

} // namespace

bool validId(const QString& id)
{
    if (id.size() != 11)
        return false;
    for (const QChar c : id) {
        const ushort u = c.unicode();
        const bool   ok = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '_' || u == '-';
        if (!ok)
            return false;
    }
    return true;
}

int parseStart(const QString& value)
{
    // "123" / "123s" / "1h2m3s" / "2m" / "1h": whole parts only, in that order, at most 24 h.
    static const QRegularExpression plain(latin("^([0-9]{1,6})s?$"));
    static const QRegularExpression parts(latin("^(?:([0-9]{1,3})h)?(?:([0-9]{1,4})m)?(?:([0-9]{1,6})s)?$"));
    const QString v = value.trimmed();
    if (v.isEmpty() || v.size() > 16)
        return 0;
    qint64                       seconds = -1;
    const QRegularExpressionMatch p      = plain.match(v);
    if (p.hasMatch()) {
        seconds = p.captured(1).toLongLong();
    } else {
        const QRegularExpressionMatch m = parts.match(v);
        if (!m.hasMatch())
            return 0;
        seconds = m.captured(1).toLongLong() * 3600 + m.captured(2).toLongLong() * 60 + m.captured(3).toLongLong();
    }
    if (seconds < 0 || seconds > kMaxStartSec)
        return 0;
    return static_cast<int>(seconds);
}

int startOf(const QUrl& link)
{
    for (const QString& part : {link.query(QUrl::FullyDecoded), link.fragment(QUrl::FullyDecoded)}) {
        const QUrlQuery q(part);
        for (const char* name : {"t", "start"}) {
            const QString value = q.queryItemValue(latin(name), QUrl::FullyDecoded);
            if (!value.isEmpty())
                return parseStart(value);
        }
    }
    return 0;
}

QString pageHtml(const QString& id, int startSec, bool popup, const QString& testApiUrl, bool muted)
{
    Q_UNUSED(popup); // the same document for both windows in 2.5.0
    if (!validId(id))
        return {};
    const int     start  = qBound(0, startSec, kMaxStartSec);
    const bool    test   = !testApiUrl.isEmpty();
    const QString origin = latin(kAppOrigin);
    QString       html;
    html += latin("<!doctype html><html><head><meta charset=\"utf-8\">\n");
    html += latin("<meta name=\"referrer\" content=\"strict-origin-when-cross-origin\">\n");
    html += latin("<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; script-src 'unsafe-inline' https://www.youtube.com")
            + (test ? latin(" ") + origin : QString())
            + latin("; frame-src https://www.youtube-nocookie.com; style-src 'unsafe-inline'\">\n");
    html += latin("<style>html,body{margin:0;height:100%;background:#000;overflow:hidden}iframe{border:0;width:100%;height:100%}</style>\n");
    html += latin("</head><body>\n");
    html += latin("<iframe id=\"p\" allow=\"autoplay; encrypted-media\"\n");
    html += latin("  src=\"https://www.youtube-nocookie.com/embed/") + id
            + latin("?enablejsapi=1&origin=https%3A%2F%2Fio.github.metihttp.tsmediachat&autoplay=1&rel=0&fs=0&start=") + QString::number(start)
            + (muted ? latin("&mute=1") : QString()) // TS Media's session mute: YouTube starts muted (review fix)
            + latin("\"></iframe>\n");
    html += latin("<script>\n"
                  "var p = null;\n"
                  "function tell(s) { window.chrome.webview.postMessage(s); }\n"
                  "function now() { if (p && p.getCurrentTime) tell('time:' + Math.floor(p.getCurrentTime())); }\n"
                  "function onYouTubeIframeAPIReady() {\n"
                  "  p = new YT.Player('p', { events: {\n"
                  "    onReady: function () { tell('ready'); },\n"
                  "    onStateChange: function (e) { tell('state:' + e.data); now(); },\n"
                  "    onError: function (e) { tell('error:' + e.data); } } });\n"
                  "  setInterval(function () { if (p.getPlayerState && p.getPlayerState() === 1) now(); }, 5000);\n"
                  "}\n"
                  "window.chrome.webview.addEventListener('message', function (e) {\n"
                  "  var c = String(e.data);\n"
                  "  if (!p || !p.pauseVideo) return;\n"
                  "  if (c === 'pause') p.pauseVideo(); else if (c === 'play') p.playVideo();\n"
                  "  else if (c === 'mute') p.mute(); else if (c === 'unmute') p.unMute();\n"
                  "  else if (/^volume:\\d{1,3}$/.test(c)) p.setVolume(Math.min(100, parseInt(c.slice(7), 10)));\n"
                  "});\n"
                  "</script>\n");
    html += latin("<script src=\"") + (test ? testApiUrl.toHtmlEscaped() : latin("https://www.youtube.com/iframe_api")) + latin("\"></script>\n");
    html += latin("</body></html>\n");
    return html;
}

Message parseMessage(const QString& text, bool allowTest)
{
    const std::wstring     w = text.toStdWString();
    const tsmpmsg::Message m = tsmpmsg::parse(w.c_str(), w.size(), allowTest);
    Message                out;
    out.kind  = static_cast<int>(m.kind);
    out.value = m.value;
    return out;
}

bool validCommand(const QString& command)
{
    const std::wstring w = command.toStdWString();
    return tsmpmsg::validCommand(w.c_str(), w.size());
}

double cssScale(double s, double dpr)
{
    const double v = (s > 0 ? s : 1.0) * (dpr > 0 ? dpr : 1.0);
    return qBound(0.25, v, 8.0);
}

bool canPlayInPlace(bool gate, const QSizeF& thumbDu, double s, double sysScale)
{
    if (!gate)
        return false;
    if (thumbDu.width() + 1e-6 < kMinWidthDu || thumbDu.height() + 1e-6 < kMinHeightDu)
        return false;
    return s + 1e-6 >= 0.9 * (sysScale > 0 ? sysScale : 1.0);
}

double visibleFraction(const QRect& rect, const QRect& clip)
{
    if (rect.isEmpty())
        return 0.0;
    const QRect  shown = rect.intersected(clip);
    const double whole = static_cast<double>(rect.width()) * rect.height();
    return shown.isEmpty() ? 0.0 : (static_cast<double>(shown.width()) * shown.height()) / whole;
}

QSize popupSizeDu(const QString& setting)
{
    static const QRegularExpression wxh(latin("^([0-9]{1,5})x([0-9]{1,5})$"));
    const QRegularExpressionMatch   m = wxh.match(setting.trimmed());
    if (!m.hasMatch())
        return QSize(kPopupWidthDu, kPopupHeightDu);
    return QSize(qBound(kMinWidthDu, m.captured(1).toInt(), 4096), qBound(kMinHeightDu, m.captured(2).toInt(), 4096));
}

QString popupSizeText(const QSize& du)
{
    return QString::number(du.width()) + QLatin1Char('x') + QString::number(du.height());
}

QRect popupRect(const QSize& du, double s, double dpr, const QRect& owner)
{
    const double f = (s > 0 ? s : 1.0) * (dpr > 0 ? dpr : 1.0);
    const QSize  device(qRound(du.width() * f), qRound(du.height() * f));
    if (!owner.isValid())
        return QRect(QPoint(64, 64), device);
    return QRect(QPoint(owner.left() + (owner.width() - device.width()) / 2, owner.top() + (owner.height() - device.height()) / 2), device);
}

QSize sizeDuFromDevice(const QString& wxh, double s, double dpr)
{
    static const QRegularExpression size(latin("^([0-9]{1,5})x([0-9]{1,5})$"));
    const QRegularExpressionMatch   m = size.match(wxh);
    if (!m.hasMatch())
        return {};
    const double f = (s > 0 ? s : 1.0) * (dpr > 0 ? dpr : 1.0);
    return QSize(qBound(kMinWidthDu, qRound(m.captured(1).toInt() / f), 4096), qBound(kMinHeightDu, qRound(m.captured(2).toInt() / f), 4096));
}

Note noteFor(int error)
{
    switch (error) {
    case 101:
    case 150:
        return Note::OnlyOnYouTube;
    case 100:
        return Note::Unavailable;
    default:
        return Note::CantPlay;
    }
}

bool rememberedError(int error)
{
    return error == 100 || error == 101 || error == 150;
}

QString noteText(Note note)
{
    switch (note) {
    case Note::OnlyOnYouTube:
        return i18n::t("This video can only be watched on YouTube");
    case Note::Unavailable:
        return i18n::t("Video unavailable");
    case Note::CantPlay:
        return i18n::t("Can't play this video here");
    case Note::None:
        break;
    }
    return {};
}

QString shortNoteText(const QString& note)
{
    if (note == noteText(Note::OnlyOnYouTube))
        return i18n::t("Only on YouTube");
    if (note == noteText(Note::CantPlay))
        return i18n::t("Can't play it here");
    if (note == noteText(Note::Unavailable))
        return note;
    return {};
}

QString timeText(qint64 ms)
{
    const qint64 total = qMax<qint64>(0, ms / 1000);
    const qint64 h = total / 3600, m = (total / 60) % 60, s = total % 60;
    const QString ss = QString::number(s).rightJustified(2, QLatin1Char('0'));
    if (h > 0)
        return QString::number(h) + QLatin1Char(':') + QString::number(m).rightJustified(2, QLatin1Char('0')) + QLatin1Char(':') + ss;
    return QString::number(m) + QLatin1Char(':') + ss;
}

QUrl watchUrl(const QString& id, int startSec)
{
    if (!validId(id))
        return {};
    QString text = latin("https://www.youtube.com/watch?v=") + id;
    if (startSec > 0 && startSec <= kMaxStartSec)
        text += latin("&t=") + QString::number(startSec) + QLatin1Char('s');
    return QUrl(text);
}

QString spikePage()
{
    return latin("<!doctype html><html><head><meta charset=\"utf-8\">\n"
                 "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'\">\n"
                 "<style>html,body{margin:0;height:100%;background:#202225;color:#fff;font:16px 'Segoe UI',sans-serif}"
                 "#b{margin:24px;width:240px;height:96px;background:#5865f2;border-radius:8px;display:flex;align-items:center;justify-content:center}"
                 "</style></head><body>\n"
                 "<p style=\"margin:24px\">TS Media player spike: click the box, then press Esc.</p>\n"
                 "<div id=\"b\">Click me</div>\n"
                 "<script>document.getElementById('b').addEventListener('click', function () { this.style.background = '#3ba55d';"
                 " this.textContent = 'Clicked'; window.chrome.webview.postMessage('ready'); });</script>\n"
                 "</body></html>\n");
}

// The texts that say where the player shows follow the release gate (2.5.0: always the popup), so they can't drift.
QString consentQuestion()
{
    if constexpr (kAnchoredPlayer)
        return i18n::t("Play this video here?");
    else
        return i18n::t("Play this video in a window?");
}

QString consentLine()
{
    return i18n::t("YouTube's own player. YouTube and Google see your IP address and this video; ads may play.");
}

QString consentNotice()
{
    // design2 3.10's approved notice; only where the player shows follows the gate (the popup in 2.5.0).
    if constexpr (kAnchoredPlayer)
        return i18n::t("TS Media plays this video with YouTube's own player, right in the chat, in YouTube's privacy-enhanced mode (YouTube "
                       "describes that mode for websites; TS Media is an app). The player loads nothing from YouTube before you press Play. Then "
                       "your PC connects to YouTube and Google: they see your IP address and which video you watch, and ads may play. Microsoft "
                       "Edge WebView2, which runs the player, may also contact Microsoft. By playing videos here you agree to the YouTube Terms "
                       "of Service; the Google Privacy Policy applies. You can turn this off under Settings \xE2\x86\x92 Privacy & updates "
                       "\xE2\x86\x92 Link previews.");
    else
        return i18n::t("TS Media plays this video with YouTube's own player, in a window of its own over TeamSpeak, in YouTube's "
                       "privacy-enhanced mode (YouTube describes that mode for websites; TS Media is an app). The player loads nothing from "
                       "YouTube before you press Play. Then your PC connects to YouTube and Google: they see your IP address and which video you "
                       "watch, and ads may play. Microsoft Edge WebView2, which runs the player, may also contact Microsoft. By playing videos "
                       "here you agree to the YouTube Terms of Service; the Google Privacy Policy applies. You can turn this off under Settings "
                       "\xE2\x86\x92 Privacy & updates \xE2\x86\x92 Link previews.");
}

QString termsText()
{
    return i18n::t("YouTube Terms of Service");
}

QString privacyText()
{
    return i18n::t("Google Privacy Policy");
}

QUrl termsUrl()
{
    return QUrl(latin("https://www.youtube.com/t/terms"));
}

QUrl privacyUrl()
{
    return QUrl(latin("https://policies.google.com/privacy"));
}

} // namespace ytpage
