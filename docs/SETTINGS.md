# Settings

[← Back to README](../README.md)

Every option of TS Media chat, with its default and its range.

<p align="center">
  <img src="images/settings.png" width="640" alt="The TS Media chat settings with the tabs General, Sending, Receiving and playback, Servers, and Privacy and updates">
</p>

Open the settings with **Plugins → TS Media chat → Settings…**, by typing `/tsmedia settings` in the chat, or with the plugin's Settings button in **Tools → Options → Addons**.

| Tab | Groups |
| --- | --- |
| [General](#general-tab) | Media cache, Troubleshooting (*Diagnostic info…*), Voice messages |
| [Sending](#sending-tab) | Sending, Note for people without the plugin, Dropping files, Videos |
| [Receiving & playback](#receiving--playback-tab) | Receiving (with data saver), Playback, Spoilers |
| [Servers](#servers-tab) | Servers (settings for single servers), Server access (for server admins) |
| [Privacy & updates](#privacy--updates-tab) | Privacy (reactions and presence), Updates |

- <kbd>Ctrl</kbd>+<kbd>Tab</kbd> and <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Tab</kbd> switch tabs, and the dialog opens on the tab you used last. On a small screen the tabs scroll; the window keeps its size when you switch tabs.
- **OK** saves and closes, **Apply** saves and keeps the dialog open, **Cancel** closes without saving.
- **Restore defaults** only fills in the default of every option; nothing is saved until you click **OK** or **Apply**. It never touches your update choice, a skipped version, the settings of single servers or anything done in *Server access*.
- Only the options you changed are saved. The volume you set in the gallery viewer is kept, and the dialog follows it.
- Options that only matter when another one is on are indented under it and greyed out while it is off.

Settings are stored in `%APPDATA%\TS3Client\plugins\tsmedia\settings.ini` (a portable TeamSpeak uses the `config` folder inside the TeamSpeak folder instead of `%APPDATA%\TS3Client`). A value outside its range is corrected to the nearest allowed value when the plugin starts.

## General tab

### Media cache

| Setting | Default | Range and notes |
| --- | --- | --- |
| Size limit | 1024 MB | 100 MB to 1 TB, in steps of 256 MB. |

- Beyond the limit, the media you haven't opened for the longest time is removed first. Media that is playing or open in the viewer never is. A lower limit applies as soon as you click **OK** or **Apply**.
- The space in use is shown under the limit, in the same unit (*700 MB of 1024 MB used (68%)*), with **Open folder** and **Clear cache** buttons.
- **Clear cache** asks first and says how much it will delete. Files on the server aren't affected: media still in the chat downloads again when you view it. Media that is playing or open in the viewer is kept. The reactions saved on this computer are removed too; people online share theirs again.
- The cache lives in `%APPDATA%\TS3Client\plugins\tsmedia\cache`.

### Troubleshooting

**Diagnostic info…** opens the window that `/tsmedia diag` opens: versions, settings, session counts and recent plugin messages to paste into a bug report. Nothing is sent by the plugin; see [privacy & security](PRIVACY-SECURITY.md#what-stays-on-your-computer).

### Voice messages

See [voice messages](USAGE.md#voice-messages) for how recording works.

| Setting | Default | Range and notes |
| --- | --- | --- |
| Microphone | Same as TeamSpeak (recommended) | Or one of Windows' microphones by name. When the plugin can't tell which microphone TeamSpeak uses, it records from Windows' default communications microphone and the recording window says so. A microphone picked here that is unplugged shows as *not connected*; until it is back, the TeamSpeak one is used. |
| Mute my TeamSpeak microphone while recording | on | So people in your channel don't hear you live (voice activation). Others see your microphone as muted until you finish. It is always unmuted again when recording stops, unless you unmuted it yourself meanwhile. |
| Let me listen before sending (hotkey) | on | Off: the hotkey's second press sends right away instead of stopping for a listen. |
| Play a sound when recording starts and stops | on | A short beep through TeamSpeak's playback device; only you hear it. |

The line under the options shows the record hotkey (*Record hotkey: F9*, or *not set*); **Set up hotkeys…** opens TeamSpeak's hotkey setup, where it is listed under TS Media chat as *Record a voice message*.

## Sending tab

### Sending

| Setting | Default | Range and notes |
| --- | --- | --- |
| Send files dropped on the chat (hold Shift for TeamSpeak's own drop) | on | |
| Send screenshots and files pasted into the chat input (Ctrl+V) | on | Opens the send window; nothing is sent before you click **Send**. |
| Convert pasted images over 2 MB to JPEG | on | Images with transparency stay PNG. |
| Upload a small preview with large images and videos | on | Others see a sharp preview while the full file downloads. |
| Upload size limit | 100 MB | 1–4096 MB. |
| Upload folder | `/tsmedia` | A folder in the channel's file browser, created when needed. Empty means `/tsmedia`; type `/` for the top level of the channel's file browser. |

- **Convert pasted images over 2 MB to JPEG** uses JPEG quality 90. It also applies to edited pictures saved as PNG.
- **Upload a small preview**: the [usage guide](USAGE.md#file-names-and-folders) lists which files get a preview and where it is stored.
- **Upload size limit** is your own limit. The server's transfer quotas still apply. The send window marks files over the limit before you click **Send**.
- **Upload folder** is per user: each person sends into the folder set in their own settings. Without permission to create folders, files go to the channel root.
- Upload size limit and upload folder can be different for a single server: see [Servers](#servers).
- The picture editor (*Edit…* in the send window) remembers the colour, size and *Hide details* mode you used last. They are kept in `settings.ini` (`editorColor`, `editorStroke`, `editorHideMode`) and have no control in this dialog.

### Note for people without the plugin

| Setting | Default | Range and notes |
| --- | --- | --- |
| Add a note after files you send | on | People without the plugin see *TS Media chat plugin required to view this in chat* after the link. |
| Link in the note | the project's GitHub page | Where "TS Media chat" in the note links to. |

- **Link in the note** must be an `http://` or `https://` address; `https://` is added when it is missing (you see it once you leave the field). Empty means the official page, `https://github.com/Metihttp/Teamspeak_Media_chat`. The link is checked when you leave the field: a problem (not a web address, another scheme, `[` or `]`, more than 512 characters) is explained right under it. The field can only be edited while the note is on.

### Dropping files

| Setting | Default | Range and notes |
| --- | --- | --- |
| When you drop files on the chat | Open the send window | Or *Send right away*. Hold Ctrl while dropping to do the other; hold Shift for TeamSpeak's own drop. Only matters while *Send files dropped on the chat* is on. |

- **Open the send window** lets you add a caption, mark spoilers, build an album and see what can't be sent before anything goes out. **Send right away** sends dropped files as soon as you let go (as in 2.1), except when none of them could be sent: the window then opens and says why.

### Videos

| Setting | Default | Range and notes |
| --- | --- | --- |
| Compress videos larger than | on, 25 MB | 1–4096 MB. The video becomes an H.264/AAC MP4 before it is uploaded. |
| Convert iPhone (HEVC), AV1, VP9 and camcorder videos too | on | Of any size, and only when this computer can play them. |
| Quality | Balanced (720p) | Smaller (480p, up to 30 fps), Balanced (720p, up to 60 fps) or High (1080p, up to 60 fps). |
| Use the graphics card when possible | on | Off when no graphics card video encoder was found; the processor is used then. |

- Compressing happens on your computer only; nothing is sent anywhere else. One video is compressed at a time, at a lower priority than TeamSpeak's own work.
- A video is only compressed when that makes it at least 30% smaller. A video larger than your **upload size limit** is made small enough to fit when possible (a lower bitrate first, then a smaller picture); the send window then picks that quality by itself.
- The send window has a **Quality** list for each video, with the size each choice is expected to have; *Original* sends the file as it is.
- If compressing fails and the original fits your upload limit, the original is sent and the upload panel says so.
- The result has the same random part in its name, with the extension `.mp4`. Extra sound tracks, subtitles and metadata (including any location data in phone videos) are not kept.
- Needs Windows Media Foundation. On Windows N editions without the Media Feature Pack, videos are sent as they are.

## Receiving & playback tab

### Receiving

| Setting | Default | Range and notes |
| --- | --- | --- |
| Show images, videos and file cards in the chat | on | The other Receiving options only apply while this is on. |
| Data saver: pause automatic downloads | off | Images, videos and audio load when you click them; small previews still load. The limits below are kept. |
| Download images, GIFs and voice messages automatically up to | on, 15 MB | 1–500 MB. Larger images and GIFs load when clicked. Voice messages download up to this size, but never above 16 MB; larger ones load when played. |
| Play GIFs automatically | on | When off, a GIF plays while the pointer is over it. |
| Download videos and audio automatically | Off (download when played) | 0–4096 MB, in steps of 10 MB. Videos and audio files up to this size download automatically. |
| Maximum preview size | 400 × 300 px | Width 120–1200 px, height 80–1200 px. Albums use the same width. |

- With *Show images, videos and file cards in the chat* off, you only see the links. The "plugin required" note stays hidden either way.
- While **data saver** is on, the download options below it are greyed out and say *Paused by data saver*. Data saver can also be set for a single server ([Servers](#servers), the Plugins menu or `/tsmedia datasaver`). It never holds back your uploads, reactions or the update check.
- With *Download videos and audio automatically* at *Off (download when played)*, a video or audio file is only downloaded when you press play.
- With Windows animations turned off (**Settings → Accessibility → Visual effects → Animation effects**), GIFs play only while the pointer is over them, whatever *Play GIFs automatically* says. The dialog shows a note under the option then.

### Playback

| Setting | Default | Range and notes |
| --- | --- | --- |
| Default volume | 80% | 0–100%. For inline videos, audio files, voice messages and the viewer. |
| Start videos muted | off | Audio files and voice messages are never started muted. |
| Loop videos | off | Audio files and voice messages never loop. |

Changing the volume in the gallery viewer updates this setting too.

### Spoilers

| Setting | Default | Range and notes |
| --- | --- | --- |
| Show spoilers without blurring | off | On: pictures and videos marked as spoilers are shown right away. Off: they stay blurred until you click them. |

## Servers tab

### Servers

Gives a single server its own value for four settings. Pick the server at the top (the ones you are connected to come first, then the ones that already have their own settings), then change what should differ:

| Setting | Choices |
| --- | --- |
| Automatic downloads | *Same as all servers*, *On*, or *Paused (data saver)* |
| Upload folder | empty for *Same as all servers*, or a folder |
| Upload size limit | *Same as all servers*, or 1–4096 MB |
| Note for people without the plugin | *Same as all servers*, *Add the note* or *Don't add the note* |

- *Same as all servers* shows the current value in brackets, for example *Same as all servers (100 MB)*.
- **Forget** removes the server's own settings; it then uses the settings for all servers.
- Only servers you change here are remembered, by their ID and name, on this computer (at most 200). Connect to a server to give it its own settings.
- The Plugins menu (**Pause / Resume automatic downloads on this server**) and `/tsmedia datasaver on`, `off` or `default` change the same data saver value.

### Server access

For server admins: creates and repairs the `tsmediachat` server group whose members can send files with TS Media chat, on the server of the tab you're looking at. These actions take effect on the server right away; **OK**, **Apply**, **Cancel** and **Restore defaults** don't affect them. The box contacts the server only while it is visible.

- **Create TS Media chat group** creates the group with its icon, file permissions at power 75 and a copy of the default group's permissions. **Details…** lists every permission first. See the [server admin guide](SERVER-ADMIN.md#one-click-ts-media-chat-group).
- **Repair TS Media chat group** appears when something is missing and adds only that. Permissions your server permissions can't grant are listed as needing a higher admin.
- **Check again** reads the group from the server again.
- People who can't create server groups only see whether they are in the group.

## Privacy & updates tab

### Privacy

| Setting | Default | Range and notes |
| --- | --- | --- |
| Show reactions on media | on | Reaction rows, the add button and *Add reaction*. Off: no reactions are shown or sent. |
| Tell people in your channel that you have TS Media | on | Lets the send window show who will see your media in the chat. Your TS Media version is shared with people in your channel and with private-chat partners you send to. Off: the plugin says goodbye to them, and the send window shows no count. |

Both travel through TeamSpeak's plugin commands only; nothing leaves TeamSpeak. Details: [privacy & security](PRIVACY-SECURITY.md#what-your-channel-sees).

### Updates

| Setting | Default | Range and notes |
| --- | --- | --- |
| Check for updates automatically | off until you agree | The plugin asks once (*Keep TS Media chat up to date?*). On: about once a day it asks GitHub whether there is a new version. Nothing is installed until you click **Update**. |

- The status line shows your version and the last check (*Version 2.2.0 · You have the latest version*), a version that is ready (**Update…**), an update waiting for a restart (**Restart now**) or a skipped version (**Undo**).
- **Check now** checks once, also while automatic checks are off.
- Versions you build yourself show *Updates are turned off in versions you build yourself* instead.
- What is sent and how updates are checked: [updates](UPDATES.md).

## Keys in settings.ini

For reference, the keys 2.2 added. Values outside their range are corrected when the plugin starts.

| Key | Setting |
| --- | --- |
| `dropOpensSendWindow` | *When you drop files on the chat* (`true` = open the send window) |
| `sendAsAlbum` | the send window's last *Send as an album* choice |
| `compressVideos`, `compressVideosOverMB`, `compressVideoQuality` (480, 720 or 1080), `convertUnplayableVideos`, `compressUseGpu` | Videos |
| `editorColor`, `editorStroke`, `editorHideMode` | the picture editor's last colour, size and *Hide details* mode |
| `voiceMicrophone`, `voiceMuteTeamSpeakMic`, `voiceReview`, `voiceSounds` | Voice messages |
| `dataSaver`, `revealSpoilers` | data saver, *Show spoilers without blurring* |
| `showReactions`, `sharePresence` | Privacy |
| `updateCheck` (0 = not asked, 1 = on, 2 = off), `updateConsentAsked`, `updateSkipVersion` | Updates |
| `[server_<id>]` groups: `name`, `dataSaver`, `uploadDirectory`, `uploadMaxMB`, `addRequiredNotice` | Servers (`<id>` is 12 hex digits derived from the server's unique ID) |
| `[accessGroups]` | the `tsmediachat` group id remembered per server (Server access) |
