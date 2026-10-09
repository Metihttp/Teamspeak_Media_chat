# Settings

[← Back to README](../README.md)

Every option of TS Media chat, with its default and its range.

<p align="center">
  <img src="images/settings.png" width="640" alt="The TS Media chat settings dialog with its Receiving, Playback, Media cache, Sending and Note for people without the plugin sections">
</p>

Open the settings with **Plugins → TS Media chat → Settings…**, by typing `/tsmedia settings` in the chat, or with the plugin's Settings button in **Tools → Options → Addons**.

- The options are on tabs: **General** (media cache, voice messages), **Sending** (sending, and the note for people without the plugin) and **Receiving & playback**. <kbd>Ctrl</kbd>+<kbd>Tab</kbd> and <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Tab</kbd> switch tabs, and the dialog opens on the tab you used last.
- **OK** saves and closes, **Apply** saves and keeps the dialog open, **Cancel** closes without saving.
- **Restore defaults** only fills in the default of every option; nothing is saved until you click **OK** or **Apply**.
- Only the options you changed are saved. The volume you set in the gallery viewer is kept, and the dialog follows it.
- Options that only matter when another one is on are indented under it and greyed out while it is off.

Settings are stored in `%APPDATA%\TS3Client\plugins\tsmedia\settings.ini` (a portable TeamSpeak uses the `config` folder inside the TeamSpeak folder instead of `%APPDATA%\TS3Client`). A value outside its range is corrected to the nearest allowed value when the plugin starts.

## Receiving

| Setting | Default | Range and notes |
| --- | --- | --- |
| Show images, videos and file cards in the chat | on | The other Receiving options only apply while this is on. |
| Download images, GIFs and voice messages automatically up to | on, 15 MB | 1–500 MB. Larger images and GIFs load when clicked. Voice messages download up to this size, but never above 16 MB; larger ones load when played. |
| Play GIFs automatically | on | When off, a GIF plays while the pointer is over it. |
| Download videos automatically | Off (download when played) | 0–4096 MB, in steps of 10 MB. Videos up to this size download automatically. |
| Maximum preview size | 400 × 300 px | Width 120–1200 px, height 80–1200 px. |

- With *Show images, videos and file cards in the chat* off, you only see the links. The "plugin required" note stays hidden either way.
- With *Download videos automatically* at *Off (download when played)*, a video is only downloaded when you press play.
- With Windows animations turned off (**Settings → Accessibility → Visual effects → Animation effects**), GIFs play only while the pointer is over them, whatever *Play GIFs automatically* says. The dialog shows a note under the option then.

## Playback

| Setting | Default | Range and notes |
| --- | --- | --- |
| Default volume | 80% | 0–100%. For inline videos and the viewer. |
| Start videos muted | off | |
| Loop videos | off | |

Changing the volume in the gallery viewer updates this setting too.

## Media cache

| Setting | Default | Range and notes |
| --- | --- | --- |
| Size limit | 1024 MB | 100 MB to 1 TB, in steps of 256 MB. |

- Beyond the limit, the media you haven't opened for the longest time is removed first. Media that is playing or open in the viewer never is. A lower limit applies as soon as you click **OK** or **Apply**.
- The space in use is shown under the limit, in the same unit (*700 MB of 1024 MB used (68%)*), with **Open folder** and **Clear cache** buttons.
- **Clear cache** asks first and says how much it will delete. Files on the server aren't affected: media still in the chat downloads again when you view it. Media that is playing or open in the viewer is kept.
- The cache lives in `%APPDATA%\TS3Client\plugins\tsmedia\cache`.

## Voice messages

On the **General** tab. See [Voice messages](USAGE.md#voice-messages) for how recording works.

| Setting | Default | Range and notes |
| --- | --- | --- |
| Microphone | Same as TeamSpeak (recommended) | Or one of Windows' microphones by name. When the plugin can't tell which microphone TeamSpeak uses, it records from Windows' default communications microphone and the recording window says so. A microphone picked here that is unplugged shows as *not connected*; until it is back, the TeamSpeak one is used. |
| Mute my TeamSpeak microphone while recording | on | So people in your channel don't hear you live (voice activation). Others see your microphone as muted until you finish. It is always unmuted again when recording stops, unless you unmuted it yourself meanwhile. |
| Let me listen before sending (hotkey) | on | Off: the hotkey's second press sends right away instead of stopping for a listen. |
| Play a sound when recording starts and stops | on | A short beep through TeamSpeak's playback device; only you hear it. |

The line under the options shows the record hotkey (*Record hotkey: F9*, or *not set*); **Set up hotkeys…** opens TeamSpeak's hotkey setup, where it is listed under TS Media chat as *Record a voice message*.

## Sending

| Setting | Default | Range and notes |
| --- | --- | --- |
| Send files dropped on the chat (hold Shift for TeamSpeak's own drop) | on | |
| Send screenshots and files pasted into the chat input (Ctrl+V) | on | Always asks before sending. |
| Convert pasted images over 2 MB to JPEG | on | Images with transparency stay PNG. |
| Upload a small preview with large images and videos | on | Others see a sharp preview while the full file downloads. |
| Upload size limit | 100 MB | 1–4096 MB. |
| Upload folder | `/tsmedia` | A folder in the channel's file browser, created when needed. Empty means `/tsmedia`; type `/` for the top level of the channel's file browser. |

- **Convert pasted images over 2 MB to JPEG** uses JPEG quality 90.
- **Upload a small preview**: the [usage guide](USAGE.md#file-names-and-folders) lists which files get a preview and where it is stored.
- **Upload size limit** is your own limit. The server's transfer quotas still apply. The paste dialog marks files over the limit before you click **Send**.
- **Upload folder** is per user: each person sends into the folder set in their own settings. Without permission to create folders, files go to the channel root.

## Note for people without the plugin

| Setting | Default | Range and notes |
| --- | --- | --- |
| Add a note after files you send | on | People without the plugin see *TS Media chat plugin required to view this in chat* after the link. |
| Link in the note | the project's GitHub page | Where "TS Media chat" in the note links to. |

- **Link in the note** must be an `http://` or `https://` address; `https://` is added when it is missing (you see it once you leave the field). Empty means the official page, `https://github.com/Metihttp/Teamspeak_Media_chat`. The link is checked when you leave the field: a problem (not a web address, another scheme, `[` or `]`, more than 512 characters) is explained right under it. The field can only be edited while the note is on.
