# Using TS Media chat

[← Back to README](../README.md)

How to send files, what you see in the chat, how the gallery viewer works, and the chat commands.

## Sending files

| Method | How |
| --- | --- |
| Drag & drop | Drop files on the chat messages or the input line. They are sent right away. |
| Paste | Press <kbd>Ctrl</kbd>+<kbd>V</kbd> in the chat input line. Always asks first. |
| Menu | **Plugins → TS Media chat → Send file / image to chat…** |
| Hotkey | Set it up once in **Tools → Options → Hotkeys** (see below). |
| Chat command | `/tsmedia send` |

- **Drag & drop:** you can drop one or more files at once. Hold <kbd>Shift</kbd> while dropping to get TeamSpeak's normal behaviour. Drags from TeamSpeak's own file browser keep working as before.
- **Paste:** take a screenshot (for example with <kbd>Win</kbd>+<kbd>Shift</kbd>+<kbd>S</kbd>) or copy files in Explorer, click the chat input line and press <kbd>Ctrl</kbd>+<kbd>V</kbd>. Plain text still pastes as usual.
- **Hotkey:** in **Tools → Options → Hotkeys**, click **Add**, then **Show Advanced Actions**, and pick **Plugins → Plugin Hotkey → TS Media chat → Send file / image to the current chat**.

