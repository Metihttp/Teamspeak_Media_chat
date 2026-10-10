# Server admin guide

[← Back to README](../README.md)

What a TeamSpeak server needs so its users can send and see media with TS Media chat.

The plugin uses TeamSpeak's own file transfer. **If a user can upload and download files in a channel with the file browser, the plugin works for them.** Nothing has to be installed or configured on the server, only permissions.

## One-click TS Media chat group

Since version 2.2, the plugin can set this up for you. Open **Plugins → TS Media chat → Settings…** while connected to the server and use the **Server access** box on the **Servers** tab:

1. Click **Create TS Media chat group**. The plugin creates a regular server group named `tsmediachat` with the TS Media chat picture icon. It shows each step and prints a line in the server's chat when it's done.
2. To let someone send files, right-click them and choose **Server Groups → tsmediachat** (TeamSpeak's own menu), or **TS Media chat → Give TS Media chat access**. **Remove TS Media chat access** takes it away again.

What the group contains (**Details…** in the box lists every permission):

- `i_ft_file_upload_power`, `i_ft_file_download_power`, `i_ft_file_browse_power` and `i_ft_directory_create_power` at **75** (what Server Admin has), with *skip* off like TeamSpeak's own groups. It doesn't get file delete or rename power.
- The upload and download quotas of your default group (unlimited if it has none).
- A copy of your default group's permissions (usually **Guest**). When a Guest is given any other server group, TeamSpeak takes them out of Guest, so without the copy they couldn't join channels or chat any more. Permissions marked *negate* are not copied, and permission-system powers never are.
- Protection: `i_group_needed_member_add_power` and `i_group_needed_member_remove_power` at 75 (or your own power if it is lower), so only admins can give the group out and nobody can add themselves. The protection is sent first; if the server refuses it, the plugin deletes the group it just created again.

If something is missing later (for example someone removed a permission), the box says what and offers **Repair TS Media chat group**, which only adds what's missing and keeps permissions you set yourself. The group is recognised by its name; after a rename, only if its permissions still match. The plugin never deletes or changes any other group, and it offers no Delete: remove the group in **Permissions → Server Groups** if you no longer want it.

Who can do what:

- Creating needs `b_virtualserver_servergroup_create` and `b_virtualserver_servergroup_permission_list`, and enough `i_permission_modify_power` and grant powers for the permissions above (a Server Admin has all of them).
- The icon needs `b_icon_manage` and `i_max_icon_filesize` of at least 335 bytes. Without them the group is created without its icon.
- Giving and removing the group needs `i_group_member_add_power` / `i_group_member_remove_power` of at least the group's protection value.

Channels whose `i_ft_needed_file_upload_power` is above 75 stay closed to members on purpose; the box says so for the channel you're in.

## Permissions

| Permission | Needed for | Rule |
| --- | --- | --- |
| `i_ft_file_upload_power` | sending (uploading) | at least the channel's `i_ft_needed_file_upload_power` |
| `i_ft_file_download_power` | seeing media inline, downloading | at least the channel's `i_ft_needed_file_download_power` |
| `i_ft_directory_create_power` | creating `/tsmedia` and `/tsmedia/previews` | at least the channel's `i_ft_needed_directory_create_power` |

Without `i_ft_directory_create_power`, files go to the root of the channel's file browser instead of `/tsmedia`.

> [!WARNING]
> On a default TeamSpeak server, the **Guest** server group cannot upload (its `i_ft_file_upload_power` is not high enough). New users can't send anything until you grant it to Guest or to the group your members are in.

### Granting permissions in the TeamSpeak client

1. Connect with an identity that has admin rights.
2. Open **Permissions → Server Groups**.
3. Select the group on the left (for example Guest, or the group your members are in).
4. Find the permissions in the **File Transfer** section. Untick *Show granted only* if they aren't listed.
5. Grant each permission with a value at least as high as the channel's matching *needed* value. A channel's needed values are shown in **Permissions → Channel Permissions**.

### Granting permissions with ServerQuery

Use `servergroupaddperm`, and find the group id with `servergrouplist`:

```text
servergroupaddperm sgid=<group id> permsid=i_ft_file_upload_power permvalue=<power> permnegated=0 permskip=0
```

Repeat it for `i_ft_file_download_power` and `i_ft_directory_create_power` if needed.

## Transfer quotas

- Transfer volume can be limited per client with `i_ft_quota_mb_upload_per_client` and `i_ft_quota_mb_download_per_client`, and for the whole virtual server with its upload and download quotas.
- When a download quota is used up, previews and cards show *Server transfer limit reached*. When an upload quota or the server's storage is full, sending fails with a message that asks the user to contact a server admin.
- Each user also has their own *Upload size limit* setting (100 MB by default), which they can set differently for your server in **Settings → Servers**. Server quotas apply on top of it.
- Since 2.2, videos over 25 MB are compressed to a 720p MP4 on the sender's computer before the upload (on by default), so large clips take far less space. A voice message takes about 0.73 MB per minute, at most about 3.7 MB (5 minutes).

## Folders and housekeeping

- `/tsmedia` is created automatically in each channel the first time someone sends a file there, and `/tsmedia/previews` the first time a preview is uploaded. Each user can change the folder name in their own settings, also for your server only.
- Previews are small JPEG files named like the media's random part (`/tsmedia/previews/<8 hex>.jpg`, or `<8 hex>.preview.jpg` next to the file when the previews folder can't be created).
- Voice messages are stored as `voice_message_<8 hex>.m4a`; compressed videos keep the original name with `.mp4`.
- The plugin never deletes sent files. It only removes the preview of an upload that was canceled or failed, and an uploaded file whose message was canceled before it was posted. Cleaning up old files is up to the server admins, in the channel's file browser.
- Clients check every download against the SHA-256 in its link (2.2 senders). If you replace a sent file on the server, people with the plugin get *File doesn't match what was sent* instead of the new content.

## Presence and reactions

TS Media chat 2.2 tells the people in a channel that it is installed (and its version), so the send window can count who will see a file in the chat, and it sends emoji reactions. Both use TeamSpeak's plugin commands to the channel or to a private-chat partner: nothing is stored on the server, nothing goes to the whole server, and the plugin paces them with the same flood protection as its chat messages. If your server refuses plugin commands, presence and reactions simply stay off there; sending and viewing files is not affected. Each user can turn both off in **Settings → Privacy & updates**.

## Limitations

- Uploading to **password-protected channels** is not supported, voice messages included; the plugin refuses with a message. Downloading a file stored in a password-protected channel fails with *Channel is password protected*.
- Clients must be able to reach the server's file transfer port (TCP 30033 by default). If TeamSpeak's own file browser doesn't work, the plugin can't work either.
- Everyone who wants to see media inline needs the plugin. Everyone else still gets a normal TeamSpeak download link, which needs the same download permission.
- Reactions are not available in the server chat.
