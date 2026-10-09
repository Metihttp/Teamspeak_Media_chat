# Changelog

All notable changes to **TS Media chat** are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Version 2.0.6 is the first public release. Earlier versions were internal builds without a public release; they are listed briefly at the end.

## [2.1.0] - unreleased

### Changed

- Clearer, higher-contrast previews and file cards in light and dark TeamSpeak themes.
- Previews react to hover and clicks, and show tooltips.
- Previews and cards show whether a file is waiting to download or downloading.
- More reliable inline video controls.
- Gallery viewer: works from the keyboard with a visible focus, has a full-screen header with the file name and counter and an overview of its keyboard shortcuts, and confirms Copy and Save more clearly.
- Animations follow the Windows "Show animations" setting.
- Upload panel: failed uploads can be retried, waiting uploads are marked as queued, the time left is shown, and the list scrolls when many files are sent.
- Files sent together are posted in the order they were chosen, with at most two uploads at a time.
- Dragging files onto the chat shows that they will be sent.
- Clearer paste confirmation dialog.
- Reorganised settings dialog that checks values as you type.
- Consistent, clearer wording; error messages say what to do next.
- File names are shown without the random part added on upload, and pasted pictures as *Pasted image*.
- The note for people without the plugin is easier to read on TeamSpeak's default white chat.

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

[2.0.7]: https://github.com/Metihttp/Teamspeak_Media_chat/releases/tag/v2.0.7
[2.0.6]: https://github.com/Metihttp/Teamspeak_Media_chat/releases/tag/v2.0.6
