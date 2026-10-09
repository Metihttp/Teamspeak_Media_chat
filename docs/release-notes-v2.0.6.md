![TS Media chat: inline video player, images, file cards and animated GIFs in the TeamSpeak 3 chat](https://github.com/Metihttp/Teamspeak_Media_chat/raw/v2.0.6/docs/images/hero.png)

**TS Media chat** is a plugin for the TeamSpeak 3 client on Windows that makes the chat feel like Discord. Drop a file on
the chat or paste a screenshot with **Ctrl+V**: it is uploaded to the channel's file browser on your TeamSpeak server and a
normal TeamSpeak file link is posted. People with the plugin see images, animated GIFs and videos right in the chat, with an
inline player and a gallery viewer. People without it get the usual download link and a short note that the plugin is needed.

This is the **first public release**.

## Install

1. Download **`TSMedia-2.0.6.ts3_plugin`** from the assets below.
2. Close TeamSpeak (recommended; required when updating, because the old DLL is locked while TeamSpeak runs).
3. Double-click the file. In TeamSpeak's installer click **Install**, and answer **Yes** when it asks whether to activate
   the add-on.
4. Start TeamSpeak. The plugin is enabled (otherwise: **Tools → Options → Addons**). Type `/tsmedia` in the chat to see
   the version and commands.

Requirements: **TeamSpeak 3 Client 3.6.x** on Windows. The package contains a 64-bit and a 32-bit DLL. **TeamSpeak 5 and 6
are not supported.** Your server group needs file transfer permissions (`i_ft_file_upload_power`,
`i_ft_file_download_power`, and `i_ft_directory_create_power` for the `/tsmedia` folder). See the
[README](https://github.com/Metihttp/Teamspeak_Media_chat#server-admin-guide) for the server admin guide.

## What it does

- Send files by drag & drop, Ctrl+V (with a confirmation), the Plugins menu, a hotkey or `/tsmedia send`, in channel,
  server and private chats.
- Files are stored on your TeamSpeak server, in the channel's file browser (`/tsmedia`, previews in `/tsmedia/previews`).
- Inline images with correctly sized BlurHash placeholders, animated GIFs, an inline video player (play/pause, seek, mute,
  expand) and file cards.
- Gallery viewer with zoom, a full video player, full screen and keyboard shortcuts; a right-click menu on every preview.
- English and Persian interface, light and dark themes, a cache with a size limit.
- Received executables are never launched, and link metadata from the chat is treated as untrusted.

## What's new in 2.0.5 / 2.0.6

- The project lives at https://github.com/Metihttp/Teamspeak_Media_chat; "TS Media chat" in the plugin-required note links there.

- **Fixed:** images and videos received from remote servers were sometimes cached incomplete (zeros at the end of the file,
  shown as *This image can't be shown*). The plugin now waits until TeamSpeak has finished writing a download before using it.
- **Fixed:** damaged copies cached by earlier versions are detected and downloaded again automatically.

## Earlier 2.0.x changes (included)

- **2.0.4:** fixed `Failed to extract: plugins` in TeamSpeak's installer.
- **2.0.3:** "TS Media chat" in the note for people without the plugin links to the GitHub page.
- **2.0.2:** renamed to **TS Media chat**, author **MehdiHttp**.
- **2.0.1:** version information in the DLL; package layout like official TeamSpeak packages.
- **2.0.0:** inline video player, animated GIFs, gallery viewer, BlurHash placeholders, previews and posters, Persian / English
  interface, cache size limit, 32-bit + 64-bit package, plus stability fixes (e.g. a crash on exit after the DLL was unloaded,
  name collisions between uploads).

Full history: [CHANGELOG.md](https://github.com/Metihttp/Teamspeak_Media_chat/blob/v2.0.6/CHANGELOG.md)

## Known issues

- **Antivirus / "Bad Image" `0xc0000020`:** the DLL is not code-signed and some antivirus products may flag or damage it.
  "Bad Image 0xc0000020" means the DLL file is damaged: close TeamSpeak, reinstall the package and add an antivirus
  exclusion for `%APPDATA%\TS3Client\plugins` if it happens again.
- **32-bit TeamSpeak:** the 32-bit DLL is included but has not been tested on a real 32-bit client yet.
- **Default Guest group:** on a default server the Guest group cannot upload; an admin has to grant `i_ft_file_upload_power`.
- **Password-protected channels:** uploading there is not supported yet.
- **Video codecs:** H.264 plays out of the box; HEVC, VP9 and AV1 each need their video extension from the Microsoft
  Store, and Windows N editions need the Media Feature Pack. A video that can't be played inside the chat opens in your
  default video app instead.
- Other people need the plugin too to see media inline; without it they get the link and a short note.
