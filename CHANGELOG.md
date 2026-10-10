# Changelog

All notable changes to **TS Media chat** are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Version 2.0.6 is the first public release. Earlier versions were internal builds without a public release; they are listed briefly at the end.

## [2.2.0] - 2026-10-10

### Added

- **Send window** for drops, <kbd>Ctrl</kbd>+<kbd>V</kbd> and the file picker: it shows what will be sent and where, marks what can't be sent, and says how many people here will see it in the chat.
- Captions of up to 300 characters, shown above the file. A caption too long for the file's message is sent as its own message right above it.
- Spoilers: pictures, GIFs and videos marked in the send window stay blurred in the chat and the viewer until clicked. *Show spoilers without blurring* turns this off.
- Albums: 2 to 10 pictures and videos sent together appear as one grid (up to six tiles, *+N* on the last).
- Picture editor in the send window (**Edit…**): crop, rotate, pen, arrow, rectangle, text, pixelate or black box, undo and redo. Edited copies are sent without photo metadata.
- Video compression: videos over 25 MB, and iPhone (HEVC), AV1, VP9 and camcorder videos, become an MP4 before the upload. Each video has a *Quality* list, and the upload panel offers **Send original**.
- Voice messages of up to 5 minutes: **Record voice message…**, `/tsmedia voice` and a hotkey. Listen before sending; your TeamSpeak microphone is muted while you record (a setting).
- Audio files and voice messages play in the chat, with a seek bar; only one video or audio plays at a time.
- Reactions (thumbs up, heart, laughing, surprised, sad, fire) under pictures, GIFs, videos, albums and audio, seen by people with TS Media who are online in the same chat.
- Downloads are checked against the SHA-256 in the link. A file that was replaced on the server is blocked: *File doesn't match what was sent*, with **Copy details**.
- Drag pictures, videos and files out of the chat or the viewer to a folder or another app, or use **Copy file** (<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>C</kbd> in the viewer). The copies carry Windows' "downloaded from the internet" mark.
- Data saver: pause automatic downloads for all servers or for one (Plugins menu, `/tsmedia datasaver`).
- Settings per server (**Settings → Servers**): automatic downloads, upload folder, upload size limit and the note for people without the plugin.
- Server access for admins: create or repair the `tsmediachat` server group with one click, and **Give / Remove TS Media chat access** in a client's right-click menu.
- Automatic updates from GitHub, after you agree once: a daily check, signed downloads, **Restart TeamSpeak now**. Also **Check for updates…** and `/tsmedia update`.
- **Diagnostic info** to copy into a bug report (`/tsmedia diag`, **Settings → General**); file names are left out unless you include them.
- **Settings → Privacy & updates**: *Show reactions on media* and *Tell people in your channel that you have TS Media*.
- The plugin keeps its own log, `%APPDATA%\TS3Client\plugins\tsmedia\logs\tsmedia.log`.

### Changed

- Dropping files opens the send window. Hold <kbd>Ctrl</kbd> to send right away, or choose *Send right away* in **Settings → Sending**.
- Text already typed in the chat input becomes the caption when you press <kbd>Ctrl</kbd>+<kbd>V</kbd>.
- Video compression is on by default (*Compress videos larger than 25 MB*, *Balanced (720p)*).
- A video over your upload limit gets a smaller quality that fits, instead of being skipped.
- Your TS Media version is shared with your channel so the send window can count who has the plugin; turn it off in **Settings → Privacy & updates**.
- The settings are on tabs: General, Sending, Receiving & playback, Servers, Privacy & updates.
- *Download videos automatically* is now *Download videos and audio automatically*; voice messages download like images (at most 16 MB).
- Files sent together can share one chat message, and messages wait for TeamSpeak's flood protection instead of being lost.
- Chat links read *Spoiler (image)* for spoilers and *Voice message (0:12)* for voice messages.
- Bug report form: a new *Diagnostic info* field; the plugin load line is only needed when the plugin doesn't load.

### Fixed

- A chat message TeamSpeak never confirmed counted as sent: now it is reported, and **Retry** posts it again without a second upload.
- Seeking a video after it ended started it again by itself.

## [2.1.0] - 2026-10-09

### Added

- `/tsmedia cancel` and a *Cancel all uploads* hotkey stop every running upload; `/tsmedia help` lists the commands, one per line, and an unknown command is named.
- Upload panel: **Retry** for failed uploads, and for several files a header with the overall progress, the time left and **Cancel all**.
- While files are dragged onto the chat, it shows where they will go (with the Shift hint), or why nothing can be sent there.
- Tooltips on cards, failed previews and inline video controls; the seek bar shows the time under the pointer.
- Failed cards have a **Retry** button.
- Gallery viewer: <kbd>Tab</kbd> / <kbd>Enter</kbd> reach the bottom-bar and message buttons, and every player control has its own key; new keys (<kbd>L</kbd> loop, <kbd>+</kbd> / <kbd>−</kbd> zoom, arrows move a zoomed picture, <kbd>Ctrl</kbd>+<kbd>O</kbd> open with default app) and a shortcut list (**?** button, <kbd>?</kbd> or <kbd>F1</kbd>).
- Gallery viewer: **Get it from Microsoft Store** when a video needs a decoder from the Store, and **Retry** / **Open with default app** right under an error.
- Settings: an **Apply** button.
- Confirmations for *Copy link*, *Copy image* and *Save as…* (*Saved to Pictures*), and *Diagnostics saved to …* for `/tsmedia debug`.

### Changed

