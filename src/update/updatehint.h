#pragma once

// 2.3.4: the newer-version hint. Every HELLO / HI of a peer names its TS Media version
// (peers::PeerDirectory::versionSeen). When someone in the user's channel announces one newer than
// anything this plugin knows of, the user's own chat gets one line: "Someone in this channel has TS
// Media chat 2.3.5 (you have 2.3.4)." with a "Check for updates" link that the plugin handles itself
// (ChatIntegration's own-link handler): a manual check, one request to GitHub. Nothing is sent before
// the click. Pure QtCore, unit-tested.
//
// The version is UNTRUSTED (anyone can claim any version), so: strict versions only, at most one major
// version ahead, every hint above all earlier ones, never one a check found unreleased, and at most
// kMaxPeerHintsPerSession lines per plugin load.
//
// The link is https://github.com/<repo>/releases/latest#tsmupd-<nonce>-<version>, with a random nonce
// per Updater. A link someone types into the chat, or one from an earlier plugin load, doesn't match:
// TeamSpeak then opens the releases page, which is harmless.

#include <QString>
#include <QVector>

#include "updatemanifest.h"

namespace upd {

constexpr int kMaxPeerHintsPerSession = 2;

class PeerHints
{
  public:
    enum class Verdict { Show, Malformed, NotNewer, Implausible, AlreadyHinted, NotHigher, Capped, NotReleased };

    explicit PeerHints(const QString& nonceHex); // 16 lowercase hex digits (anything else: no link is ever ours)
    static QString newNonce();                   // 16 hex digits from QRandomGenerator::system()

    // floor: the newest version this plugin knows of (running, installed, offered, skipped, latest
    // release); never below the running one, which also bounds the major version.
    Verdict check(const QString& peerText, const Version& floor) const;
    void    markShown(const Version& version);
    void    markNotReleased(const Version& version); // a check found that it isn't released
    int     shownCount() const { return m_shown.size(); }

    // The link of a hint that was shown; parseLink accepts exactly that text (this nonce, a shown version).
    QString linkUrl(const Version& version) const;
    bool    parseLink(const QString& href, Version* hinted) const;

    static QString text(const Version& version, const Version& current);
    static QString linkText();

  private:
    QString          m_nonce;
    QVector<Version> m_shown; // in the order shown (ascending)
    QVector<Version> m_notReleased;
};

} // namespace upd
