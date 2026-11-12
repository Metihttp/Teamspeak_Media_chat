#pragma once

// 2.5 link previews: Nextcloud and ownCloud 10 public share links (design2.md section 2). Pure QtCore (unit-tested in
// tst_shares), no network. providers.cpp classifies with parse() (the shape rows of the MediaPage provider
// "nextcloud", matched by path shape on any host no provider owns); the Fetcher's share plan reads the share page with
// readMarkers() / markersComplete(), decides with decide() and builds every address it requests here.
//
// N2: a share is confirmed only from the response: the body's token marker equal to the link's token AND an og:url
// on the final host ending in /s/<token>. Never by host name, never by a product header.
// N3: every media, preview and thumbnail address is built from the link (host, prefix, token, a fixed template),
// never taken from the page. The one exception is ownCloud 10's picture address, which carries the page's file name,
// strictly encoded (encodeFileName). Every hop stays on the share's own host.
// N4: no file request for password shares, file drops, hidden downloads (except the server's preview of a picture as
// the card's picture), download-limit shares, incomplete reads or sizes over the cap.

#include <QString>
#include <QStringList>
#include <QUrl>

class QTextCodec;

namespace linkpreview::shares {

// The link forms (2.2): the share page (/s/<token>, also with /<fileid>), its download, its preview, the
// publicpreview addresses of Nextcloud (publicpreview/<token>) and ownCloud (publicpreview?...&t=<token>), and
// Nextcloud's public DAV address of the shared file (public.php/dav/files/<token>).
enum class Form { None, Page, Download, Preview, PublicPreview, Dav };

struct ShareLink {
    QString host;             // lower-case ASCII host (publicHostName)
    QString prefix;           // the install prefix as written: "", "/nextcloud", "/a/b" (ASCII, never a dot segment)
    bool    indexPhp = false; // "/index.php" after the prefix (as the link wrote it)
    QString token;
    Form    form = Form::None;
    bool    isValid() const { return form != Form::None; }
};

// The share form of a link (http or https, as written; read as normalizedPage() reads it), or Form::None. Path rules
// over the fully encoded path (ASCII only; a %2F never splits a segment): a prefix of at most two segments of
// [A-Za-z0-9._~-]{1,64} that are none of s, index.php, apps, public.php, remote.php and not only dots; the pretty
// /s/<token> forms only with exactly 15 letters and digits, the index.php, publicpreview and DAV forms with
// [A-Za-z0-9-]{6,32}; ownCloud's publicpreview with exactly one query item t.
ShareLink parse(const QUrl& url);
// "page", "download", "preview", "pp", "dav"; empty for None.
QString subOf(Form form);
// Automatic-budget tokens of one load: the page and download forms 2 (the page, then the file or its preview), the
// others 1.
int expectedRequestsOf(Form form);

// ---- addresses (all built from the link; 2.5) ---------------------------------------------------------------------

constexpr int kPictureSide     = 2048; // a picture's preview (a 400 du picture at R 4 needs 1600 px)
constexpr int kCardPictureSide = 1024; // a share card's picture

QString root(const ShareLink& s);                         // https://host + prefix
QString base(const ShareLink& s);                         // root + "/index.php" when the page had it
QUrl    pageUrl(const ShareLink& s);                      // base/s/<token>: no slash, file id, download or query
QUrl    downloadUrl(const ShareLink& s);                  // base/s/<token>/download
QUrl    previewUrl(const ShareLink& s);                   // base/s/<token>/preview
QUrl    publicPreviewUrl(const ShareLink& s, int side);   // root/index.php/apps/files_sharing/publicpreview/<token>?x=&y=&a=1
// ownCloud 10's picture: root/index.php/apps/files_sharing/publicpreview?file=/<name>&t=<token>&x=&y=&a=1; invalid
// when encodeFileName refuses the name.
QUrl ownCloudPreviewUrl(const ShareLink& s, const QString& fileName, int side);
// The file name ownCloud's picture address carries (the exception to N3): 1..255 bytes of UTF-8, no '/', '\', NUL or
// other control character (C0, DEL, C1), not "." or ".."; then percent-encoded byte by byte, only RFC 3986 unreserved
// characters kept. Empty when refused.
QString encodeFileName(const QString& name);
// The share base after the page's redirects on the share's host (2.3): the final address read as a share page with
// the same token gives its prefix and index.php; otherwise the link's own.
ShareLink baseAfter(const ShareLink& link, const QUrl& finalUrl);
// A hop of the download request (2.5): the request itself, or a same-host redirect to
// <prefix>[/index.php]/s/<token>/download or <prefix>/public.php/dav/files/<token>[/] (query accept=zip tolerated).
bool downloadHopAllowed(const ShareLink& s, const QUrl& hop);
// A same-host preview address of this share (an og:image that says the server makes a preview of the file):
// .../apps/files_sharing/publicpreview[/<token>], with t=<token> for ownCloud's, or .../s/<token>/preview.
bool isPreviewAddress(const QUrl& url, const ShareLink& s);
// Any host: an address that reads a shared file or its preview through a share's token (Nextcloud, ownCloud, oCIS,
// Seafile): .../apps/files_sharing/publicpreview[/...], .../s/<token>/preview|download[/...],
// .../public.php/dav/files/<token>/..., .../public.php/webdav..., .../remote.php/dav/public-files/<token>/...,
// .../seafhttp/files/<token>/... Every such read may use up a download of a limited share (research_nextcloud: the
// download limit counts every file read, a preview too), and the token is a capability: never a web page's picture,
// never a GIF kept by itself.
bool isShareFileAddress(const QUrl& url);

// ---- the page's markers (2.3) --------------------------------------------------------------------------------------

constexpr int kMaxStateJsonBytes = 64 * 1024; // one initial state, decoded

// What a share page says: the head's og:url / og:image / og:image:type / og:video:type, and the whitelisted body
// <input type="hidden"> tags: Nextcloud's initial states (base64 of JSON: decoded with typed reads, at most 64 KiB
// each, unknown keys ignored) and the old template's inputs (NC <= 30, ownCloud 10). Inputs are matched by id; name
// only for the old template's names and only on an input without an id; the first of each wins. Inputs before <body>,
// in comments and in raw-text elements never count.
struct Markers {
    // The initial-state block: its first input, and the first start or end tag after it that isn't one.
    bool blockStarted = false, blockEnded = false;
    // NC 31+ (initial states)
    bool    hasStateToken = false;
    QString stateToken; // files_sharing-sharingToken (a JSON string)
    bool    hasView = false;
    QString view;       // "public-file-share", "public-share"
    bool    hasStateName = false;
    QString stateName;  // files_sharing-filename
    int     isFileRequest = -1;  // -1: not said
    qint64  permissions   = -1;  // files_sharing-sharePermissions; -1: not said
    bool    menuSeen = false, menuDownload = false;
    qint64  menuSize = -1;       // the menu's download entry's details (decimal digits only, <= 2^53)
    bool    downloadLimit = false; // files_downloadlimit-download_limit present
    bool    passwordForm  = false; // core-publicShareAuth present
    // The old template (NC <= 30, ownCloud 10)
    bool    hasTemplateToken = false;
    QString templateToken;
    bool    hasTemplateName = false;
    QString templateName;
    bool    hasMimetype = false;
    QString mimetype;
    bool    hasFilesize = false;
    qint64  filesize    = -1;
    bool    hasHideDownload = false;
    bool    hideDownload    = false;
    // The head
    QStringList ogUrls;   // every og:url (at most 8)
    QStringList ogImages; // og:image / og:image:url / og:image:secure_url (at most 4)
    QString     ogImageType, ogVideoType;
    // The read
    bool tagLimit = false; // the tokenizer's tag limit was hit
};
// The markers of html (already pagemeta::prepared(); codec null: UTF-8).
Markers readMarkers(const QByteArray& html, const QTextCodec* codec);
// The read's early stop: (the block ended, or no initial-state input yet and the old template's token, mimetype and
// filesize seen) AND a token marker AND (NC 31+'s view, or the old template's mimetype and filesize).
bool markersComplete(const Markers& m);
// A read that stopped (cap, tag limit, end of body) inside the initial-state block: a download limit may be in the
// part not read.
bool insideBlock(const Markers& m);

// ---- the decision (2.4) --------------------------------------------------------------------------------------------

enum class FileType { File, Folder, Pdf, Document, Spreadsheet, Presentation, Archive, Audio, Text, Picture, Gif, Video };

// The file's type: from its MIME type (the old template's mimetype, else og:image:type / og:video:type), else by its
// name's extension; a folder by NC 31+'s view or ownCloud's httpd/unix-directory.
FileType typeOf(const Markers& m);

enum class Action {
    NotFound,    // 404 / 410: expired or deleted
    Nothing,     // a password share, a log-in page, a file drop: no preview
    Unconfirmed, // not proven a share: the generic web card rules on the same bytes
    Card,        // a share card (name, "Type · size")
    Picture,     // the picture through the server's preview
    Gif,         // the GIF through /download
    Video,       // the video through /download
    TooLarge,    // a video over the cap: no media request
};

struct Decision {
    Action   action   = Action::Nothing;
    FileType type     = FileType::File;
    qint64   size     = -1;    // bytes, -1: unknown
    bool     viewOnly = false; // the owner turned downloads off
    bool     noFiles  = false; // a download limit or an incomplete read: no picture, no preview, no file request
    bool     ownCloud = false; // no initial-state block: ownCloud 10's addresses
    QString  name;             // the file or folder name as the page said it (not cleaned)
    QString  reason;           // for logs and tests: "password", "login", "drop", "limit", "incomplete", ...
};

struct PageFacts {
    int       status = 200;
    QUrl      finalUrl;   // the page's last hop (on the share's host)
    ShareLink link;       // the link (its token)
    Markers   markers;
    qint64    videoCap = 0; // the most a video may download (Caps::video, else Caps::media)
};
// The first row of design2 2.4 that matches.
Decision decide(const PageFacts& facts);
// The size the page announced (NC 31+ menu details, else the old template's filesize), -1 when it didn't.
qint64 sizeOf(const Markers& m);
// The download is hidden: NC 31+ (a view) whose menu has no download entry, or the old template's hideDownload.
bool downloadHidden(const Markers& m);
// The picture's file is a JPEG, PNG or WebP (by its MIME type, else its name): when its previews fail, the file itself
// may load (within the cap); HEIC, TIFF, BMP and AVIF never.
bool isWebPicture(const Markers& m);

// ---- the share card (2.7) ------------------------------------------------------------------------------------------

// "Folder", "PDF document", "Document", "Spreadsheet", "Presentation", "Archive", "Audio", "Text file", "Picture",
// "GIF", "Video", "File".
QString typeLabel(FileType type);
// "<Type> · <size>" (formatSize; the size left out when unknown), "... · View only" when the download is hidden.
QString cardDescription(FileType type, qint64 size, bool viewOnly);

} // namespace linkpreview::shares
