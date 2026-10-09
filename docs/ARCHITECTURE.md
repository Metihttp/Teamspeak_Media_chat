# Architecture overview

[← Back to README](../README.md)

How TS Media chat is put together: the modules, the link format it posts, how uploads and downloads flow, how copies of the plugin talk to each other, and the rules that keep it safe inside TeamSpeak's process. For build instructions, see [building from source](BUILDING.md); for the updater, see [updates](UPDATES.md).

## Platform constraints

- The plugin is a DLL loaded into the TeamSpeak 3 client (plugin API 26). It uses the Qt 5.15.2 that TeamSpeak 3.6 ships, and only the modules TeamSpeak ships with it: QtCore, QtGui and QtWidgets. There is no QtMultimedia and no QtConcurrent.
- Video, audio, voice recording and video compression use Windows Media Foundation and WASAPI directly. `mfplat.dll` and `mfreadwrite.dll` are delay-loaded and checked for before use, so the plugin still loads on Windows N editions without the Media Feature Pack, just without those features.
- SHA-256 and signature checks use Windows CNG (`bcrypt.dll`). `winhttp.dll` is loaded on demand, only in official builds and only while an update check or download runs.
- The previews live inside TeamSpeak's own chat: TeamSpeak's chat views are `QTextBrowser` widgets, and the plugin adds its previews to their documents as image resources placed under the file links.

## Module map

### TeamSpeak and the engine