- Clearer, higher-contrast previews, file cards and plugin messages in light and dark TeamSpeak themes. Cards are 2 px taller.
- Previews and cards react to hover and clicks. Previews a click can't help (deleted files, password-protected channels) keep the normal pointer and do nothing when clicked.
- Previews and cards show whether a file is waiting to download or downloading.
- Inline video: the controls stay while the pointer rests on them and then fade out; small players get a smaller play button. A video Windows can't play inside the chat says *Opens in default app*, and its right-click menu offers *Open in default app*.
- Programs and scripts from the chat are shown in their folder instead of opened (the card says *Show in folder* once downloaded).
- Animations follow the Windows "Show animations" setting: with it off, GIFs play only while hovered and loading indicators stand still.
- Previews are drawn again, sharp and in the right colours, after the window moves to a monitor with another scale or the theme changes.
- Gallery viewer: 100% now shows one image pixel per screen pixel, so on scaled displays pictures look smaller at 100% than before. Double-click zooms into the clicked point; Fit and 100% show which view is active; large downscales look smoother.
- Gallery viewer: full screen keeps a header with the name, position, Copy, Save and Open, and the controls and the pointer hide there for pictures too. Errors say the cause first; playback errors no longer call audio files videos.
- Upload panel: follows TeamSpeak's light or dark theme, shows the speed, the amounts and the time left, marks waiting uploads, lists at most four files (failed first, the rest as *+N more*), and keeps failures on screen long enough to read (and never removes one while the pointer is on the panel).
- Files sent together are posted in the order they were chosen, with at most two uploads at a time. A file waiting for earlier ones can still be canceled, and *Cancel all* stops it too.
- Paste dialog: a thumbnail or the file's type icon, the size of each file and the total; empty files and files over the upload limit are marked and skipped (*Send 7 files*).
- Being disconnected, a password-protected channel or an unknown private chat partner is reported before the file picker, the paste dialog or a drop, not after.
- The menu item and the hotkey are called *Send files to chat…* / *Send files to the current chat*; the menu item is greyed out while the current server tab is not connected.
- Plugin messages in the chat appear in the tab you are looking at on that server, with colours readable in light and dark themes.
- Settings dialog: regrouped into Receiving, Playback, Media cache, Sending and Note for people without the plugin, with clearer labels and helper texts, a link check right under the field, focus rings in TeamSpeak's dark skins and a *Clear cache* that says how much it deletes.
- An empty *Upload folder* now means `/tsmedia`; type `/` for the top level of the channel's file browser.
- A lower cache size limit applies as soon as the settings are saved.
- Only the settings you changed are saved, so the volume set in the viewer is no longer overwritten.
- Consistent, clearer wording; error messages say what to do next.
- File names are shown without the random part added on upload, and pasted pictures as *Pasted image*.
- The note for people without the plugin is easier to read on TeamSpeak's default white chat.

### Fixed

- TeamSpeak could crash while closing when it was closed soon after starting or within about 30 seconds of sending a file (also in 2.0.x).
- Plugin messages in the chat showed `[noparse]` tags around their text.

## [2.0.7] - 2026-10-09

### Changed

- The note for people without the plugin is always in English.

### Removed

- The Persian translation and the *Language* setting; the interface is English only. A language chosen in an earlier version is ignored.

## [2.0.6] - 2026-10-09

**First public release.**

### Changed

- The project lives at https://github.com/Metihttp/Teamspeak_Media_chat. "TS Media chat" in the plugin-required note and the default *Download link* setting point there (links to the previous address are redirected by GitHub).

## [2.0.5] - internal build

### Fixed

- Images and videos received from remote servers were sometimes cached incomplete and showed *This image can't be shown*. The plugin now waits until TeamSpeak has finished writing a download, and damaged copies from earlier versions are downloaded again automatically.

## [2.0.4] - internal build

### Fixed

- TeamSpeak's installer could fail with `Failed to extract: plugins`.

## [2.0.3] - internal build

### Changed

- "TS Media chat" in the note for people without the plugin links to the GitHub page. The link can be changed in Settings → Sending → *Download link*.

## [2.0.2] - internal build

### Changed

- The plugin is now called **TS Media chat**; the author is **MehdiHttp**.

## [2.0.1] - internal build

### Added

- Version information in the DLL (file properties → Details).

### Changed

- The `.ts3_plugin` package uses the same layout as official packages from myteamspeak.com.

## [2.0.0] - internal build

### Added

- Inline video player in the chat (Windows Media Foundation) and animated GIFs.
- Gallery viewer with zoom, a full video player and keyboard shortcuts.
- Size, duration and a BlurHash placeholder in the posted link, so the chat never jumps while media loads; small previews and video posters for large files.
- Right-click menu on previews.
- A note for people without the plugin, hidden from people who have it.
- Persian translation (removed in 2.0.7).
- Cache size limit; 64-bit and 32-bit DLLs in one package.

### Fixed

- Stability fixes, among them a crash when TeamSpeak exited after the plugin DLL had been unloaded, and name collisions between uploads.

## [1.0.0] - internal build

### Added

- Files are uploaded to the channel's file browser (`/tsmedia`) and posted as a normal `ts3file://` link that works for everyone.
- Inline image previews and file cards for users with the plugin.

[2.2.0]: https://github.com/Metihttp/Teamspeak_Media_chat/compare/v2.1.0...v2.2.0
[2.1.0]: https://github.com/Metihttp/Teamspeak_Media_chat/releases/tag/v2.1.0
[2.0.7]: https://github.com/Metihttp/Teamspeak_Media_chat/releases/tag/v2.0.7
[2.0.6]: https://github.com/Metihttp/Teamspeak_Media_chat/releases/tag/v2.0.6
