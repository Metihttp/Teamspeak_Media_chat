# Privacy and security

[← Back to README](../README.md)

Where your files go, what stays on your computer, and how the plugin protects you from what others post.

## Where your files go

- **Files only go to your TeamSpeak server.** Uploads and downloads use TeamSpeak's own file transfer into the channel's file browser. There is no other server or cloud service.
- **No telemetry, and only one kind of web request.** Official builds can check GitHub for updates, but only after you turn that on (it asks once, at most twice); see [Updates](UPDATES.md) for exactly what is sent. Otherwise the only web address involved is the link in the note for people without the plugin, which opens only if someone clicks it.
- **The plugin's own log** (`%APPDATA%\TS3Client\plugins\tsmedia\logs\tsmedia.log`, at most about 512 KB) holds the plugin's messages, including the names of files you send and receive. It stays on your computer; the diagnostic info leaves those names out unless you include them.
- **Channel members can open what you send.** Anyone on the server with download permission for that channel can get the file. Treat sent files like any other file in the file browser: they stay on the server until someone deletes them.

## What stays on your computer

- **Local cache:** downloaded media is kept in `%APPDATA%\TS3Client\plugins\tsmedia\cache`, limited by the cache size you choose. You can clear it at any time in the settings.
- **Temporary copies:** files being sent are staged in the `upload` and `paste` folders next to the cache. They are deleted once they have been sent or dismissed, and at the latest at the next start. Pictures edited in the send window are written to the `edit` folder there and handled the same way.
- **Photo metadata:** a picture you edit in the send window (crop, draw, hide details) is sent as a new file without any metadata: no EXIF, so no camera details and no location. Pictures you send without editing are sent exactly as they are, so a phone photo can still contain where it was taken.
- **Hiding details:** *Black box* covers an area with solid black. *Pixelate* replaces it with coarse blocks (with a little fixed noise so the text can't be guessed back), but large text can stay partly readable; use *Black box* for passwords, codes and addresses.
- **Dragged and copied files:** dragging a file out of the chat or using *Copy file* makes a copy with a clean name in the `export` folder next to the cache (a hard link where possible). These are removed after 24 hours, even if the cache limit removed the cached file earlier. Like downloads from a browser, they carry Windows' "downloaded from the internet" mark, so Windows warns before running a program from the chat.
- **Diagnostic info** (`/tsmedia diag`) is built only when you open it and leaves your computer only if you paste it somewhere. It shows exactly what *Copy* copies: plugin, Qt, TeamSpeak and Windows versions, which video and audio codecs Windows has, your settings (custom folders and links only as "changed"), counts of this session's transfers and the recent TS Media lines of the log. File names and paths in those lines read `<file 1>`, `<path 1>` unless you tick *Include file names* (your user folder then shows as `%USERPROFILE%`). Server addresses, server and channel names, unique IDs and nicknames are never included. *Copy and open bug report* opens GitHub's form in your browser with only the version fields filled in; the plugin itself sends nothing.

## Nothing is sent by accident

- **Pasting always asks first.** The clipboard may hold something old you forgot about.
- **Dropping can be skipped.** Hold Shift while dropping to get TeamSpeak's normal behaviour, or turn drop sending off in the settings.
- **Private chats stay private.** If the partner of a private chat can't be identified, nothing is sent.

## The microphone (voice messages)

- **Only while you record.** The plugin opens the microphone only after you start a voice message, and only while the *Voice message* window shows *Recording*. If that window is closed, or TeamSpeak unloads the plugin, recording stops at once. Nothing is recorded in the background, and the hotkey can't record without the window appearing.
- **Nothing leaves your computer until you press Send.** The sound is kept in memory, written to `%APPDATA%\TS3Client\plugins\tsmedia\voice` when you stop, and sent through TeamSpeak's file transfer like any file. Discarded recordings are deleted right away; sent ones are deleted with their upload, and leftovers after a day.
- **Your TeamSpeak microphone is muted while you record** (a setting), and others can see that. It is unmuted again when recording stops; it is never unmuted where you muted it yourself. If something goes wrong (TeamSpeak crashes, the connection drops), it stays muted rather than open.
- The recording is the raw microphone: TeamSpeak's noise suppression and echo cancellation don't apply to it.

## Received programs are never run

`.exe`, `.bat`, `.cmd`, `.msi`, `.ps1`, `.vbs`, `.js`, `.lnk`, `.jar`, `.dll`, `.reg`, `.scr`, `.ts3_plugin` and the other types Windows runs on double-click are only shown in Explorer. Trailing-dot and trailing-space tricks (such as `file.exe.`) are caught too.

## Chat links are untrusted input

Anyone can type a TeamSpeak file link with fake metadata, so the plugin checks everything a link claims:

- Dimensions are clamped to 16384 px and to an aspect ratio of at most 8:1.
- Invalid BlurHash placeholders, and ones longer than 120 characters, are ignored.
- Preview paths must be absolute, in the same channel and without `..`.
- Preview downloads above 5 MB are aborted.
- Images above 80 megapixels are never decoded. They show *Can't preview this image* and can still be opened in your default app.
- Expensive decodes run off TeamSpeak's user interface thread.
- File names are sanitised before they touch the disk.
- Control and bidirectional-text characters are removed from displayed names, so a name can't disguise its real extension.
- A download whose size doesn't match the link is discarded.

## Automatic downloads are limited

By default, only images and GIFs up to 15 MB download automatically, plus small previews. Videos are only downloaded when you press play. Both limits can be changed in the [settings](SETTINGS.md#receiving).

## The DLL is not code-signed

The plugin DLL is not signed with a code-signing certificate, so some antivirus products may flag it. If you prefer, [build it yourself](BUILDING.md) from this source code.
