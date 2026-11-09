#include "updatehint.h"

#include <QRandomGenerator>

#include "i18n.h"
#include "updatepolicy.h"

namespace upd {

namespace {

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

bool isNonce(const QString& text)
{
    if (text.size() != 16)
        return false;
    for (const QChar c : text) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
            return false;
    }
    return true;
}

} // namespace

PeerHints::PeerHints(const QString& nonceHex)
    : m_nonce(isNonce(nonceHex) ? nonceHex : QString())
{
}

QString PeerHints::newNonce()
{
    return QString::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, QLatin1Char('0'));
}

PeerHints::Verdict PeerHints::check(const QString& peerText, const Version& floor) const
{
    const Version v = Version::parse(peerText); // strict: 3 parts, no leading zeros, no suffix
    if (!v.isValid())
        return Verdict::Malformed;
    if (!(v > floor))
        return Verdict::NotNewer;
    if (v.major > floor.major + 1)
        return Verdict::Implausible; // "99.0.0"
    if (m_notReleased.contains(v))
        return Verdict::NotReleased;
    if (m_shown.contains(v))
        return Verdict::AlreadyHinted;
    if (!m_shown.isEmpty() && !(v > m_shown.last()))
        return Verdict::NotHigher;
    if (m_shown.size() >= kMaxPeerHintsPerSession)
        return Verdict::Capped;
    return Verdict::Show;
}

void PeerHints::markShown(const Version& version)
{
    if (version.isValid() && !m_shown.contains(version) && (m_shown.isEmpty() || version > m_shown.last()))
        m_shown.append(version);
}

void PeerHints::markNotReleased(const Version& version)
{
    if (version.isValid() && !m_notReleased.contains(version))
        m_notReleased.append(version);
}

QString PeerHints::linkUrl(const Version& version) const
{
    if (m_nonce.isEmpty() || !version.isValid())
        return QString();
    // Fixed parts, hex digits and a validated version: nothing that could end a [url=...] tag.
    return latestReleaseUrl().toString() + latin("#tsmupd-") + m_nonce + QLatin1Char('-') + version.toString();
}

bool PeerHints::parseLink(const QString& href, Version* hinted) const
{
    if (m_nonce.isEmpty())
        return false;
    const QString prefix = latestReleaseUrl().toString() + latin("#tsmupd-") + m_nonce + QLatin1Char('-');
    if (!href.startsWith(prefix))
        return false;
    const Version v = Version::parse(href.mid(prefix.size()));
    if (!v.isValid() || !m_shown.contains(v) || linkUrl(v) != href)
        return false;
    if (hinted)
        *hinted = v;
    return true;
}

QString PeerHints::text(const Version& version, const Version& current)
{
    return i18n::t("Someone in this channel has TS Media chat %1 (you have %2).").arg(version.toString(), current.toString());
}

QString PeerHints::linkText()
{
    return i18n::t("Check for updates");
}

} // namespace upd