Drag & drop and Ctrl+V sending can each be turned off in the [settings](SETTINGS.md#sending).

### Where a message goes

- The message goes to the chat tab you are looking at: channel, server or a private chat. The file itself is always stored in the file browser of **the channel you are currently in**.
- If the partner of a private chat can't be identified (for example, they left the server), nothing is sent. Something meant for one person never ends up in the channel instead.
- When you send several files at once, their messages appear in the chat in the order you chose the files. Up to two files upload at the same time; the others wait their turn.

### The paste confirmation

<p align="center">
  <img src="images/paste-dialog.png" width="480" alt="The Send to chat dialog after pressing Ctrl+V: a thumbnail of the pasted screenshot, the question whether to send it to the current channel, its size in pixels, and Send and Cancel buttons">
</p>

Pressing Ctrl+V with a screenshot or copied files opens a small dialog that shows what will be sent and where (the channel, the whole server or a private chat). For a screenshot it shows a thumbnail and the size in pixels; for files, their names and the total size. Nothing is sent until you click **Send**. The clipboard may hold something old you forgot about, so the plugin always asks.

### Upload progress

<p align="center">
  <img src="images/upload-toast.png" width="360" alt="The upload panel in the corner of the chat: a file name, a progress bar with the percentage and upload speed, and a button to cancel the upload">
</p>

A small panel in the corner of the chat shows each upload with its progress and speed. Its **×** button cancels a running upload. If an upload fails, the panel and the chat say why and what to do next.

### File names and folders

- Files are uploaded as `/tsmedia/<name>_<8 random hex digits>.<ext>`, for example `/tsmedia/holiday_3f9a1c2e.jpg`. Spaces become `_`, unusual characters are replaced, the name (without the extension) is cut to 48 characters and the extension is lower-cased. The random part keeps names apart, and an existing file is never overwritten.
- Where the plugin shows a file name, it leaves the random part out (`holiday.jpg`). A pasted screenshot is called *Pasted image*.
- A pasted image is sent as `new_photo_<random>.png`, or converted to JPEG (quality 90) if it is larger than 2 MB and has no transparency.
- With *Upload a small preview* on (the default), a preview is stored as `/tsmedia/previews/<name>.jpg`. Previews are only made for photos larger than 1.5 MB or with a side longer than 2560 px (preview up to 1280 px), for animated GIFs and WebPs larger than 4 MB (first frame, up to 640 px), and for every video (poster up to 960 px).
- If the folder can't be created (no `i_ft_directory_create_power`), the file goes to the root of the channel's file browser and its preview to `/previews`, or next to the file as `<name>.preview.jpg`.
- The folder and the maximum upload size (default 100 MB) can be changed in the [settings](SETTINGS.md#sending).

## Viewing media

- **Images and GIFs** up to 15 MB load automatically. Larger ones show their preview (or a blurred placeholder) with the file size, and load when you click them. Clicking an image or GIF opens the gallery viewer.
- **Videos** show their poster, a play button and the duration. Clicking play downloads the video first (a progress ring shows the percentage) and then plays it right in the chat. Hover over a playing video to show its controls: play/pause, time, seek bar, mute and expand (opens the gallery viewer). The controls hide 2.5 s after the mouse stops. Starting a video pauses any other.
- **Other files**, audio files included, appear as cards. Click a card to download the file and open it with its default Windows app. Programs and scripts are never run; they are only shown in Explorer.
- **Ordinary TeamSpeak file links** (for example a file dragged from the file browser into the chat) get the same treatment, just without the dimensions, duration, blurred placeholder and preview that TS Media chat adds to its own links.
- You have to be connected to the server the file is on. Up to 3 downloads run at once, previews first.
- When something goes wrong, the reason is written on the preview or card. Click it to try again (not offered for deleted files or password-protected channels). The [FAQ](FAQ.md#what-do-the-messages-on-a-preview-mean) explains every message.

## Right-click menu

Right-click any preview or card:

| Item | Shown for |
| --- | --- |
| Play / Pause | videos |
| Mute / Unmute | videos that have been started |
| Open | always (media opens in the viewer, other files in their default app) |
| Open with default app | downloaded images and videos |
| Save as… | downloaded files |
| Copy image | downloaded images and GIFs |
| Copy link | always (copies the `ts3file://` link) |
| Show in folder | downloaded files |
| Download | files that are not downloaded yet |
| Retry download | files whose download failed (unless the file was deleted) |

## Gallery viewer

<p align="center">
  <img src="images/viewer.png" width="100%" alt="The TS Media chat gallery viewer showing a photo, with the action bar along the bottom and an item counter">
</p>

The viewer shows every media item of the chat, so you can step through them without closing it. The counter shows which item you are looking at.

- The mouse wheel zooms around the pointer, and dragging pans the image.
- Double-clicking an image switches between *fit to window* and *actual size*.
- Clicking a GIF pauses or resumes it. Clicking a video plays or pauses it, and double-clicking a video toggles full screen.
- The bottom bar has **Fit**, **100%**, **Copy image** (**Copy frame** for videos), **Save as…**, **Show in folder** and **Open with default app**.
- Volume changes made in the viewer are remembered.
- Images that are not downloaded yet load when you step to them. Videos wait until you press play, unless they are small enough to download automatically.

### Viewer keyboard shortcuts

| Key | Action |
| --- | --- |
| <kbd>←</kbd> / <kbd>→</kbd> | previous / next item of the gallery |
| <kbd>Shift</kbd>+<kbd>←</kbd> / <kbd>Shift</kbd>+<kbd>→</kbd> | seek 5 s back / forward (video) |
| <kbd>Space</kbd> or <kbd>K</kbd> | play / pause a video; pause / resume a GIF |
| <kbd>Home</kbd> | back to the start of the video |
| <kbd>M</kbd> | mute / unmute |
| <kbd>↑</kbd> / <kbd>↓</kbd> | volume up / down by 5% |
| <kbd>F</kbd> | full screen |
| <kbd>Esc</kbd> | leave full screen, otherwise close the viewer |
| <kbd>0</kbd> | fit the image to the window |
| <kbd>1</kbd> | actual size (100%) |
| <kbd>Ctrl</kbd>+<kbd>C</kbd> | copy the image, or the current video frame |
| <kbd>Ctrl</kbd>+<kbd>S</kbd> | save as… |

Letter and number keys also work with non-Latin keyboard layouts.

## Chat commands

| Command | What it does |
| --- | --- |
| `/tsmedia send` | pick files and send them to the current chat |
| `/tsmedia settings` | open the settings |
| `/tsmedia cache` | open the media cache folder |
| `/tsmedia debug` | write a list of TeamSpeak's chat widgets to `widget_dump.txt` in the plugin's data folder (`%APPDATA%\TS3Client\plugins\tsmedia`), for bug reports |
| `/tsmedia` | print the version and the list of commands |
