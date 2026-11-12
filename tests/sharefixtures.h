#pragma once

// 2.5 link previews: minimal synthesized Nextcloud and ownCloud 10 share pages for tst_shares and tst_linkpage_net
// (design2.md 4.1 N-T3, N-T5). Each mirrors the structure the research saw (research_nextcloud 4.1/4.2: the head's
// og tags, the initial-state block of base64 JSON inputs, the old template's hidden inputs); none is a copy of a real
// page, and every value is made up.

#include <QByteArray>
#include <QString>

namespace sharefixtures {

inline constexpr char kToken[] = "Ab3dE6gH9jK2mN5"; // 15 letters and digits: a default token
inline constexpr char kHost[]  = "cloud.example.org";

inline QByteArray state(const char* key, const QByteArray& json)
{
    return QByteArray("\t\t\t<input type=\"hidden\" id=\"initial-state-") + key + "\" value=\"" + json.toBase64() + "\">\n";
}

inline QByteArray filler(int count)
{
    QByteArray out;
    for (int i = 0; i < count; ++i)
        out += state(QByteArray("app") + QByteArray::number(i) + "-setting", "true");
    return out;
}

// A Nextcloud 31+ share page: the head's og tags, then the initial-state block (files_sharing first, other apps, the
// header menu, more apps), then the page's own markup.
struct NcPage {
    QByteArray host = kHost;
    QByteArray path = QByteArray("/s/") + kToken; // og:url's path
    QByteArray token = kToken;
    QByteArray name  = "top_shunt.jpg";
    QByteArray view  = "public-file-share";
    QByteArray ogImage;                 // default: <og:url>/preview
    QByteArray ogImageType = "image/jpeg";
    QByteArray ogVideoType;
    bool       menu         = true;
    bool       menuDownload = true;
    qint64     size         = 1757588;
    int        limitAt      = -1; // -1 none; 0 before the menu; 1 right after it; 2 after it and 20 more states
    bool       fileRequest  = false;
    int        permissions  = 1;
    int        before = 8, after = 16; // other apps' states before and after the menu
    QByteArray betweenInputs;           // put after the files_sharing states (white space, a comment, ...)
    QByteArray blockEnd = "\t</div>\n\t<div id=\"skip-actions\"><a href=\"#content\">Skip</a></div>\n";

