# FAQ and troubleshooting

[← Back to README](../README.md)

Answers to common questions, grouped by topic. Each question has its own link, so you can share it.

- [Install and updates](#install-and-updates)
- [Sending](#sending)
- [Viewing and playback](#viewing-and-playback)
- [Reactions and presence](#reactions-and-presence)
- [Replies and emoji](#replies-and-emoji)
- [Voice messages](#voice-messages)
- [Permissions](#permissions)
- [Compatibility](#compatibility)
- [Storage](#storage)

## Install and updates

### TeamSpeak shows "Bad Image" with error status `0xc0000020`

Windows considers the plugin DLL damaged. Usually an antivirus product quarantined or modified it, or the download was incomplete. Close TeamSpeak, download the package again from the [releases page](https://github.com/Metihttp/Teamspeak_Media_chat/releases/latest) and reinstall it. If it happens again, add an exclusion for `%APPDATA%\TS3Client\plugins` in your antivirus. The DLL is not code-signed, so some antivirus heuristics may flag it.

### The plugin doesn't show up in TeamSpeak

Check that you run TeamSpeak 3.6.x and that the plugin is enabled in **Tools → Options → Addons**. The TeamSpeak log (`%APPDATA%\TS3Client\logs`) should contain a line like `TS Media chat 2.2.0 loaded`.

### How do I update?

From 2.2.0 on, the plugin can update itself. About 45 seconds after its first start it asks *Keep TS Media chat up to date?*; click **Turn on**. It then checks GitHub about once a day and shows *Update to X?* with what's new. **Update** downloads and checks the new version, and **Restart TeamSpeak now** finishes it. You can also check at any time with **Plugins → TS Media chat → Check for updates…** or `/tsmedia update`.

Coming from 2.1 or older, install 2.2.0 by hand once: download the new `.ts3_plugin`, close TeamSpeak completely (the old DLL is locked while TeamSpeak runs), double-click the file and start TeamSpeak again. Your settings and the cache are kept. The same works for any later version.

### Is the update check safe? What does it send?

It asks GitHub for one small file, and nothing is installed unless it carries the author's signature and you click **Update**. GitHub sees your IP address; no names, servers, chats or files are sent. It is off until you agree, and you can turn it off in **Settings → Privacy & updates → Updates**. Copies you build yourself have no update check. Details: [updates](UPDATES.md).

### An update went wrong

Every error says what happened and whether anything was changed. If the new version crashes while starting, its second start puts the previous version back by itself and tells you in the chat. If TeamSpeak doesn't start at all, start it once with `-safemode` or install any release by hand. See [if something goes wrong](UPDATES.md#if-something-goes-wrong).

### Does it work with 32-bit TeamSpeak?

The package includes a 32-bit DLL, but it hasn't been tested on a real 32-bit client yet. If you try it, please report the result in the [issues](https://github.com/Metihttp/Teamspeak_Media_chat/issues).

## Sending

### Dropping files opens a window instead of sending them

Since 2.2, a drop opens the send window, where you can add a caption, mark spoilers and send an album. Hold <kbd>Ctrl</kbd> while dropping to send right away, or set **Settings → Sending → When you drop files on the chat** to *Send right away*. Hold <kbd>Shift</kbd> for TeamSpeak's own drop.

### I don't want Ctrl+V or dropping files to send anything

Both can be turned off in **Settings → Sending**. For a single drop, hold <kbd>Shift</kbd> while dropping.

### My video was compressed. How do I send the original?

Videos over 25 MB, and iPhone (HEVC), AV1, VP9 and camcorder videos, are converted to an MP4 (720p by default) before the upload, so they upload faster and play everywhere. To send one as it is, pick *Original* in its **Quality** list in the send window, or click **Send original** in the upload panel while it compresses. To change the size, the quality or turn it off: **Settings → Sending → Videos**.

### "This file is larger than your 100 MB upload limit"

Raise *Upload size limit* in **Settings → Sending** (up to 4096 MB), or for one server in **Settings → Servers**. The server's transfer quotas still apply. For videos, the send window picks a smaller quality that fits when it can.

### The send window says only some people will see it in the chat

The line counts the people in your channel whose TS Media chat answered. People on TS Media 2.1 or older can't answer yet, so they count as getting a link even though they see the media; the count grows as friends update. People who turned off *Tell people in your channel that you have TS Media* aren't counted either.

### Some people see my spoiler unblurred

People on TS Media 2.1 or older, and people without the plugin, can't blur it; the send window says so under *Mark as spoiler*. People with 2.2 or later who turned on *Show spoilers without blurring* see spoilers right away by their own choice.

### My caption appeared as its own message

A caption that doesn't fit in the same chat message as the file (long file names, very long captions) is sent as its own message right above the file. It is never shortened.

### Files end up in the channel root instead of `/tsmedia`

Your server group doesn't have `i_ft_directory_create_power`, so the plugin can't create the folder. Ask your server admin, or see the [server admin guide](SERVER-ADMIN.md#permissions).

### I can't send files in a password-protected channel

Sending files and voice messages to password-protected channels isn't supported. Use another channel.

### Nothing happens when I send in a private chat

If the plugin can't tell who the private chat is with (for example, they left the server), it sends nothing and says so in the chat. This makes sure something meant for one person never ends up in the channel.

### The upload panel says "TeamSpeak is limiting messages. Retrying…"

TeamSpeak's flood protection asked the plugin to slow down, usually after many files or messages in a short time. The plugin waits and sends the message again by itself; nothing is lost.

## Viewing and playback

### A video doesn't play

Inside the chat, a video that Windows can't play shows *Opens in default app* and opens in your default video app when you click it. The gallery viewer names the missing decoder and offers **Get it from Microsoft Store** when there is an extension for it. H.264 (most .mp4 files) works out of the box. For the others, install the matching extension from the Microsoft Store:

| Format | Extension to install |
| --- | --- |
| HEVC (H.265) | HEVC Video Extensions |
| VP9 (many .webm files) | VP9 Video Extensions |
| AV1 | AV1 Video Extension |
| MPEG-2 | MPEG-2 Video Extension |

Windows N editions (for example Windows 11 Pro N) need the **Media Feature Pack**. Formats Windows can't decode at all (ProRes, DNxHD, …) can still be opened with right-click → *Open in default app*. Since 2.2, senders convert hard-to-play videos to H.264 before uploading, so this gets rarer as people update.

### An audio file doesn't play

Ogg and Opus files need **Web Media Extensions** from the Microsoft Store; the card or the viewer says so. A file Windows can't play shows *Can't play here · Click to open in your default app*.

### Pictures don't load by themselves any more

Data saver is probably on, for all servers (**Settings → Receiving & playback**) or for this server (**Settings → Servers**, or **Plugins → TS Media chat → Resume automatic downloads on this server**). Held items say *Data saver — click to load*. Spoilers also wait until you reveal them.

### GIFs only play when I point at them

Either *Play GIFs automatically* is off in **Settings → Receiving & playback**, or Windows animations are turned off (**Settings → Accessibility → Visual effects → Animation effects**). The plugin follows that Windows setting: GIFs then play only while the pointer is over them, and loading indicators stand still.

### Pictures look smaller at 100% in the viewer

Since 2.1.0, 100% in the gallery viewer shows one image pixel per screen pixel. On a display scaled to 125%, 150% or more, that is smaller than in apps that scale pictures up. *Fit* still fits the picture into the window, but never enlarges it.

### Other people don't see my images in the chat

They need the plugin too, and download permission in that channel. Without the plugin they see the link and the note, and can click the link to download the file.

### A preview says "Not connected to this server"

The file is on a server you are not connected to, for example after a disconnect. Reconnect and click the preview. Downloads interrupted by a lost connection resume by themselves once you are back.

### "File doesn't match what was sent"

The downloaded file is different from the one the sender posted: it was most likely replaced or damaged on the server. TS Media chat deletes it and never shows or saves it, and there is no way to open it anyway. Ask the sender to send it again. Right-click → **Copy details** copies both checksums and the file's path if you want to report it.

### What do the messages on a preview mean?

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="images/errors-dark.png">
  <img src="images/errors-light.png" width="100%" alt="File cards in the chat showing error messages such as no permission to download, file no longer on the server and server transfer limit reached">
</picture>

When a download fails or waits, the preview or card says why:

| Message | What it means | What to do |
| --- | --- | --- |
| No permission to download | Your server group may not download files in this channel. | Ask a server admin ([permissions](#no-permission-to-download)). |
| File is no longer on the server | The file was deleted from the server, or replaced after it was sent. | Ask the sender to send it again. |
| File doesn't match what was sent | The file on the server is not the one that was sent. | Ask the sender to send it again. |
| Checking file… | The download is being compared with its checksum. | Wait a moment. |
| Channel is password protected | The file is stored in a password-protected channel. | Use TeamSpeak's file browser. |
| Not connected to this server | You are not connected to the server the file is on. | Reconnect; the download resumes. |
| Server transfer limit reached | The server's file transfer quota is used up. | Ask a server admin. |
| Data saver — click to load | Data saver holds automatic downloads. | Click it, or turn data saver off. |
| Download failed | Something else went wrong. | Click the preview to try again. |
| Can't preview this image | The image is too large to decode (above 80 megapixels) or can't be read. | Click it, then choose **Open with default app**. |

Deleted files, files that don't match and files in password-protected channels can't be retried: their previews do nothing when clicked. For everything else, click the picture or the card's **Retry** button to try again. Point at a card to read the whole message.

## Reactions and presence

### Others don't see my reactions

Reactions reach people with TS Media chat 2.2 or later who are online in the same channel or private chat. People on 2.1 or older and people without the plugin don't see them. Someone who comes in later sees the reactions of the people who are still there. Check that *Show reactions on media* is on in **Settings → Privacy & updates**, and that you are connected. If a reaction couldn't be sent, it is taken back and its tooltip says why (for example *This server doesn't allow plugin messages*).

### Why can't I react in the server chat?

Reactions are only for channel and private chats. In the server chat there are no reaction rows and no *Add reaction*.

### Can I hide that I use TS Media chat?

Yes: turn off **Settings → Privacy & updates → Tell people in your channel that you have TS Media**. The plugin then says goodbye to the people it told, and your send window no longer counts who will see your media. You still see media and can send it as before.

## Replies and emoji

### There are two smiley buttons in the chat input

One is TeamSpeak's own emoticon button, the other is the TS Media chat emoji picker (HD emoji, search, recently used). To keep only TeamSpeak's, turn off **Settings → General → Emoji → Show the emoji button in the chat input**; <kbd>Ctrl</kbd>+<kbd>E</kbd> in the chat input still opens the picker.

### A smiley like `:)` shows as text in a reply line

In the message itself, TeamSpeak's smileys (`:)`, `;)`, `:D` …) show as HD emoji. In the reply line above a reply, the reply bar, the send window's *Replying to* line and *View replies*, they stay text for now. Real emoji show in HD there too.

## Voice messages

### What do the messages in the Voice message window mean?

| Message | Cause | What to do |
| --- | --- | --- |
| Microphone access is blocked | Windows' privacy setting keeps desktop apps (TeamSpeak too) away from the microphone. | **Open Windows settings**, turn on *Let desktop apps access your microphone*, then try again. |
| No microphone found | Windows has no microphone that is on. | Connect one, or pick one in **Open sound settings**. |
| The microphone is busy | Another app uses the microphone in exclusive mode. | Close that app (or turn off *Allow applications to take exclusive control* for the device), then **Try again**. |
| The microphone was disconnected | It was unplugged or turned off. With 1 second or more recorded, you get the review instead and keep what you said. | Reconnect it, then **Try again**. |
| This microphone can't be used | Its audio format isn't 44.1 or 48 kHz, which Windows' encoder needs. | Pick another microphone in the TS Media settings, or change the device's format in Windows sound settings. |
| Voice messages aren't available | Windows Media Foundation is missing (Windows N editions). | Install the Media Feature Pack. |
| Couldn't save the recording | The file couldn't be written (disk full?). Your recording is kept while the window is open. | Free some space, then **Try again**. |
| No sound from the microphone | Nothing louder than -50 dB arrived for 3 seconds: the microphone may be muted in Windows or on the headset. | Unmute it; the recording goes on meanwhile. |
| Recording from Windows' default communications microphone | The plugin couldn't tell which microphone TeamSpeak uses. | If that's the wrong one, pick yours under **Settings → General → Voice messages → Microphone**. |

### Others saw my microphone as muted

That's on purpose: while you record, your TeamSpeak microphone is muted so voice activation doesn't send you live to the channel. It is unmuted as soon as the recording stops. If TeamSpeak lost the connection meanwhile, it may stay muted for that server: unmute it with TeamSpeak's mute button. Turn this off with *Mute my TeamSpeak microphone while recording* in the settings.

### Does the plugin listen all the time?

No. The microphone is only opened while the *Voice message* window shows *Recording*, and closing the window stops it. Windows' microphone icon in the taskbar shows TeamSpeak as using it during that time (the plugin runs inside TeamSpeak).

### How long can a voice message be?

Up to 5 minutes; the time turns red at 4:30. A minute takes about 0.73 MB on the server.

## Permissions

### "No permission to download"

Your server group lacks the file transfer permissions (`i_ft_file_download_power`). Show the [server admin guide](SERVER-ADMIN.md) to your server admin.

### "You don't have permission to upload files in this channel"

Your server group lacks `i_ft_file_upload_power`. On a default TeamSpeak server the Guest group cannot upload, so new users hit this first. A server admin can create the `tsmediachat` group with one click and give it to you ([one-click group](SERVER-ADMIN.md#one-click-ts-media-chat-group)), or grant the permission by hand ([granting permissions](SERVER-ADMIN.md#granting-permissions-in-the-teamspeak-client)).

### How do I give someone access as a server admin?

Create the group once in **Settings → Servers → Server access → Create TS Media chat group**, then right-click the person and choose **TS Media chat → Give TS Media chat access** (or **Server Groups → tsmediachat**). See the [server admin guide](SERVER-ADMIN.md#one-click-ts-media-chat-group).

## Compatibility

### What do friends on TS Media 2.1 or older see?

Your files, captions and albums still reach them: they see the caption, every picture of an album as its own preview, spoilers unblurred and voice messages as an ordinary audio file card. A reply reaches them as an italic quote line (*↪ Alice · 21∶14: “…”*) followed by the reply, and emoji look the way TeamSpeak draws them. They don't see or send reactions and don't count as having TS Media in your send window. Once they install 2.2.0 by hand, later versions reach them through the update check.

### Does it work with TeamSpeak 5 or TeamSpeak 6?

No. They don't load TeamSpeak 3 plugins. The plugin is made for TeamSpeak 3.6.x.

### macOS or Linux?

No, the plugin is Windows only.

## Storage

### The cache takes too much space

Lower the *Size limit* or click **Clear cache** in **Settings → General → Media cache**. Media still in the chat is downloaded again when needed.

### Where are my settings, the cache and the log?

In `%APPDATA%\TS3Client\plugins\tsmedia`: `settings.ini`, the `cache` folder and `logs\tsmedia.log`. Type `/tsmedia cache` to open the cache folder. A portable TeamSpeak uses the `config` folder inside the TeamSpeak folder instead of `%APPDATA%\TS3Client`. What each file holds: [privacy & security](PRIVACY-SECURITY.md#what-stays-on-your-computer).
