# Changelog

All notable changes to **TS Media chat** are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Version 2.0.6 is the first public release. Earlier versions were not released on GitHub, so they have no release date.

## [2.0.7] - 2026-10-09

### Removed

- The Persian user interface and the UI language setting; the plugin is English only (an old language entry in
  `settings.ini` is ignored).

### Changed

- The plugin-required note is always in English.

## [2.0.6] - 2026-10-09

### Changed

- The project lives at https://github.com/Metihttp/Teamspeak_Media_chat. "TS Media chat" in the plugin-required note and the default
  *Download link* setting point there (links to the previous address are redirected by GitHub).

## [2.0.5] - 2026-10-09

### Fixed

- Images and videos received from remote servers were sometimes cached incomplete: the file had the right size but ended
  in zeros, and the viewer showed *This image can't be shown*. TeamSpeak may still be writing a download when it reports
  the transfer as complete; the plugin now waits until the file is no longer open for writing (checked every 100 ms, for
  up to 30 s) before taking it into the cache.
- Damaged copies cached by earlier versions are detected (an image ending in 4 KB of zeros, a video ending in 64 KB of
  zeros) and downloaded again automatically. The same check applies to cached previews and video posters.

## [2.0.4] - not released publicly

### Fixed

- TeamSpeak's package installer failed with `Failed to extract: plugins`. It decides whether a zip entry is a folder from
  its MS-DOS attributes, so the `plugins/` folder entry of the `.ts3_plugin` package now carries the directory attribute
  (files carry the archive attribute, as in packages from myteamspeak.com).

## [2.0.3] - not released publicly

### Changed

- "TS Media chat" in the note shown to people without the plugin now links to the GitHub page
  (`https://github.com/Metihttp/Teamspeak_Media_chat`). The link can be changed in Settings → Sending → *Download link*; an empty
  value means the GitHub page.

## [2.0.2] - not released publicly

### Changed

- The plugin is now called **TS Media chat**; the author is **MehdiHttp**.

## [2.0.1] - not released publicly

### Added

- Version information in the DLL (file properties → Details: product name, version, author).

### Changed

- The `.ts3_plugin` package uses the same layout as official packages from myteamspeak.com: `package.ini` first, then an
  explicit `plugins/` folder entry, then the DLLs.

## [2.0.0] - not released publicly

### Added

- Inline video player in the chat (Windows Media Foundation): poster with play button and duration, play/pause, seek bar,
  mute and expand; only one video plays at a time.
- Animated GIFs (and animated WebP) inside the chat.
- Gallery viewer: every media item of the chat, zoom and pan, a full video player (volume, loop, full screen) and keyboard
  shortcuts.
- Media metadata in the posted link (size, duration and a BlurHash placeholder) so the chat never jumps while media loads.
- Small previews and video posters uploaded with large images and videos (`/tsmedia/previews`).
- Right-click menu on previews: open, open with default app, save as, copy image, copy link, show in folder, download /
  retry, play / pause, mute.
- A note for people without the plugin ("plugin required to view this in chat"), hidden from people who have it.
- English and Persian user interface, right-to-left in Persian.
- Cache size limit with least-recently-used cleanup.
- The package contains both the 64-bit and the 32-bit DLL.

### Fixed

- Fixes from a code review, among them a crash when TeamSpeak exited after the plugin DLL had been unloaded, and name
  collisions between uploads (names get a random part, existing files are never overwritten, and an upload is renamed when
  the server reports that the name is taken).

## [1.0.0] - not released publicly

### Added

- Files are uploaded to the channel's file browser (`/tsmedia`) and posted as a normal `ts3file://` link that works for
  everyone.
- Inline image previews and file cards for users with the plugin.

[2.0.7]: https://github.com/Metihttp/Teamspeak_Media_chat/releases/tag/v2.0.7
[2.0.6]: https://github.com/Metihttp/Teamspeak_Media_chat/releases/tag/v2.0.6
