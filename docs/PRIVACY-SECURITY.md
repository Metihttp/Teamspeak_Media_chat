# Privacy and security

[← Back to README](../README.md)

Where your files go, what other people learn about you, what stays on your computer, and how the plugin protects you from what others post. This page describes version 2.2.

## At a glance

- Files go only to your TeamSpeak server, with TeamSpeak's own file transfer.
- The only web request the plugin makes is the update check, to GitHub, and only after you agree.
- People in your channel learn that you have TS Media chat and its version, and see your reactions. Both go through TeamSpeak and can be turned off.
- The microphone is used only while the *Voice message* window shows *Recording*.
- No telemetry, no accounts, no analytics.

## Where your files go

- **Files only go to your TeamSpeak server.** Uploads and downloads use TeamSpeak's own file transfer into the channel's file browser. There is no other server or cloud service, and video compression and picture editing run on your own computer.
- **Channel members can open what you send.** Anyone on the server with download permission for that channel can get the file. Treat sent files like any other file in the file browser: they stay on the server until someone deletes them.
- **The chat message is an ordinary TeamSpeak message.** It holds the file link with the file's size, picture size, duration, a blurred placeholder, the path of its preview, its SHA-256 and, where they apply, the spoiler, album and voice message marks. Everyone who can read the chat can read it, with or without the plugin.

## What your channel sees

TS Media chat talks to other copies of itself with TeamSpeak's plugin commands. They go through your TeamSpeak server to the people in your channel, or to one private-chat partner, never to the whole server and never to the internet. Clients without TS Media chat ignore them.

- **Presence.** When you enter a channel, the plugin says hello to the people there, and to a private-chat partner when you send them something: *I have TS Media chat*, its version (for example `2.2.0`) and which features it supports. That is how the send window can say *3 of 5 people here will see it in the chat*. When you turn it off, or the plugin unloads, it says goodbye. Turn it off with **Settings → Privacy & updates → Tell people in your channel that you have TS Media**.
- **Reactions.** Your reactions go to the people with TS Media who are online in the same channel or private chat: which reactions you picked on which media item (named by a hash of its link). Someone who comes in later asks the people present for their own reactions; nobody ever passes on someone else's. Reactions are not stored on the server. Turn them off with **Settings → Privacy & updates → Show reactions on media**: none are then shown or sent.
- **Who sent it** is always the identity the TeamSpeak server reports, never something written inside a message, so nobody can react or say hello in your name.
- What others see comes from their messages, checked the same way: reactions on channel media are accepted only from people in your channel, private ones only from the partner of that private chat, and nothing from ServerQuery clients.

## Web requests

