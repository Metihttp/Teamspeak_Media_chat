#pragma once

// 2.5 link previews: minimal synthesized pages, oEmbed answers, feeds, manifests and media bytes for the unit tests
// (tst_pagemeta, tst_linkpage_net, tst_safeimage). Each mirrors the meta structure the research saw on the real
// site (research_media §25, research_cards §9, research_threats §7); large fillers are generated in code. None is
// a copy of a real page. Plus a scripted HTTP transport (FakeHttp) that records every request.

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

#include "linkpreview/transport.h"

namespace linkfixtures {

// ---- pages ------------------------------------------------------------------------------------------------------

QByteArray tenorView(const QString& id);
QByteArray tenorOEmbed(const QString& mediaId);      // thumbnail_url https://media.tenor.com/<mediaId>AAAAe/x.png
QByteArray giphyGif(const QString& id);
QByteArray imgurAlbum(bool videoCover);              // twitter:player:stream .mp4 (or only og:image)
QByteArray imgurPostMismatch();                      // og:video .gif typed video/mp4, twitter:player:stream .mp4
QByteArray imgurShell();                             // only og:site_name
QByteArray klipyClip();                              // og:video typed video/mp4 on static.klipy.com
QByteArray klipySticker();                           // og:image (a GIF) on static.klipy.com
QByteArray klipyChallenge();                         // the body of a Cloudflare challenge ("Just a moment...")
// api.klipy.com's oEmbed answer for a GIF page (its shape on 2026-11-11), thumbnail_url as written (JSON-escaped); the
// answer of that day: "https:\/\/static2.klipy.com\/".
QByteArray klipyOEmbed(const QByteArray& thumbnailUrl);
QByteArray kickClip();                               // </head> at about 8.7 KB, 260 KB of body filler, then og tags
QByteArray kickChannel();
enum class BlueskyKind { Image, Video, Text, OptOut };
QByteArray blueskyPost(BlueskyKind kind);            // a 25 KB inline script first
QByteArray twitchShell();                            // the site-wide og (og:title "Twitch", ttv-static-metadata image)
QByteArray twitchClip();
QByteArray twitchChannel();
enum class RedditLink { IRedd, VRedd, Imgur, External };
QByteArray redditRss(RedditLink link);
QByteArray vredditMpd(bool audio, bool cmaf);
QByteArray pinterestOEmbed();
QByteArray gyazoOEmbed(bool video);
QByteArray vimeoOEmbed();
QByteArray soundcloudOEmbed();                       // description with &nbsp; and <a href>
QByteArray spotifyTrack();
QByteArray appleOEmbed();
QByteArray deezerOEmbed();
QByteArray dailymotionOEmbed();
QByteArray youtubeOEmbed();
QByteArray medalOEmbed();
QByteArray xStatus(bool largeImage);                 // light/dark theme-color pair
QByteArray wikiSummary(bool rtlZwnj);
QByteArray discordInvite(bool splash, bool nsfw);
QByteArray pixeldrainInfo(const QString& mime, qint64 size);
QByteArray telegramPost();
QByteArray steamStore();
QByteArray steamScreenshot();
QByteArray steamAgecheck();
QByteArray githubRepo();                             // og:title after 25 KB
QByteArray cnnLike();                                // og:title after 240 KB, og:site_name after 420 KB
QByteArray teamspeakHome();                          // relative og:image
QByteArray discourseSvg();                           // og:image .svg, twitter:image .png
QByteArray shiftJisPage();                           // charset only in <meta>, a Shift_JIS title
QByteArray cfChallenge403();
QByteArray clientChallenge200();
QByteArray consentPage();                            // where consentRedirect() leads: robots noindex, <title>Golem</title>
QByteArray mastodon404WithOg();
QByteArray twitchVodBoilerplate();
QByteArray titleOnly();
QByteArray webCard(const QString& title, const QString& description, const QString& image, const QString& extraHead = QString());
QByteArray makeagifPage();
QByteArray lightshotPage();
QByteArray tumblrPost(int kind); // 0 video, 1 image, 2 text

// The code points as UTF-8 (fixtures and tests never hold invisible characters in their source).
QByteArray utf8(std::initializer_list<uint> codes);
QString    text(std::initializer_list<uint> codes);

// ---- media bytes ----------------------------------------------------------------------------------------------

QByteArray png(int width, int height, bool alpha = false);
QByteArray jpeg(int width, int height);
QByteArray gif(int frames); // 1 or more frames of 4x4
QByteArray webpLossy();     // a structurally valid lossy WebP ("VP8 "), from Qt's writer when it has one
QByteArray webpLossless();  // "VP8L"
QByteArray webpAlphLossless(); // VP8X + ALPH (lossless alpha) + VP8
QByteArray webpAnimatedLosslessFrame(); // VP8X + ANIM + ANMF(VP8) + ANMF(VP8L)
QByteArray mp4Header(); // enough for content::sniff (ftyp isom)
QByteArray svgAsJpg();  // an SVG with an <image> pointing at a local share, saved under a .jpg name
QByteArray xpmImage();

// ---- a scripted transport ---------------------------------------------------------------------------------------

struct Reply {
    int        status = 200;
    QByteArray contentType;
    qint64     contentLength = -2; // -2: the body's size; -1: not sent
    QString    location;
    QByteArray body;
    QByteArray contentEncoding;
    qint64     retryAfterMs  = -1;
    double     rateRemaining = -1;
    qint64     rateResetMs   = -1;
    bool       challenge     = false;
    int        chunk         = 16 * 1024;
    int        delayMs       = 0;      // before the head
    int        stallAfterBytes = -1;   // then wait for the cancel or the deadline
    int        dripMs          = 0;    // wait this long before each chunk (a slow page)
    QString    peer;                   // the connected address; empty: 93.184.216.34
};

class FakeHttp : public linkpreview::http::Transport
{
  public:
    struct Request {
        QString    url;
        QByteArray accept, userAgent, acceptEncoding;
        bool       viaProxy = false;
    };

    void  respond(const QString& url, const Reply& reply); // by the fully encoded URL; missing: 404
    void  respondSequence(const QString& url, const QVector<Reply>& replies); // one per request, the last repeats
    QVector<Request> requests() const;
    QStringList      urls() const;
    int              count() const;
    qint64           bytesRead() const { return m_bytesRead.load(); }
    void             setViaProxy(bool viaProxy) { m_viaProxy = viaProxy; }
    void             setDns(const QString& host, const QString& address);
    std::function<qint64()> clock; // optional: a virtual clock for delays (else real waits)

    linkpreview::http::Error resolve(const QString& host, QVector<linkpreview::net::Address>* out, const linkpreview::http::Cancel& cancel,
                                     quint64 deadline) override;
    linkpreview::http::Route route(const QUrl& url, const linkpreview::http::Cancel& cancel, quint64 deadline) override;
    std::unique_ptr<linkpreview::http::Exchange> open(const linkpreview::http::HopOptions& options, const linkpreview::http::Cancel& cancel,
                                                      linkpreview::http::Error* error) override;

    std::atomic<qint64> m_bytesRead{0};

  private:
    mutable QMutex                   m_mutex;
    QHash<QString, QVector<Reply>>   m_replies;
    QHash<QString, int>              m_served;
    QHash<QString, QString>          m_dns;
    QVector<Request>                 m_requests;
    bool                             m_viaProxy = false;
};

} // namespace linkfixtures
