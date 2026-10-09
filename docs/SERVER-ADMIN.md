# Server admin guide

[← Back to README](../README.md)

What a TeamSpeak server needs so its users can send and see media with TS Media chat.

The plugin uses TeamSpeak's own file transfer. **If a user can upload and download files in a channel with the file browser, the plugin works for them.** Nothing has to be installed or configured on the server, only permissions.

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
- Each user also has their own *Upload size limit* setting (100 MB by default). Server quotas apply on top of it.

## Folders and housekeeping

- `/tsmedia` is created automatically in each channel the first time someone sends a file there, and `/tsmedia/previews` the first time a preview is uploaded. Each user can change the folder name in their own settings.
- Previews are small JPEG files stored next to the media (in `/tsmedia/previews`, or as `<name>.preview.jpg` next to the file when the previews folder can't be created).
- The plugin never deletes sent files. It only removes the preview of an upload that was canceled or failed. Cleaning up old files is up to the server admins, in the channel's file browser.

## Limitations

- Uploading to **password-protected channels** is not supported; the plugin refuses with a message. Downloading a file stored in a password-protected channel fails with *Channel is password protected*.
- Clients must be able to reach the server's file transfer port (TCP 30033 by default). If TeamSpeak's own file browser doesn't work, the plugin can't work either.
- Everyone who wants to see media inline needs the plugin. Everyone else still gets a normal TeamSpeak download link, which needs the same download permission.
