# Changelog

All notable changes to **TS Media chat** are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Version 2.0.6 is the first public release. Earlier versions were internal builds without a public release; they are listed briefly at the end.

## [2.6.0] - 2026-11-14

### Added
- **Chat proxy**, for places where YouTube and other sites are filtered. Link previews (YouTube, Klipy and other GIF and media sites, cards for sites and pages, Nextcloud shares), the YouTube player and the update check and download can go through an HTTP or SOCKS5 proxy, such as your VPN app's local port, while TeamSpeak itself (the server connection, voice, chat, plugin commands and file transfers) never uses it. You can keep your VPN app's TUN or VPN mode off, stay connected to TeamSpeak servers in your country and still load previews through the VPN. Previews you send are shared through your TeamSpeak server as before, so friends without a VPN see them too. [How to use it with your VPN app](docs/FAQ.md#how-do-i-use-ts-media-with-my-vpn-app).
  - Its own window, *Chat proxy*: *Use a chat proxy for link previews, the YouTube player and update checks* (on and off), SOCKS5 or HTTP, server and port, an optional user name and password, and what to use while it is off (*Use Windows proxy settings*, the default, or *No proxy (direct)*). Everything is kept while it is off, and the fields can be tested while it is off.
  - **Find my VPN app** looks for v2rayN, Throne, Nekoray, Clash Verge Rev and Hiddify at their usual ports on this PC (nothing leaves the PC). **Paste from clipboard** fills the fields from a proxy link such as `socks5://127.0.0.1:10808`. **Test** sends one request to `www.youtube.com` through the proxy and says what is wrong (*Can't reach the proxy at …*, *The proxy refused the login…*, *The proxy can't reach the website…*).
  - Website names are looked up by the proxy, never by your PC, so a filtered DNS doesn't matter.
  - On and off at once, without retyping anything: **Plugins → TS Media chat → Turn on chat proxy** / **Turn off chat proxy**, a TeamSpeak hotkey (*Turn the chat proxy on or off*), `/tsmedia proxy on` / `off` (`/tsmedia proxy` says what is in use) and a button in **Settings → Privacy & updates → Chat proxy**. Each says in the chat what it did. Previews that are loading start again through the new route, and a YouTube video that is playing closes (*Closed: the chat proxy changed. Press Play again.*).
  - When the proxy fails, automatic previews pause (*Paused · the proxy isn't working*, with **Load preview**) instead of turning into error cards, and one line in the chat says why, with **Turn off chat proxy**; they load by themselves once the proxy works again. A preview that failed through a proxy says so (*Couldn't load through the chat proxy*, *Couldn't load through your Windows proxy*), and its menu offers **Turn off chat proxy** and **Proxy settings…**. Previews that waited for the proxy load when you click them after you turn it off.
- The diagnostic info has a *Chat proxy* section: the mode, where the proxy is (this PC, the local network or elsewhere), whether it works and the last Test. Never its address, port or login.

### Changed
- With *Use Windows proxy settings*, a Windows proxy setting or setup script that names only a SOCKS proxy no longer sends previews directly: they wait for a click. For a SOCKS proxy entered in the Windows proxy settings (not a setup script), the YouTube player doesn't play and the update check says it can't use that proxy instead of going directly, and the Chat proxy window offers **Use it as the chat proxy**. A setup script that doesn't answer in time no longer sends previews directly either.
- A link whose site your network's DNS filters (an answer in 10.10.34.0/24) says *Your network blocks this site* and offers the chat proxy (**Use chat proxy**, or **Set up proxy…** while none is saved).
- *Play YouTube videos in the chat* is now called *Play YouTube videos in a TS Media window*.
- The update consent window names the YouTube player as a web connection too.
- The GIFs bar's button says *Remove* instead of *Remove all* when there is only one GIF.

### Fixed
- Link previews with a fake-IP TUN VPN (Clash Verge Rev and other mihomo or sing-box TUN modes, whose DNS answers addresses in 198.18.0.0/15) no longer fail.
- A proxy that fails or is slow no longer pauses a website for 10 minutes.
- A chat at its end stays at its end when the plugin inserts a preview or a preview changes size. After **Tools → Options → Addons → Reload** at 100% scaling the chat now stays at its end; with higher Windows scaling it can still stop above its end, and a chat you scrolled up can move by a message or two.

### Security
- The proxy password is stored with Windows DPAPI for your Windows account and only for the proxy server it was saved with (another server needs it typed again). It is sent to a proxy on another computer only when that proxy asks for it, and never appears in logs or the diagnostic info.
- TS Media never falls back to a direct connection when the chat proxy fails, and a proxy setting it can't use never means "direct".
- TS Media's own local connection to your proxy listens on 127.0.0.1 only, with a random key per session, and passes only TS Media's own requests to public websites on port 443.
- The YouTube player never sends WebRTC traffic outside your proxy settings.
- `.alt` names are never loaded.

## [2.5.0] - 2026-11-12

### Added
- **Page links like Discord.** Links to GIF and media sites show the media itself: Tenor, Giphy, Imgur (also `.gifv` and albums), Klipy (clips, stickers, memes, emoji, AI GIFs; for its GIF pages, which are behind Klipy's own bot check, Klipy's public oEmbed is asked for the GIF: its answer doesn't name it yet, and they start working by themselves once Klipy fixes that), Reddit videos and post links, Streamable, Imgflip, Pinterest, 9GAG, Coub, Gyazo, Lightshot, ImgBB, Postimages, Gifer, Makeagif, Pixeldrain, Tumblr posts and Steam screenshots. GIF-like videos play silently in a loop, like GIFs.
- **Cards for well-known sites:** Vimeo, SoundCloud, Spotify, Apple Music, Dailymotion, Medal, X, Bluesky, Twitch, Kick, Steam, GitHub, Wikipedia, Telegram, Discord invites and Reddit posts, with the site's colour, author, title, a short text and a picture; a click opens the link in your browser.
- **Cards for other web pages** (*Show cards for other web pages*, on by default): site name, title, a short text, the picture and the page's colour, read from the start of the page (never shown or run).
- Made by the sender like every other preview: the sender's plugin reads the page once and shares the card through the TeamSpeak server, and people with TS Media 2.5 use that card instead of contacting the site. When the sender has no TS Media 2.5 (or turned previews off), their PC loads it when it scrolls into view, within the automatic limits. Older versions ignore the new previews.
- **Load link previews** in a message's menu loads (or retries) the previews of that message's links by hand.
- **Nextcloud and ownCloud share links** show the shared picture, GIF or video inline, and a card for other files and folders. From a link to a share page, password-protected, upload-only and download-limited shares, and shares whose owner turned downloads off, are never downloaded (a link straight to the file loads like any file link); servers on a LAN, VPN or private address are never contacted.
- Cards and media from pages that declare themselves for adults (their rating, or an age of 18 or more) show their picture covered like a spoiler: a click shows it, and *Show spoilers without blurring* shows it at once. A page without that flag may still be for adults.
- **Previews follow zoom and high-DPI screens.** Every preview (pictures, GIFs, videos, albums, file, audio and voice cards, link cards, polls, reactions) is drawn in proportion to the chat's text and sharp: it grows with Windows display scaling (150%, 200% on 4K screens), with Ctrl + mouse wheel over the chat and with TeamSpeak's chat font. The line you are reading stays in place while you zoom, and a chat at its end stays at its end. Reply lines and the reaction rows under messages follow the text too.
- **Preview size** (*Settings → Receiving & playback*): Small, Normal, Large or Larger, for previews larger or smaller than the text.
- **A GIFs tab in the emoji picker**, like Telegram: the GIFs you send (GIF site links, GIF links, GIF files you upload) are kept, newest first, and one click sends one again; *Save to GIFs* in a GIF's menu keeps any other. Only the GIF under the pointer moves; opening the tab contacts no website. *Remember GIFs I send* and *Remove all GIFs…* are in *Settings → General → Emoji*; *Clear cache* keeps your GIFs. There is no GIF search.
- **YouTube videos play in a window of their own over TeamSpeak** with YouTube's own player in its privacy-enhanced mode (`youtube-nocookie.com`, in Microsoft Edge WebView2, the web view built into Windows), after you press Play on a YouTube card; nothing of the player loads before that. The first time, the card asks once (*Play this video in a window?*) with links to YouTube's Terms of Service and Google's Privacy Policy. The window closes with its close box, **Esc** or the card's (x), and its size is remembered; the video pauses when TeamSpeak is minimized or a TS Media video or voice message starts, and only one plays at a time. Videos whose owner doesn't allow embedding show *This video can only be watched on YouTube* with **Watch on YouTube**. *Play YouTube videos in the chat* (*Settings → Privacy & updates*, on by default) turns it off: Play then opens the browser as before. 64-bit TeamSpeak only (in 32-bit TeamSpeak, Play opens the browser); without the WebView2 Runtime, videos open in the browser.

### Changed
- Links written as `http://` (TeamSpeak links addresses typed without `https://` that way) get previews too, always loaded over https.
- Website requests say who they are (`TSMediaChat/2.5` with the project's address) and ask for no compression.
- Card and title text keeps Persian and emoji joiners and loses more invisible characters.
- Links with Persian (or other) joiners in their address, and links that end in a bracket such as Wikipedia's `Foo_(bar)`, are read the same way by the sender's plugin and by everyone else.
- In 32-bit TeamSpeak, pictures larger than about 16.7 megapixels get no preview (they could exhaust its memory).
- YouTube cards use the large thumbnail (1280 × 720) when the video has one, so they stay sharp on high-DPI screens; sender-made YouTube previews are up to 960 px wide.
- GIFs are decoded at most at twice their box and never above their own size, which saves work while many play at once on a high-DPI screen.
- A long chat keeps the memory of its preview pictures bounded (128 MB, 48 MB in 32-bit TeamSpeak): pictures far from the view are drawn again when they come near.
- The diagnostic info shows the chat's preview scale.

### Fixed
- A site that answers "too many requests" (HTTP 429) no longer turns previews into error cards: requests to one site are spaced out (also when the site says its allowance is used up), and its cards wait as long as the site asks, then load by themselves.
- Links a sender's plugin skipped because of its per-minute limit no longer keep people waiting for minutes.
- Cards above the automatic download limit load by themselves once the limit is raised.

### Security
- Pictures from other people and from websites are decoded only as PNG, JPEG, GIF, WebP or BMP, by their content (no SVG or other formats), and lossless WebP pictures are refused (a bug in the image library TeamSpeak ships). A received file that isn't a picture by its content (such as an SVG named `.jpg`) shows as a file card.
- Through a proxy, names under your own network's DNS suffixes are never loaded.
- Links and picture addresses that look like sign-in, confirmation, unsubscribe, download or token links are never loaded for a preview.

## [2.4.0] - 2026-11-09

2.4.0 includes everything prepared for 2.3.4, which was never released on its own.

### Added

- **Link previews** ([issue #1](https://github.com/Metihttp/Teamspeak_Media_chat/issues/1)). Web links in the chat get a preview under them, in every chat layout and in channel, private and server chats. [How link previews work](docs/USAGE.md#link-previews).
  - **YouTube** links (`youtube.com/watch`, `youtu.be`, `shorts`, also typed without `https://`) show a card with the channel, the title and the thumbnail (cut to 16:9, without YouTube's black bars); a click opens the video in your default browser.
  - **Links to pictures, GIFs and videos** (`.png .jpg .jpeg .gif .webp .mp4 .webm .mov .m4v`) show the file like one sent in the chat: the gallery viewer, animated GIFs, inline video with its controls. A video too large to play shows its poster with **Open in browser**.
  - **Made by the sender:** when you send such a link in a channel or private chat, your plugin loads it once and shares the preview through the channel's files with one plugin command. People with TS Media see it from the TeamSpeak server and never contact the website; it is only accepted from the sender of that message, and checked against its SHA-256. Previews are remembered in `link-previews.json`, so they come back when the chat history reloads.
  - **Your own links** show their preview at once from your own download (*Making the preview…* until then), before it is uploaded.
  - **Waiting for the sender:** when the sender makes previews, your PC waits for theirs (*Waiting for the sender's preview…*, also for their earlier links' previews, which their plugin makes first) instead of contacting the website; if their plugin can't make one it says so with a small plugin command, and your PC loads the link itself. [The exceptions](docs/PRIVACY-SECURITY.md#link-previews).
  - **Other links load by themselves** (*Load link previews automatically*, on by default): links from people without the plugin, links in the server chat and links whose sender's preview doesn't come load their preview from the website when they show on your screen, at most 2 at a time (one per person) and a few per person in a row: YouTube always, pictures, GIFs and videos within your automatic download limit for images (videos use your video limit if you set one), only YouTube with data saver. A larger file shows **Load preview** with its size (a click loads pictures and GIFs up to 25 MB, videos up to 50 MB). Previously loaded links show from the cache without any request. Turn the switch off to load them only on a click; the card's tool tip says that loading connects your PC to that website.
  - **Hide preview** per card (× or right-click), **Open in browser** and **Copy link** in the right-click menu. Text reactions and replies on such messages work as before.
  - Pictures, GIFs and videos only over https, at most 3 links per message; web pages, other http links and the plugin's own links never get a preview. Website requests use no cookies, no Referer and a generic User-Agent, refuse addresses of your own network, follow at most 5 checked redirects and stop at the size limits.
  - **Your Windows proxy** is used when one is set (a proxy server, a setup script or automatic detection), so previews also work behind a proxy-based VPN. [What that means for privacy](docs/PRIVACY-SECURITY.md#link-previews).
  - A preview that can't be loaded says *Couldn't load the preview* with **Retry**, whatever the reason; the reason is in the plugin's log, which names websites only by their host.
  - **Clear cache** also clears the link previews (`link-previews.json` and the files loaded from websites, which stay within about 200 MB).
- **Important updates install by themselves.** With automatic checks on, a release the author marks as important (a fix that shouldn't wait) is downloaded and installed without a click and without a window, and starts with your next TeamSpeak start; TeamSpeak is never restarted for it. The chat says so, with a *What's new* link; to start it right away, use **Check for updates…** in the plugin menu. [How important updates work](docs/UPDATES.md#important-updates).
  - It needs the author's signature: the release must carry a marker (`TSMedia-X-important.json`) signed with the release key for exactly that release, and every downloaded file must match the signed SHA-256. A marker counts for at most 60 days.
  - *Skip this version* doesn't hold it back, but a version that rolled back on your PC, or that you replaced by hand with an older one, is never installed by itself again. After 2 attempts that didn't work out, a version is only offered the normal way.
  - Manual checks (**Check now**, **Check for updates…**, `/tsmedia update`) never install anything without a click, and versions before 2.4.0 always ask. Turning automatic checks off turns this off too.
  - The daily check makes one more small request for the marker, only when it finds a newer version.
- **"Someone in this channel has a newer version."** When someone in your channel has a newer TS Media chat than you, your own chat shows one line with a **Check for updates** link. Only you see it, it names nobody, and nothing is sent until you click; the click checks GitHub once. Anyone can claim any version, so a version GitHub doesn't have is answered with *isn't released yet*, and at most 2 such lines appear per TeamSpeak session.

### Changed

- **Settings → Privacy & updates → Link previews**: *Show previews for links to other websites*, *Load link previews automatically* and *Make previews for links I send*, all on by default, each saying who contacts the website. The README and [privacy & security](docs/PRIVACY-SECURITY.md#link-previews) describe the new web requests.
- The consent window and **Settings → Privacy & updates** say that important fixes signed by the author install by themselves. With automatic checks already on, the first start of 2.4.0 says it once in the chat.
- The consent window no longer calls the update check the plugin's only internet connection (link previews are one too), and names **Settings → Privacy & updates** as the place to change it.
- After an important update, the next start says *Updated to TS Media chat X (an important update, installed automatically).*
- `/tsmedia update` while a check or an install is running now says *TS Media chat is already checking for or installing an update.* instead of nothing.
- Releases: `scripts\build.ps1 -Important` writes and checks the signed marker, `-VerifyPublished` checks it on GitHub, and the reused `tsmedia-update.json` of 2.2.1 is pinned by its SHA-256.

### Fixed

- The update window opened while an update was downloading or installing showed an empty offer; it now shows the progress.

## [2.3.3] - 2026-11-01

### Fixed

- **TeamSpeak disconnected you from TeaSpeak servers when you sent a file with 2.3.2.** To make sure the server had the whole file, 2.3.2 asked the server for the file's size. TeaSpeak answers that request in a way TeamSpeak can't read, and TeamSpeak drops the connection ("parameter not found"). The same could happen while receiving a file that was still uploading. TS Media no longer asks TeaSpeak servers (or any server that isn't TeamSpeak's own) for a file's size: it only reads the end of the file back from the server, which already told whether the upload had arrived. Tested on a real TeaSpeak server: the message waited 4 to 8 seconds for the last bytes of a photo and a video, without a disconnect.

### Changed

- The TeamSpeak log notes for each server whether file sizes are asked (`Server: TeaSpeak … file sizes aren't asked`).

## [2.3.2] - 2026-10-31

### Fixed

- **"File doesn't match what was sent" for files that were fine.** TeamSpeak reports an upload as finished as soon as it has handed the last bytes to the network, so the message could reach the channel while part of the file was still on its way to the server. Whoever opened it right away downloaded an unfinished file. On TeaSpeak servers, which give an upload its full size from the start and fill in the bytes as they arrive, that unfinished file failed the checksum and was blocked for good.
  - **Sending:** your message now waits until the server has the whole file. Before it posts the message, TS Media also reads the end of the file back from the server (about 64 KB) and compares it with what you sent; this matters on servers that preallocate uploads, such as TeaSpeak.
  - **Receiving:** a download that came out short, or that ends in zeros because the sender's upload is still arriving, now waits (*Waiting for the sender's upload…*) and downloads the file again once it is all there. This also covers messages from senders with older versions.
  - A file is only reported as different when two complete downloads agree on it. A click on *File doesn't match what was sent* tries once more.
  - If the sender's upload never completes, the preview says *Download failed* (the file never arrived completely) with **Retry**.

### Changed

- The diagnostic info and the TeamSpeak log name the server software of each connection (for example `Server: TeaSpeak … on Linux`) and count uploads that were waited for and downloads that waited for an upload.

## [2.3.1] - 2026-10-12

### Added

- **Your messages on the right**, like Telegram (Cozy with bubbles, on by default): your messages sit on the right without your name or avatar, each bubble as wide as what it holds, with its time on it (after the last line, or under a picture or card). Right-to-left text such as Persian is right-aligned in its bubble, and so are its reactions. Pictures, albums, voice and file cards, polls, reactions and replies come along; the buttons that appear when you point at your message sit on its left. Turn it off in **Settings → General → Chat layout → Show my messages on the right**.

### Fixed

- The newest message no longer touches the chat tabs under the chat: the chat keeps some room under its last message (Cozy and Compact), and stays at its end when new messages arrive.

## [2.3.0] - 2026-10-11

### Added

- **Paperclip in the chat input**: a button at the left end of the chat input opens a menu with **Photo or video**, **Document**, **Poll** and **Live music**. Photos, videos and documents open the file chooser and then the send window; while the send window is open, they are added to it.
- **Anonymous polls**, like Telegram's: a question with 2 to 10 answers, one or several per person. Vote right on the poll card and see percentages, counts and the total; nobody, not even the poll's author, can see who voted for what. People without the plugin read the poll as text. Paperclip → **Poll**, or `/tsmedia poll` with the keyboard. [How polls work](docs/POLLS.md).
- **Live music**: play a music file from your computer to everyone in your channel through your own voice, and keep talking over it (MP3, M4A / AAC, FLAC, WAV, WMA; Ogg and Opus with Windows' *Web Media Extensions*).
  - A slim music bar above the chat input, no extra window: the title, play / pause, the time, a slider to move forward or back, the volume (remembered) and **Stop**, which ends the music and removes the bar.
  - On another server tab the bar stays, compact, and says where the music plays. TeamSpeak only sends from the server tab in front, so the music pauses by itself while you're on another server tab and plays on when you go back to its tab, unless you paused it yourself. **Play** in the compact bar takes you back there; **Stop** works from any tab.
  - Paperclip → **Live music**, or `/tsmedia music` with the keyboard; `/tsmedia music pause`, `/tsmedia music play` and `/tsmedia music stop` control the music that plays.
  - The bar says when the channel can't hear the music (microphone or sound muted in TeamSpeak, no capture device, no talk power). Music sounds best in channels set to the Opus Music codec. With push-to-talk the channel hears only the music while it plays.
- **Reactions on text messages**: links, replies, polls and any other message can get reactions too, from **Add reaction** when you point at a message or right-click → **Add reaction…**. People with TS Media 2.3 see them, and they come back after a restart. (Messages need TeamSpeak's timestamps.)
- **Message bubbles** in the Cozy layout, like Telegram (**Settings → General → Chat layout**, on by default): every message on its own rounded bubble, yours tinted, with its pictures, cards, polls and reactions inside. The buttons that appear when you point at a message (**Add reaction**, **Reply**, **Copy text**, **More**) keep off its text, also near the bottom of the chat.

### Changed

- **A redesigned send window**: a large preview for one picture or video, a grid of tiles for several (edit, spoiler and remove on each), a **Quality** chip for videos, *Send as an album* as a switch, a rounded caption field, and a title bar that follows TeamSpeak's dark skin. **Who?** is now a button in the window's style.
- System lines (joins, leaves, channel switches, kicks, TS Media's own notes) are centred, on small pills when bubbles are on. They never get bubbles or reactions.
- Avatars come from TeamSpeak itself, your own included. People TeamSpeak has no avatar for get their initials.
- Inside a bubble, file, audio and voice message cards and poll cards have no box of their own.
- **Show reactions** (Settings → Privacy & updates) covers reactions on messages and media.

### Fixed

- **Add files…** in the send window did nothing.
- Your own avatar showed your initials instead of your picture.
- Switching server tabs while holding the microphone button, or right after, no longer leaves the recording strip or its notices over the other tab's chat (all server tabs share one chat input in TeamSpeak 3.6.2). A hold that is still going on is kept in the *Voice message* window, never sent to the other tab.

## [2.2.1] - 2026-10-11

### Added

- **Chat layout** (**Settings → General → Chat layout**): TeamSpeak's chat in a Discord-like *Cozy* layout (the default) with names, avatars and times above the messages, or *Compact* with one line per message and the time in front; *TeamSpeak classic* keeps TeamSpeak's own look. Only your view changes.
  - Messages of one person within 7 minutes are grouped under one name; day dividers read *Today, October 10, 2026*; join, leave and other system lines become small rows with an icon and their time, also TeamSpeak's status lines in the server tab and in private chats (*You switched from channel …*, *Chat partner disconnected …*). A server's welcome and host messages keep their formatting and never fold, and where TeamSpeak's saved chat history begins a *History* divider shows its day.
  - Switching the layout, turning grouping on or off or resizing the chat keeps your place in it, also when the row at the top is folded into *+N more events*; opening or closing a fold at the bottom keeps following new messages.
  - Pointing at a message shows **Add reaction**, **Reply**, **Copy text** and **More**; replies show Discord's reply row above the name.
  - Messages that mention you are highlighted; runs of join and leave events fold into *+N more events*.
  - Links keep their colour and underline and get a full underline under the pointer; copying gives TeamSpeak's text.
  - Avatars are the pictures TeamSpeak already has (nothing is downloaded); people without one get their initials. Grouping, message actions, avatars, mentions and folding can each be turned off in the same settings group.
- A **microphone button** at the right end of the chat input for voice messages (hold it to record, see *Changed*). Where nothing can be sent (not connected, a password-protected channel, a private chat whose partner left) it is greyed out and its tooltip says why.

### Changed

- **Hold the microphone button to record, release to send**, like Telegram. Recording starts the moment you press it; a strip above the chat input shows a pulsing red dot, the time out of 5:00, a live waveform and *Release to send · Move away to cancel*.
  - Move the pointer away from the button before letting go to cancel (the strip turns red: *Release to cancel*); move back to send after all. <kbd>Esc</kbd> while holding cancels too.
  - A click (under half a second) records nothing and shows *Hold to record, release to send*; a double click starts nothing.
  - At 5:00 the message is sent by itself even while you still hold the button, and the strip says so. If the pointer is away from the button then, ready to cancel, it is kept in the *Voice message* window instead.
  - If TeamSpeak never sees you let go (<kbd>Alt</kbd>+<kbd>Tab</kbd>, a menu or another window took the mouse), or another chat comes up while you hold the button, the recording is never sent by itself: it stops and is kept in the *Voice message* window with **Send**.
  - **Plugins → TS Media chat → Record voice message…** and `/tsmedia voice` still record in the window, with the keyboard.
- Updates no longer depend on a signing key. Every download is checked against the SHA-256 listed in the GitHub release.
- TeamSpeak's own emoticon button next to the chat input now shows an HD face and opens TS Media's emoji picker. The extra smiley inside the chat input is gone, and its place at the right end of the input is now the microphone's (the smiley only comes back, left of the microphone, if TeamSpeak hides its button).
- Voice messages are sent as recorded, without a review step: in the window, **Send** (or <kbd>Enter</kbd>, or a click on the red microphone button) stops and sends at once, **Cancel** (<kbd>Esc</kbd>) discards. At 5:00 the message is sent by itself, and the window says so.
- A recording that stopped by itself (the microphone was unplugged, the window was hidden) or couldn't go out (not connected) is kept in the window: **Send** it, or close the window to discard it (from 3 seconds on it asks first).
- Voice messages always record from TeamSpeak's own microphone (Windows' default communications microphone when the plugin can't tell which one that is), always mute your TeamSpeak microphone while you record, and always play the short start and stop sounds.

### Removed

- The *Record a voice message* hotkey. The microphone button, **Plugins → TS Media chat → Record voice message…** and `/tsmedia voice` stay. A binding you made for it in TeamSpeak no longer does anything.
- The **Settings → General → Voice messages** group (*Microphone*, *Mute my TeamSpeak microphone while recording*, *Let me listen before sending (hotkey)*, *Play a sound when recording starts and stops*). Values saved by 2.2.0 are ignored.
- *Let me listen before sending*: the review step with play, **Re-record** and **Discard** is gone.

### Fixed

- Picking an emoji while the chat input was empty could send TeamSpeak's grey "Enter Chat Message..." text along with it.
- If the connection dropped while you recorded a voice message, your TeamSpeak microphone could stay muted. Now it is unmuted once TeamSpeak is connected there again.
- Quitting TeamSpeak while the voice message window was open could leave TeamSpeak running in the background without a window, for example while the window asked whether to discard a recording or while a recording of 3 seconds or more was being sent (also in 2.2.0).

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
- Reactions with any emoji under pictures, GIFs, videos, albums, audio files and voice messages, seen by people with TS Media who are online in the same chat: a quick row of eight (👍 ❤️ 😂 😮 😢 🔥 and your two most recent others) and **+** for the emoji picker. Up to 10 per person and 20 different ones per item.
- Replies: right-click a message → **Reply**, <kbd>Alt</kbd>+<kbd>↑</kbd> / <kbd>Alt</kbd>+<kbd>↓</kbd> in the chat input, or the *Reply to the last message in the current chat* hotkey. A bar above the chat input shows what you are replying to (<kbd>Enter</kbd> sends, <kbd>Esc</kbd> cancels), and files sent meanwhile go out as the reply. With the plugin, a reply shows a Discord-style reply line above it that jumps to the original, and the original gets **View N replies**; everyone else sees an italic quote line (*↪ Alice · 21∶14: “…”*) before the reply.
- HD emoji: emoji and TeamSpeak's smileys show as sharp Windows 11 pictures in the chat, and emoji also in reply lines, the reply bar and reactions; a message of only emoji (up to 27) shows them large. Point at one to see its name; copying keeps the emoji and smiley codes.
- Emoji picker: the smiley at the right end of the chat input, <kbd>Ctrl</kbd>+<kbd>E</kbd>, and the send window's caption field. About 1,600 emoji, search, recently used, skin tones and full keyboard use.
- TeamSpeak's own chat menu gets **Reply**, **View N replies** and **Copy text** at the top, and on an HD emoji its name, **Copy emoji** and **Use in the chat input**.
- **Settings → General → Emoji**: *Show emoji in high quality in the chat*, *Show messages of only emoji large* and *Show the emoji button in the chat input*.
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
- In TeamSpeak's dark chat, the text of TS Media chat's own lines (notices, warnings, `/tsmedia help`) was dark blue on dark grey and hard to read: now it is light. Light chats are unchanged.

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

[2.5.0]: https://github.com/Metihttp/Teamspeak_Media_chat/compare/v2.4.0...HEAD
[2.4.0]: https://github.com/Metihttp/Teamspeak_Media_chat/compare/v2.3.3...v2.4.0
[2.3.3]: https://github.com/Metihttp/Teamspeak_Media_chat/compare/v2.3.2...v2.3.3
[2.3.2]: https://github.com/Metihttp/Teamspeak_Media_chat/compare/v2.3.1...v2.3.2
[2.3.1]: https://github.com/Metihttp/Teamspeak_Media_chat/compare/v2.3.0...v2.3.1
[2.3.0]: https://github.com/Metihttp/Teamspeak_Media_chat/compare/v2.2.1...v2.3.0
[2.2.1]: https://github.com/Metihttp/Teamspeak_Media_chat/compare/v2.2.0...v2.2.1
[2.2.0]: https://github.com/Metihttp/Teamspeak_Media_chat/compare/v2.1.0...v2.2.0
[2.1.0]: https://github.com/Metihttp/Teamspeak_Media_chat/releases/tag/v2.1.0
[2.0.7]: https://github.com/Metihttp/Teamspeak_Media_chat/releases/tag/v2.0.7
[2.0.6]: https://github.com/Metihttp/Teamspeak_Media_chat/releases/tag/v2.0.6