- **The update check is the only web request**, and only in official builds. About 45 seconds after the first start, the plugin asks *Keep TS Media chat up to date?*; nothing is sent before you click **Turn on**. Then it asks GitHub about once a day for a small file (`tsmedia-update.json`), and downloads the plugin files only after you click **Update**. **Check now** checks once when you ask.
- **What GitHub learns:** your IP address. Windows may also check GitHub's certificate with its issuer, and a proxy configured in Windows sees the connection. No names, servers, chats, files or identifiers are sent, the User-Agent carries no version, and there are no cookies. The plugin follows redirects only to GitHub's own hosts over HTTPS. Details: [updates](UPDATES.md#what-is-sent).
- **Nothing else.** The link in the note for people without the plugin, *Get it from Microsoft Store* for video extensions and *Copy and open bug report* open your browser or the Store only when someone clicks them; the plugin itself sends nothing.
- Copies you build yourself contain no update check and never load Windows' web library (`winhttp.dll`).

## What stays on your computer

Everything below is in `%APPDATA%\TS3Client\plugins\tsmedia` (a portable TeamSpeak uses the `config` folder inside the TeamSpeak folder instead of `%APPDATA%\TS3Client`).

- **Local cache** (`cache`): downloaded media, limited by the cache size you choose. You can clear it at any time in the settings.
- **Temporary copies:** files being sent are staged in the `upload` and `paste` folders. Pictures edited in the send window are written to `edit`, and voice recordings to `voice`. They are deleted once they have been sent or dismissed, and at the latest at the next start (voice recordings after a day).
- **Dragged and copied files** (`export`): dragging a file out of the chat or using *Copy file* makes a copy with a clean name (a hard link where possible). These copies are removed after 24 hours, even if the cache limit removed the cached file earlier.
- **Saved reactions** (`reactions.json`): the reactions you have seen, with the TeamSpeak unique ID and nickname of each person who reacted, for at most 30 days and 500 media items. **Clear cache** deletes them.
- **The plugin's log** (`logs\tsmedia.log`, up to 256 KB, plus one older copy `tsmedia.1.log`): the same lines TeamSpeak's own log gets from the plugin, so it contains the names of files you send and receive, local paths, and server, channel and nicknames where a line mentions them. Those values are marked in the file, so the diagnostic info can leave them out. The log never leaves your computer by itself.
- **Diagnostic info** (`/tsmedia diag`) is built only when you open it and leaves your computer only if you paste it somewhere. It shows exactly what *Copy* copies: plugin, Qt, TeamSpeak and Windows versions, which video and audio codecs Windows has, your settings (custom folders and links only as "changed"), counts of this session's transfers, file checks, presence and reactions, and the recent TS Media lines of the log. File names and paths in those lines read `<file 1>`, `<path 1>` unless you tick *Include file names* (your user folder then shows as `%USERPROFILE%`). Server addresses, server and channel names, unique IDs and nicknames are never included. *Copy and open bug report* opens GitHub's form in your browser with only the version fields filled in.
- **Settings** (`settings.ini`), including the servers you gave their own settings, by their ID and name.

## The microphone (voice messages)

- **Only while you record.** The plugin opens the microphone only after you start a voice message, and only while the *Voice message* window shows *Recording*. If that window is closed, or TeamSpeak unloads the plugin, recording stops at once. Nothing is recorded in the background, and the hotkey can't record without the window appearing.
- **Nothing leaves your computer until you press Send.** The sound is kept in memory, written to `%APPDATA%\TS3Client\plugins\tsmedia\voice` when you stop, and sent through TeamSpeak's file transfer like any file. Discarded recordings are deleted right away; sent ones are deleted with their upload (a failed one stays for Retry until you dismiss it), and anything left over at the next start.
- **Your TeamSpeak microphone is muted while you record** (a setting), and others can see that. It is unmuted again when recording stops; it is never unmuted where you muted it yourself. If something goes wrong (TeamSpeak crashes, the connection drops), it stays muted rather than open.
- The recording is the raw microphone: TeamSpeak's noise suppression and echo cancellation don't apply to it.

## Photo and video metadata

- **Edited pictures** (crop, draw, hide details in the send window) are sent as a new file without any metadata: no EXIF, so no camera details and no location.
- **Pictures you send without editing are sent exactly as they are**, so a phone photo can still contain where it was taken.
- **Compressed videos** keep no metadata (including the location some phones store), extra sound tracks or subtitles. Videos sent as *Original* are unchanged.
- **Hiding details:** *Black box* covers an area with solid black. *Pixelate* replaces it with coarse blocks (with a little fixed noise so the text can't be guessed back), but large text can stay partly readable; use *Black box* for passwords, codes and addresses.

## Nothing is sent by accident

- **Pasting always opens the send window first.** The clipboard may hold something old you forgot about.
- **Dropping opens the send window too**, unless you chose *Send right away*. Hold Shift while dropping to get TeamSpeak's normal behaviour, or turn drop sending off in the settings.
- **Private chats stay private.** If the partner of a private chat can't be identified, nothing is sent.
- **Files dragged out of the chat are never uploaded again** when they are dropped back on a chat or the send window.

## Received files are checked and never run

- **SHA-256 checks.** A file sent with TS Media chat 2.2 carries the SHA-256 of its exact bytes in the link, and its preview the start of its own. After a download the plugin hashes the file, off TeamSpeak's user interface thread, and compares. A file that doesn't match (replaced or damaged on the server) is deleted and never shown or saved: *File doesn't match what was sent*. There is no "open anyway". A link can't borrow a genuine file's cache entry with a made-up checksum, because the checksum is part of the entry's key.
- **Programs are never run.** `.exe`, `.bat`, `.cmd`, `.msi`, `.ps1`, `.vbs`, `.js`, `.lnk`, `.jar`, `.dll`, `.reg`, `.scr`, `.ts3_plugin` and the other types Windows runs on double-click are only shown in Explorer. Trailing-dot and trailing-space tricks (such as `file.exe.`) are caught too.
- **Dragged and copied files carry Windows' "downloaded from the internet" mark** (Mark-of-the-Web), like a browser download: SmartScreen warns before a program runs, and Office opens documents in Protected View. The cached file is marked too.

## Chat links are untrusted input

Anyone can type a TeamSpeak file link with fake metadata, so the plugin checks everything a link claims:

- Dimensions are clamped to 16384 px and to an aspect ratio of at most 8:1.
- Invalid BlurHash placeholders, and ones longer than 120 characters, are ignored.
- Preview paths must be absolute, in the same channel and without `..`. Preview downloads above 5 MB are aborted, and a preview that doesn't match its checksum is not shown.
- The 2.2 fields (checksum, spoiler, album, voice message, waveform) are accepted only in their exact format; an invalid value drops only that field, never the link.
- A file link inside a caption, escaped or broken by invisible characters, is never taken for a real link, so a caption can't make the plugin download something.
- Images above 80 megapixels are never decoded. They show *Can't preview this image* and can still be opened in your default app.
- Expensive decodes and checksums run off TeamSpeak's user interface thread.
- File names are sanitised before they touch the disk, for the cache, *Save as* and dragged-out copies alike.
- Control and bidirectional-text characters are removed from displayed names and captions, so a name can't disguise its real extension.
- A download whose size doesn't match the link is discarded.
- Plugin commands from other clients are size- and rate-limited and checked field by field before anything is stored.

## Automatic downloads are limited

By default, only images, GIFs and voice messages up to 15 MB download automatically (voice messages at most 16 MB), plus small previews. Videos and audio files are only downloaded when you press play, and spoilers only after you reveal them. Data saver pauses automatic downloads for all servers or for one. The limits can be changed in the [settings](SETTINGS.md#receiving).

## Updates are signed

An update is installed only if its manifest carries a valid ECDSA P-256 signature from the author's key, built into the plugin, and every downloaded file matches the signed size and SHA-256. A hacked GitHub account alone can't push an update. Only newer versions are offered, and a version that fails to start is rolled back by itself. See [updates](UPDATES.md#trust).

## The DLL is not code-signed

The plugin DLL is not signed with a code-signing certificate, so some antivirus products may flag it. If you prefer, [build it yourself](BUILDING.md) from this source code; self-built copies have no update check.
