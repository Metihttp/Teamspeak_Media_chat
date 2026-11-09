#pragma once

// 2.4 link previews: what one link's slot in the chat shows, and the made-up file links that let Core show
// a link's media like uploaded media. Pure logic (QtCore + medialink), unit-tested; ChatLinkPreviews feeds it.
//
// What a slot shows, first match wins:
//  1. Hidden (the user hid this card): the "Preview hidden · Show" pill.
//  2. Loaded here (the viewer clicked Load preview and the website answered): YouTube -> the YouTube card;
//     a picture, GIF or video -> the media itself (Core entry made by adoptLocalFile).
//  3. Announced by the message's sender (sender-made preview, files on the TeamSpeak server) and not failed:
//     YouTube -> the YouTube card (its thumbnail loading from the TeamSpeak server first); a picture / GIF, or
//     a video with its file -> the media (Core downloads it from the TeamSpeak server and checks its SHA-256);
//     a video without its file (too large) -> the poster card with "Open in browser".
//  4. Loading here -> the strip with the ring; failed -> the error strip with Retry; too large -> "Open in
//     browser".
//  5. Waiting (2.4 "Load link previews automatically", linkpreview/autoload.h): for our own pipeline ("Making the
//     preview…"), for the sender's preview ("Waiting for the sender's preview…"), for the answer whether the sender
//     makes one ("Checking for the sender's preview…"), or for an automatic load that is about to start -> the same
//     strip with the ring (the same size as every strip: nothing jumps).
//  6. Otherwise the click-to-load strip (with the file's size when it is above the automatic limit).
// A sender's preview whose files fail for good (gone from the server, no permission to download them there) falls
// back to 4-5; a failure that may pass (connection, quota, a hash that didn't match while the upload was still
// arriving) keeps it, with Core's own retry.

#include <QString>

#include "core.h"
#include "linkpreview/linkcard.h"
#include "linkpreview/linkurl.h"
#include "linkpreview/previewstore.h"

namespace linkstate {

// OverAutoLimit: an automatic load stopped because the file is larger than the automatic limit (size: what the
// website announced, -1 unknown); the click card says so and a click loads it with the click cap.
enum class Local { Idle, Loading, Loaded, Failed, TooLarge, OverAutoLimit };

// What a slot without anything to show waits for (2.4 auto-load): our own pipeline, the sender's preview, or an
// automatic website load that is due (about to start; ChatLinkPreviews starts it). Checking: we are still asking
// whether the sender makes previews (autoload::Wait::Checking).
enum class Wait { None, Own, Sender, Checking, Auto };

// The viewer's own fetch of a link (by Target::key; one per link, shared by every message showing it).
struct LocalState {
    Local   state    = Local::Idle;
    double  progress = -1.0;
    QString error;
    qint64  size = -1; // TooLarge / OverAutoLimit: what the website announced
    QString mediaKey; // Loaded picture / GIF / video: Core's key (adoptLocalFile)
    // Loaded YouTube
    QString title, author, thumbnailFile;
};

// What Core says about the files of a sender's announcement (Core keys from serverLink(), the sender's linkFor()).
struct ServerState {
    bool       announced = false;
    bool       hasMedia  = false;  // the announcement names the full file
    QString    mainKey;            // the media (hasMedia) or the preview picture as the main file
    MediaState state = MediaState::Idle; // of mainKey
    MediaError error = MediaError::None;
    bool       previewFailed = false; // mainKey's preview picture (the poster of an announced full file) failed for good
};

// What Core says of mainKey's entry (nullptr: none yet), as the chat reads it into a ServerState.
void readEntry(ServerState* server, const MediaEntry* entry);

// Whether a sender's announcement of kind announced is shown under a link the chat classifies as link. The sender
// names the kind by the bytes it got (a ".jpg" that is an animated GIF goes up as a GIF, a ".gif" that is a still
// as a picture); pictures and GIFs are one family here (decide() shows both as the media). Never across families.
bool kindFits(linkpreview::Kind announced, linkpreview::Kind link);

struct Decision {
    bool           media = false; // show the media preview (mediaKey) instead of a card
    QString        mediaKey;
    linkcard::Look look       = linkcard::Look::Click;
    bool           fromServer = false;
    Wait           wait       = Wait::None; // look Loading: what it waits for (None: a load here is running)
};

// wait: what the slot would wait for when nothing else is to be shown (rule 5). A wait never beats anything
// shown, loading, failed or too large; Auto only for a link nobody tried here yet (Idle).
Decision decide(linkpreview::Kind kind, bool hidden, const LocalState& local, const ServerState& server, Wait wait = Wait::None);

// A sender's preview that can't be shown any more: its main file's download failed for good (NotFound, Permission,
// Password), or the full file waits for a click (not loaded automatically) and its preview picture failed for good.
bool serverFailed(const ServerState& server);

// ---- Core links ---------------------------------------------------------------------------------------

// An announcement with the file browser that holds its files filled in where it doesn't say (announcements
// always do since the hub stores them with their server and channel; the fallbacks are the connection's
// server and the sender's or our channel).
linkpreview::Announcement withFileBrowser(linkpreview::Announcement a, const QString& serverUid, quint64 channelId);

// The link Core downloads an announcement's main file by: exactly the sender-previews' linkFor() (the full file
// when it was announced, its preview picture as its preview; else the preview picture), so Core knows one entry
// per file whichever side asks first. False when the announcement isn't usable (no server or channel, broken
// paths or hashes): nothing is downloaded then. Core checks the downloaded file against its SHA-256.
bool serverLink(const linkpreview::Announcement& a, MediaLink* main);

// What "Open in browser" opens and "Copy link" copies for a link's slot: the link as it was written (a YouTube
// link keeps its start time and its own host, as clicking the link itself would), https (TeamSpeak links
// "www.youtube.com/..." typed without a scheme as http://); target's checked address when href isn't that link.
QUrl webAddress(const QString& href, const linkpreview::Target& target);

// The card's line for a failed website fetch (linkpreview::errors code): empty (the card says "Couldn't load the
// preview") for everything about the connection (blocked, offline, timeout, tls, ...); the details are logged.
QString errorText(const QString& code);

// A file loaded from a website, as a made-up link for Core::adoptLocalFile: server "tsmedia-web", channel 1,
// folder "/web/<key>", the file's name from the address (kept clean) with the extension its content has
// (mime, else the address'); size and dimensions as known. Never sent anywhere.
MediaLink webLink(const linkpreview::Target& target, const QString& mime, qint64 size, int width, int height, qint64 durationMs);
bool      isWebLink(const MediaLink& link);
QString   webServerUid();

} // namespace linkstate