    QByteArray head() const
    {
        const QByteArray url   = "https://" + host + path;
        const QByteArray image = ogImage.isNull() ? url + "/preview" : ogImage;
        QByteArray       out   = "<!DOCTYPE html>\n<html class=\"ng-csp\" lang=\"en\">\n<head data-requesttoken=\"x\">\n\t<meta charset=\"utf-8\">\n\t<title>"
                         + name + " - " + host + "</title>\n\t<meta name=\"theme-color\" content=\"#00679e\">\n";
        out += "<meta property=\"og:title\" content=\"" + name + "\"/>\n<meta property=\"og:description\" content=\"Cloud - a safe home\"/>\n";
        out += "<meta property=\"og:site_name\" content=\"Cloud\"/>\n<meta property=\"og:url\" content=\"" + url + "\"/>\n";
        out += "<meta property=\"og:type\" content=\"website\"/>\n<meta property=\"og:image\" content=\"" + image + "\"/>\n";
        if (!ogImageType.isEmpty())
            out += "<meta property=\"og:image:type\" content=\"" + ogImageType + "\"/>\n";
        if (!ogVideoType.isEmpty())
            out += "<meta property=\"og:video\" content=\"" + url + "/download\"/>\n<meta property=\"og:video:type\" content=\"" + ogVideoType + "\"/>\n";
        out += "</head>\n<body id=\"body-public\">\n\t<noscript><div id=\"nojavascript\">Please enable JavaScript.</div></noscript>\n";
        return out;
    }
    QByteArray block() const
    {
        QByteArray out = "\t<div id=\"initial-state-container\" style=\"display: none;\">\n";
        out += state("files_sharing-fileId", "18977") + state("files_sharing-isPublic", "true");
        out += state("files_sharing-sharingToken", "\"" + token + "\"");
        out += state("files_sharing-sharePermissions", QByteArray::number(permissions));
        out += state("files_sharing-filename", "\"" + name + "\"");
        out += state("files_sharing-view", "\"" + view + "\"");
        out += state("files_sharing-isFileRequest", fileRequest ? "true" : "false");
        out += betweenInputs;
        out += filler(before);
        if (limitAt == 0)
            out += state("files_downloadlimit-download_limit", "{\"limit\":3,\"downloads\":1}");
        if (menu) {
            QByteArray entries = "{\"id\":\"save\",\"label\":\"Add to your Cloud\",\"href\":\"\",\"icon\":\"icon-external\",\"details\":\"\"}";
            if (menuDownload)
                entries = "{\"id\":\"download\",\"label\":\"Download\",\"href\":\"https:\\/\\/" + host + "\\/public.php\\/dav\\/files\\/" + token
                          + "\\/?accept=zip\",\"icon\":\"icon-download\",\"details\":\"" + QByteArray::number(size) + "\"}," + entries;
            out += state("core-public-page-menu", "[" + entries + "]");
        }
        if (limitAt == 1)
            out += state("files_downloadlimit-download_limit", "{\"limit\":3,\"downloads\":1}");
        out += filler(after);
        if (limitAt == 2)
            out += filler(20) + state("files_downloadlimit-download_limit", "{\"limit\":3,\"downloads\":1}");
        return out;
    }
    QByteArray build() const { return head() + block() + blockEnd + "\t<header id=\"header\"><span>" + name + "</span></header>\n</body>\n</html>\n"; }
};

// Nextcloud 26-30: the block, then the old template's inputs after it (their names and order as NC 27 prints them).
inline QByteArray ncOldTemplate(bool hideDownload, int padding = 0)
{
    const QByteArray url = QByteArray("https://") + kHost + "/index.php/s/" + kToken;
    QByteArray       out = "<!DOCTYPE html>\n<html class=\"ng-csp\">\n<head>\n<meta charset=\"utf-8\"><title>Talk.mp4 - Nextcloud</title>\n";
    out += "<meta name=\"theme-color\" content=\"#B19DF5\">\n<meta property=\"og:title\" content=\"Talk.mp4\"/>\n";
    out += "<meta property=\"og:url\" content=\"" + url + "\"/>\n<meta property=\"og:type\" content=\"object\"/>\n";
    out += "<meta property=\"og:image\" content=\"https://" + QByteArray(kHost) + "/index.php/apps/theming/icon?v=1\"/>\n</head>\n<body id=\"body-public\">\n";
    out += "<div id=\"initial-state-container\">\n" + state("core-versionHash", "\"604d8f94\"");
    if (padding > 0)
        out += "<input type=\"hidden\" id=\"initial-state-text-translation_languages\" value=\"" + QByteArray(padding, 'A') + "\">\n";
    out += state("theming-data", "{\"name\":\"Nextcloud\"}") + "</div>\n<div id=\"content\">\n";
    out += "<input type=\"hidden\" id=\"sharingUserId\" value=\"\">\n<input type=\"hidden\" id=\"filesApp\" name=\"filesApp\" value=\"1\">\n";
    out += "<input type=\"hidden\" name=\"sharingToken\" value=\"" + QByteArray(kToken) + "\" id=\"sharingToken\">\n";
    out += "<input type=\"hidden\" name=\"filename\" value=\"Talk.mp4\" id=\"filename\">\n";
    out += "<input type=\"hidden\" name=\"mimetype\" value=\"video/mp4\" id=\"mimetype\">\n";
    out += "<input type=\"hidden\" name=\"hideDownload\" value=\"" + QByteArray(hideDownload ? "true" : "false") + "\" id=\"hideDownload\">\n";
    out += "<input type=\"hidden\" name=\"filesize\" value=\"748121190\" id=\"filesize\">\n";
    out += "<input type=\"hidden\" name=\"maxSizeAnimateGif\" value=\"10\" id=\"maxSizeAnimateGif\">\n</div>\n</body>\n</html>\n";
    return out;
}

// ownCloud 10 with a theme: the theme's og set first (the instance's address), then the share's (escaped HTML in the
// themed og:title), no initial-state block, the old template's inputs.
inline QByteArray ownCloudPage(const QByteArray& fileName = "logo & mark.png")
{
    const QByteArray url = QByteArray("https://") + kHost + "/index.php/s/" + kToken;
    QByteArray       out = "<!DOCTYPE html>\n<html class=\"ng-csp\">\n<head data-requesttoken=\"x\">\n<title>ownCloud@Example</title>\n";
    out += "<meta property=\"og:type\" content=\"website\">\n<meta property=\"og:url\" content=\"https://" + QByteArray(kHost) + "\">\n";
    out += "<meta property=\"og:title\" content=\"ownCloud@Example\">\n<meta property=\"og:image\" content=\"\">\n";
    out += "<meta name=\"theme-color\" content=\"#0068A2\">\n";
    out += "<meta property=\"og:title\" content=\"ownCloud@Example - part of &lt;a href=&quot;http://example.org&quot;&gt;us&lt;/a&gt;\"/>\n";
    out += "<meta property=\"og:description\" content=\"x is publicly shared\"/>\n<meta property=\"og:url\" content=\"" + url + "\"/>\n";
    out += "<meta property=\"og:image\" content=\"https://" + QByteArray(kHost) + "/index.php/apps/files_sharing/publicpreview?file=/x.png&amp;t=" + kToken
           + "&amp;x=200&amp;y=200\"/>\n</head>\n<body id=\"body-public\">\n";
    out += "<input type=\"hidden\" id=\"filesApp\" name=\"filesApp\" value=\"1\">\n";
    out += "<input type=\"hidden\" name=\"downloadURL\" value=\"" + url + "/download\" id=\"downloadURL\">\n";
    out += "<input type=\"hidden\" name=\"sharingToken\" value=\"" + QByteArray(kToken) + "\" id=\"sharingToken\">\n";
    QByteArray escaped = fileName;
    escaped.replace("&", "&amp;");
    out += "<input type=\"hidden\" name=\"filename\" value=\"" + escaped + "\" id=\"filename\">\n";
    out += "<input type=\"hidden\" name=\"mimetype\" value=\"image/png\" id=\"mimetype\">\n";
    out += "<input type=\"hidden\" name=\"filesize\" value=\"32812\" id=\"filesize\">\n</body>\n</html>\n";
    return out;
}

} // namespace sharefixtures
