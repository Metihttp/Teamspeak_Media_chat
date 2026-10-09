# Settings

[← Back to README](../README.md)

Every option of TS Media chat, with its default and its range.

<p align="center">
  <img src="images/settings.png" width="640" alt="The TS Media chat settings dialog with its Receiving, Playback, Sending and General sections">
</p>

Open the settings with **Plugins → TS Media chat → Settings…**, by typing `/tsmedia settings` in the chat, or with the plugin's Settings button in **Tools → Options → Addons**. **Restore defaults** fills in the default of every option; click **OK** to keep them.

Settings are stored in `%APPDATA%\TS3Client\plugins\tsmedia\settings.ini` (a portable TeamSpeak uses the `config` folder inside the TeamSpeak folder instead of `%APPDATA%\TS3Client`). A value outside its range is corrected to the nearest allowed value when the plugin starts.

## Receiving

| Setting | Default | Range and notes |
| --- | --- | --- |
| Show images, videos and file cards inside the chat | on | The other Receiving options only apply while this is on. |
| Load images and GIFs automatically | on | |
| Load images automatically up to | 15 MB | 1–500 MB. Larger images and GIFs load when clicked. |
| Play GIFs automatically (otherwise while hovered) | on | When off, a GIF plays while the mouse is over it. |
| Download videos automatically up to | Never (when I press play) | 0–4096 MB. 0 shows as *Never*. |
| Preview max width | 400 px | 120–1200 px. |
| Preview max height | 300 px | 80–1200 px. |

- With *Show images, videos and file cards inside the chat* off, you only see the links. The "plugin required" note stays hidden either way.
- With *Download videos automatically up to* at *Never*, a video is only downloaded when you press play.

## Playback

| Setting | Default | Range and notes |
| --- | --- | --- |
| Volume | 80% | 0–100%. For inline videos and the viewer. |
| Start videos muted | off | |
| Loop videos | off | |

Volume changes you make in the gallery viewer are saved to this setting.

## Sending

| Setting | Default | Range and notes |
| --- | --- | --- |
| Drop files on the chat to send them (hold Shift to skip) | on | Hold Shift while dropping for TeamSpeak's normal behaviour. |
| Ctrl+V a screenshot or copied files in the chat line to send them | on | Always asks before sending. |
| Convert large pasted screenshots to JPEG | on | Pasted images larger than 2 MB without transparency. |
| Upload a small preview with large images and videos | on | Others see a sharp preview while the full file loads. |
| Tell people without the plugin that it is needed | on | Adds the grey note after the link. |
| Download link | the project's GitHub page | Where "TS Media chat" in the note links to. |
| Max upload size | 100 MB | 1–4096 MB. |
| Folder in channel files | `/tsmedia` | Empty means the root of the channel's file browser. |

- **Download link** must be an `http://` or `https://` address; `https://` is added when it is missing. Empty means `https://github.com/Metihttp/Teamspeak_Media_chat`. The field can only be edited while the note is on.
- **Convert large pasted screenshots to JPEG** uses JPEG quality 90. Screenshots with transparency always stay PNG.
- **Upload a small preview**: the [usage guide](USAGE.md#file-names-and-folders) lists which files get a preview and where it is stored.
- **Max upload size** is your own limit. The server's transfer quotas still apply.
- **Folder in channel files** is per user: each person sends into the folder set in their own settings. Without permission to create folders, files go to the channel root.

## General

| Setting | Default | Range and notes |
| --- | --- | --- |
| Cache size limit | 1024 MB | 100 MB to 1 TB. |

- Beyond the limit, the media used least recently is deleted first. Media that is playing or open in the viewer never is.
- The space in use is shown next to the limit, with **Open folder** and **Clear** buttons. Media still in the chat is downloaded again when needed.
- The cache lives in `%APPDATA%\TS3Client\plugins\tsmedia\cache`.