| Path | Purpose |
| --- | --- |
| `src/plugin.cpp` | TeamSpeak C entry points, menus, hotkeys and chat commands; moves callbacks onto the GUI thread; the shutdown order |
| `src/ts3api.*` | Thin wrapper around TeamSpeak's function table (connections, channels, chat output, paths) |
| `src/core.*` | Upload and download engine: staging, compression, hashing, previews, the post queue, the cache with its size limit, error mapping, data saver holds, exports for drag-out |
| `src/floodgovernor.*` | Per-connection pacing of chat posts, file requests and plugin commands (TeamSpeak's anti-flood counts them together) |
| `src/medialink.*` | `ts3file://` link format, the chat message composer, display names, size and progress formatting, error texts |
| `src/mediaprobe.*`, `src/blurhash.*` | Sender side: size, duration, BlurHash and preview of a file |
| `src/hashing.*`, `src/crypto.*` | SHA-256 of files and buffers (CNG, with a Qt fallback); ECDSA P-256 verification for updates |
| `src/fileverify.*` | Checking downloads against their link's SHA-256, the sender's single hashing step, the guard against made-up hashes |
| `src/filenames.*` | Safe local file names for anything that comes from a chat link |

### In the chat

| Path | Purpose |
| --- | --- |
| `src/chatintegration.*` | Finds TeamSpeak's chat widgets, inserts previews under links, hides the plugin-required note, drag & drop in and out, paste, hover, right-click menu |
| `src/chatintegration_spoiler.cpp`, `src/spoiler.*` | Spoiler covers in the chat (shared blur used by the chat, the viewer and the send window), reveal state |
| `src/albums.*`, `src/chatalbums.cpp` | Album grouping, grid geometry and hit testing (pure), and the chat side: folding album links, hiding follow-up messages, drawing grids |
| `src/inlinemedia.*` | Animated GIFs and the inline video and audio players (one plays at a time) |
| `src/previewrenderer.*`, `src/previewpaint.h` | Draws inline images, video players, file cards and album tiles |
| `src/audiocard.*`, `src/audioplayback.*` | The 340 × 64 audio player card, and when audio downloads by itself |
| `src/voicecard.*` | The 56 px voice message card with its waveform |
| `src/chatreactions.*`, `src/reactionart.*`, `src/reactionpicker.*` | The reaction row, the add button and the picker; the six reactions as vector drawings made for this plugin |
| `src/filedrag.*`, `src/dragpixmap.*` | Dragging files out of the chat and the viewer, *Copy file*, the drag picture, refusing our own drags |
| `src/mediaviewer.*`, `src/spoilercover.*` | Gallery viewer: zoom, video player, full screen, shortcuts, the spoiler cover |
| `src/uploadtoast.*` | Upload progress panel |

### Sending and recording

| Path | Purpose |
| --- | --- |
| `src/composedialog.*`, `src/composemodel.*` | The send window (paste, drop, picker): caption, spoilers, albums, what can't be sent, one `Core::send` request; its rules without widgets |
| `src/composehooks.*`, `src/composesettings.*` | Where other features plug into the send window (presence line, albums, own-drag marker), and its Sending setting |
| `src/imageeditmodel.*`, `src/imageeditor.*`, `src/imageeditorcanvas.cpp` | The crop & annotate editor: the model without widgets (rotation, crop, shapes, undo, export) and its window |
| `src/videocompress.*`, `src/video/mftranscode.*`, `src/compresssettings.*` | The compression planner (pure), the Source Reader to Sink Writer transcode to H.264/AAC MP4 with its verification, and Settings → Sending → Videos |
| `src/audio/*` | Voice messages without UI: WASAPI capture, a generated test source, the microphone choice, the recorder thread, the AAC `.m4a` writer, waveform levels, the TeamSpeak mic mute guard and the start / stop sounds |
| `src/voicecontroller.*`, `src/voicepanel.*`, `src/voicesection.*` | Recording, review and sending of a voice message, its window, and its settings group |
| `src/video/mfvideo.*`, `src/video/mfcommon.*` | Media Foundation probing and playback; shared helpers (COM and platform scopes, error texts, D3D11 device, MPEG-4 sink writer) |

### Between plugins

| Path | Purpose |
| --- | --- |
| `src/peerprotocol.*` | The `tsm1` message format: parsing, writing and checking every field (pure) |
| `src/pluginlink.*` | Transport over TeamSpeak plugin commands: one queue per connection, return codes, timeouts, flood handling |
| `src/peers.*`, `src/presenceline.*` | Presence: who in your channel or private chat has TS Media, and the send window's line |
| `src/reactions.*` | Who reacted how to which media, and `reactions.json` |
| `src/peerhub.*` | Connects transport, presence and reactions to TeamSpeak and Core and owns them |

### Settings, servers and support

| Path | Purpose |
| --- | --- |
| `src/settings.*`, `src/settingsdialog.*`, `src/settingssection.*` | Settings (INI file), the tabbed settings dialog, and the base for groups a feature adds to a tab |
| `src/serversettings.*`, `src/serverssection.*`, `src/datasaver.*` | Settings per server, the Servers tab, data saver's menu pair, command and notice |
| `src/privacysection.*`, `src/spoilersection.*` | The Privacy and Spoilers groups |
| `src/accessgroup*.*` | The one-click `tsmediachat` server group: plan, job, view, the Server access box and the client menu items |
| `src/update/*` | The update check, consent and update windows, download, checks, install and rollback ([updates](UPDATES.md)) |
| `src/diagnostics*.*` | Diagnostic info: collecting the facts, redaction, the window |
| `src/logtext.*`, `src/pluginlog.*` | Structured log lines with private values tagged, and the plugin's own rotating log file |
| `src/uiutil.*`, `src/ownedtimer.h` | Small shared UI helpers (Windows accessibility settings, contrast checks, common texts); timers owned by their object |
| `src/i18n.*` | User-visible texts (English only) |
| `src/version.*.in` | Version header and Windows version resource, generated by CMake |

## Link and message format

A sent file is posted as the same kind of BBCode link TeamSpeak itself creates when a file is dragged from the file browser into the chat:

```text
[URL=ts3file://<host>?port=..&serverUID=..&channel=..&path=..&filename=..&isDir=0&size=..&fileDateTime=..&tsm=2&w=..&h=..&d=..&bh=..&pv=..]<file name>[/URL]
```

TeamSpeak ignores the extra parameters, so clients without the plugin can still click the link and download the file. TS Media chat adds only the ones that apply, all percent-encoded:

| Parameter | Meaning |
| --- | --- |
| `tsm` | marks a TS Media message (current protocol: 2) |
| `w`, `h` | media size in pixels, in display orientation |
| `d` | duration in milliseconds (video and audio) |
| `bh` | BlurHash placeholder |
| `pv` | path of a small JPEG preview or video poster in the same channel (2.2: `<dir>/previews/<8 hex>.jpg`) |

2.2 adds these after `pv`, always in this order and only on links with `tsm` 2 or higher. Their values are base64url (no padding), hex or plain numbers, so they are never percent-encoded:

| Parameter | Meaning | Accepted only if |
| --- | --- | --- |
| `sha` | SHA-256 of the file | 43 characters, canonical last character, decodes to 32 bytes |
| `ph` | first 16 bytes of the preview's SHA-256 | 22 characters, canonical, and the link has a valid `pv` |
| `sp` | spoiler | exactly `1`, on a picture, GIF or video |
| `al`, `ai`, `an` | album id, position, size | `al` is 8 lower-case hex digits and not `00000000`, `an` is 2–10, `ai` is 1–`an`; all three or none |
| `vm` | voice message | exactly `1`, on an audio file |
| `wf` | waveform of a voice message | only with `vm`: 64 levels of 4 bits = 32 bytes, 43 characters, canonical |

- Parsing accepts plain TeamSpeak links (no `tsm`), unknown parameters, any parameter order and HTML-escaped ampersands. The first occurrence of a parameter wins, and an invalid value drops only that field (the album: all three), never the link. A link without the 2.2 fields is byte-identical to what 2.1 sent.
- The key that identifies a file (cache names, chat resources, reactions) is the first 20 hex digits of SHA-1 over `serverUID`, channel, remote path and size, joined by `\n`. When the link carries a `sha`, `\n` and its base64url text are appended, so a link with a forged hash can never share the genuine file's entry. Links without one keep 2.1's key. This definition is frozen: every 2.2+ client computes the same key from the same link.
- A tag escaped with a backslash (`\[URL=…`) or broken by an invisible character (`[` U+200B `URL=…`) is text in TeamSpeak's chat, so it is never taken for a link either. `[noparse]` protects nothing: TeamSpeak 3.6 drops the tags and still parses what is inside, so the plugin never uses it.

A message is `[caption]` + LINK (+ LINK…) + `[note]`, the parts separated by a line break (`kMessageSeparator`):

- The caption comes first, because 2.1 receivers remove the text after a TS Media link. It is one line of at most 300 characters, without control or bidirectional-text characters; every `[` and `]` gets a backslash before it (TeamSpeak then shows the bracket as text), so a caption can never open a tag or fake a file link. Web addresses need no `[URL]`: the chat links them itself. The plugin's own chat lines (`printInfo`, `printWarning`) escape names the same way.
- Each link's label is the file's display name without the random part, `Spoiler (image)` / `Spoiler (GIF)` / `Spoiler (video)` for spoilers, or `Voice message (0:12)`.
- When the note for people without the plugin is on, it follows the last link of the first message, once: a grey, italic *— TS Media chat plugin required to view this in chat*, with "TS Media chat" linked to the configured download page.
- Several links share a message while they fit (albums). TeamSpeak takes up to 8192 UTF-8 bytes of text, and its client silently drops a command whose escaped form (ServerQuery escaping: `\`, `/`, space, `|` and control characters take two bytes) is above about 9.1 KB. Every message therefore keeps its escaped size (`escapedMessageSize()`) at or below `kMaxMessageBytes` = 8168; a 10-item album fits in one message. When a link doesn't fit, optional data is dropped in one order: `ph`, `bh`, `wf`, the note's link, then the caption moves to a message of its own right above the files, then the note, `pv`, `w`/`h`/`d` and `sha`. TeamSpeak's own parameters, `tsm`, `sp`, the album and `vm` are never dropped, the caption is never cut, and links are never stripped just to fit more of them into one message.
- Clients with the plugin remove everything after a TS Media link up to the next link or the end of its line, so they never see the note.

## Sending

1. **Validate and stage.** The plugin checks that the client is connected, that the file is not empty and within the upload limit (the server's own limit when it has one), and that the channel has no password. It then stages a copy of the file, named as it will be on the server, in the plugin's data folder. The copy is made on a worker thread, so a large file never blocks the chat. Edited pictures are exported by the send window first and staged like any file; voice messages arrive as a finished `.m4a`.
2. **Probe.** On a worker thread it reads the size, duration and BlurHash of the staged copy, and makes a preview or poster when the file needs one.
3. **Compress (videos).** A video that may be compressed is probed where it is instead, before any copy, and `videocompress::planCompression` decides with a snapshot of the settings taken when the send started (the send window's Quality list asks the same planner). To compress, the job is `UploadState::Compressing`: one video at a time on Core's transcode pool, below normal priority. `mf::transcodeToMp4` reads the original with a Source Reader (scaled, rotated upright and converted by the reader), writes H.264 High and AAC-LC with a Sink Writer, tries the graphics card first and the processor once, and verifies the result (opens, size, upright, sound, length). The verified MP4 is moved into staging with the same random part in its name. If it fails and the original fits the upload limit, the original is staged and sent instead.
4. **Hash.** One step, `finalizeStaged`, hashes the final staged bytes (after an edit's export or a transcode's verification) and the preview, off the GUI thread. A file that can't be read is sent without `sha`, and the log says so.
5. **Prepare folders.** It makes sure the upload folder and its `previews` folder exist. Without permission to create folders, the file goes to the channel root; if only the previews folder fails, the preview goes next to the file as `<8 hex>.preview.jpg`. Remote names are `<base>_<8 hex>.<ext>`, the base cut to 48 characters and 64 UTF-8 bytes.
6. **Upload the preview, then the file.** A failed preview is not fatal: the file is sent without it. At most two files upload at once.
7. **Post the messages** to the chat the user is looking at (channel, server or private chat). Every send goes through `Core::send(SendRequest)`: the albums first (pictures and videos, up to 10 each, posted once all of their files are uploaded, failed or canceled; the files that made it are numbered again), then the other files in their order, with the caption on the first message. Each connection has one post queue and one `FloodGovernor`, which models the server's anti-flood counters as measured (defaults: 150 points, 5 points back per second; a chat message costs 15, file info 8, a folder 5, a transfer 3; plugin commands cost 5 on a counter of their own). Messages go one at a time, each after the answer to the previous one or a second (the messages of one send wait for that answer, or its 5 s timeout, so they always arrive in order), and only while 30 points stay free for the user's own typing: from rest about 8 messages back to back, then one every 3 s. Folder requests and upload starts wait for the same governor and leave room for one more message, so a large album never runs into the limit; each folder is asked for once per connection, and a flooded folder or upload request is sent again after the pause instead of failing. After a flood error (`0x020c`) everything pauses for the server's `retry in <n>ms` plus 250 ms, then exactly one message goes and the steady rate follows; a flooded message is sent again up to three times. A message without any answer within 5 s is never counted as sent: it fails, and Retry posts the same message again (the files are not uploaded twice).
8. **Seed the cache** with the file and its preview, so the sender sees their own upload at once. Text taken from the chat input as the caption is removed from it only once the caption was posted (`Core::captionSettled`).

## Receiving

- Every TS Media link and plain TeamSpeak file link in the chat gets an entry with sanitised metadata. Entries move through the states *Idle*, *Queued*, *Downloading*, *Ready* and *Failed*.
- Previews and posters download automatically. Images, GIFs and voice messages (at most 16 MB) download automatically up to the configured size; videos and audio files only up to their own limit, which by default means only when the user presses play. Data saver (for all servers or per server) holds every automatic download except previews, and covered spoilers wait for their reveal.
- At most three transfers run at once; previews go before main files. Downloads interrupted by a lost connection are retried once the server is reachable again.
- **Verification.** Once TeamSpeak has closed a downloaded file, a link with `sha` is hashed on the verifier's own threads. A mismatch is checked once more (TeamSpeak may report a transfer complete before the bytes are on disk); if it still differs, the file is deleted and the entry fails with *File doesn't match what was sent*, without Retry. A preview whose `ph` doesn't match is not shown.
- The image shown for an entry is the best one available: the decoded file, then the preview or poster, then the BlurHash placeholder. Videos have no decoded still: they show their poster or placeholder until they play.
- **Albums.** Links with the same `al` from the same sender are grouped into one grid object, anchored after the first member's link. The other members' links are folded away, and follow-up messages that hold nothing but album links (and the note) are hidden with `QTextBlock::setVisible`. Turning previews off, disabling or unloading the plugin restores every folded link and hidden block. The sender's own albums are recorded at post time, so they group without depending on TeamSpeak echoing the sender's identity.
- Downloads go into a cache under `<data>/cache/<server hash>/<channel>/`. When the cache grows beyond its limit, the least recently used files are deleted, except media that is playing or open in the viewer. Dragged-out files are hard links (or copies) in `<data>/export/`, marked with a `Zone.Identifier` stream and removed after 24 hours.

## Between plugins

Copies of TS Media chat 2.2 talk to each other with TeamSpeak plugin commands (`sendPluginCommand` / `onPluginCommandEvent`). Receivers see the plugin name `tsmedia`, the DLL name without its architecture suffix.

```text
command = "tsm1" SP type *(SP key "=" value)      ASCII only, at most 32 tokens
HELLO pv=1 v=2.2.0 caps=p,r rr=1                  I'm here (to the channel, or to one private-chat partner)
HI    pv=1 v=2.2.0 caps=p,r                       the answer, only to whoever asked
BYE                                               presence switched off, or the plugin unloads
R     s=c|p [y=1] i=<key>:<codes>,...             my complete reaction set on each media item (idempotent)
SYNC  s=c m=<key>,...                             a late joiner asks present members for their own reactions
```

- Unknown types and fields are ignored, and so is `tsm2` and later, so newer versions can add to the protocol. A command we send is at most 900 bytes; a received one above 2048 bytes is dropped before it is copied. Values are limited to `[A-Za-z0-9._,:-]`.
- **The sender's identity is never in the payload:** receivers take the invoker the server fills in. ServerQuery clients are ignored for `R` and `SYNC`. A channel reaction counts only from someone in our channel right now, a private one only from the partner of an open private chat and only for media seen in that chat.
- **PluginLink** keeps one queue per connection (presence before reactions before background work). Each command carries its own return code; no answer within about 3 s counts as not sent, and a late `Ok` is still delivered. A permission error marks the server as blocking plugin commands, and presence and reactions stay off there. Commands share the connection's `FloodGovernor` with chat posts.
- **Presence** (`PeerDirectory`): entering a channel starts an 800 ms settle time, then one `HELLO` with `rr=1` goes to the channel. Members answer with one `HI` after a random 150–900 ms (answers to several joiners merge). A member without an answer within 4 s (6 s for someone who just joined) counts as without TS Media; 2.1 and older can't answer and count the same. Private chats get one `HELLO` per partner at most once a minute; the server chat never asks anyone. Nothing is kept on disk. Turning presence off drops queued `HELLO`/`HI` and sends `BYE`.
- **Reactions** (`ReactionStore`): one 6-bit set per reactor and media key; a new `R` replaces it, an empty set removes it. Your own clicks are applied at once, sent debounced, and taken back if they couldn't be sent. After entering a channel a `SYNC` asks the present members; each answers with its own reactions only, never anyone else's. `reactions.json` keeps at most 500 media and 100 reactors each for 30 days and is always saved within the 2 MB its loader accepts.

## Untrusted input

Anyone can post a `ts3file://` link with any parameters, or send a plugin command, so every value from the chat is treated as untrusted:

- `w` and `h` are clamped to 1–16384 and the aspect ratio to 1:8–8:1; `d` must not be negative.
- A BlurHash longer than 120 characters, or one that doesn't validate, is ignored.
- `pv` must be an absolute path without `..`, in the same channel, and must not point at the main file. A preview transfer above 5 MB is aborted.
- The 2.2 fields are accepted only in their exact form (table above); one hostile-input table drives the tests of every feature that reads them.
- Images above 80 megapixels are never decoded; the size is checked before decoding.
- Displayed names lose control and bidirectional-text characters, and file names are sanitised (`filenames::safeLocalFileName`) before they are used on disk.
- Programs, scripts and other file types Windows runs on double-click are never opened, only shown in Explorer.
- Plugin commands are parsed by `peerprotocol` field by field, rate-limited per sender and size-limited before anything is stored.

## Updates

The update check is built only with `TSMEDIA_UPDATER=ON` (official signed releases). A signed manifest (ECDSA P-256, keys built into the DLL) names the size and SHA-256 of each raw `.update` asset; the plugin builds every URL itself, follows redirects only to GitHub hosts, checks each file's PE machine and that its imports resolve in the running TeamSpeak, and installs by renaming the loaded DLL aside. A boot counter in `ts3plugin_init` rolls back a version that never finished starting. Everything else, including the manifest format, is in [updates](UPDATES.md).

## Threading and unloading

- TeamSpeak calls the plugin on its own threads; every callback is moved onto the GUI thread before it touches Qt objects. The static plugin-command callbacks hold a mutex from reading the hub pointer through queuing the call, so a callback never reaches a hub that is being deleted.
- Probing, hashing, compression, expensive image decodes, the editor's export, diagnostics and voice capture and encoding run on worker threads, so the GUI thread is never blocked for long.
- TeamSpeak can unload the DLL while it keeps running. Nothing may outlive plugin shutdown: objects own their timers (delayed callbacks use `singleShotOwned()` from `src/ownedtimer.h`: a delayed `QTimer::singleShot` belongs to Qt's event dispatcher, not to its context object), worker threads are joined, Media Foundation players are shut down synchronously and the plugin's windows are closed.
- `ts3plugin_shutdown` runs in this order: `BYE` to everyone we said hello to and `reactions.json` written (`PeerHub::prepareShutdown`); the diagnostics hooks cleared; the updater (cancels a check or download, waits at most 1 s); every `tsmedia*` top-level window (the send window, the editor, the voice recorder, the viewer, the diagnostic info, the update dialogs, each waiting for its own worker); `ChatIntegration` (the voice controller first, which joins the recorder's worker and gives TeamSpeak's microphone back, then the inline players); leftover `tsmedia*` widgets inside TeamSpeak's chats; the server group helper; `PeerHub` (store, presence, transport); `Core` (joins the hash and transcode pools, removing a partial transcode); `mf::shutdown`; and last the plugin log.
- Log lines tag their private values (`ts3::log(level, sch, "Uploaded %1", {ts3::file(path)})`). TeamSpeak's log gets plain text; the plugin's own log, `<data>/logs/tsmedia.log` (256 KB, one backup), keeps the tags so the diagnostic info can leave names out.
- Strings that end up in Qt or TeamSpeak state outliving the DLL (settings, style sheets, the clipboard, chat documents) are always heap-allocated through Qt, never `QStringLiteral` data stored in the DLL image. Otherwise TeamSpeak would crash on exit after the plugin was unloaded.
