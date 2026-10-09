// 2.2 servergroup: TeamSpeak callbacks only the Server access feature uses. They live here instead of
// plugin.cpp so the feature stays in its own files; the callbacks plugin.cpp already exports (errors,
// transfers, connection changes, menus) forward to AccessGroup from there.
//
// Every one of them runs on a TeamSpeak thread: AccessGroup's handlers copy the arguments and continue
// on the GUI thread. If another feature needs one of these events later, move the export to plugin.cpp
// and call both handlers from it (a second definition would not link).

#include "accessgroup.h"

#define TS3_EXPORT extern "C" __declspec(dllexport)

TS3_EXPORT void ts3plugin_onServerGroupListEvent(uint64 serverConnectionHandlerID, uint64 serverGroupID, const char* name, int type, int iconID, int saveDB)
{
    (void)saveDB;
    AccessGroup::handleServerGroupList(serverConnectionHandlerID, serverGroupID, name, type, iconID);
}

TS3_EXPORT void ts3plugin_onServerGroupListFinishedEvent(uint64 serverConnectionHandlerID)
{
    AccessGroup::handleServerGroupListFinished(serverConnectionHandlerID);
}

TS3_EXPORT void ts3plugin_onServerGroupPermListEvent(uint64 serverConnectionHandlerID, uint64 serverGroupID, unsigned int permissionID, int permissionValue, int permissionNegated,
                                                     int permissionSkip)
{
    AccessGroup::handleServerGroupPermList(serverConnectionHandlerID, serverGroupID, permissionID, permissionValue, permissionNegated, permissionSkip);
}

TS3_EXPORT void ts3plugin_onServerGroupPermListFinishedEvent(uint64 serverConnectionHandlerID, uint64 serverGroupID)
{
    AccessGroup::handleServerGroupPermListFinished(serverConnectionHandlerID, serverGroupID);
}

TS3_EXPORT void ts3plugin_onServerGroupClientListEvent(uint64 serverConnectionHandlerID, uint64 serverGroupID, uint64 clientDatabaseID, const char* clientNameIdentifier,
                                                       const char* clientUniqueID)
{
    (void)clientNameIdentifier;
    (void)clientUniqueID;
    AccessGroup::handleServerGroupClientList(serverConnectionHandlerID, serverGroupID, clientDatabaseID);
}

TS3_EXPORT void ts3plugin_onServerGroupClientAddedEvent(uint64 serverConnectionHandlerID, anyID clientID, const char* clientName, const char* clientUniqueIdentity,
                                                        uint64 serverGroupID, anyID invokerClientID, const char* invokerName, const char* invokerUniqueIdentity)
{
    (void)clientName;
    (void)clientUniqueIdentity;
    (void)invokerClientID;
    (void)invokerName;
    (void)invokerUniqueIdentity;
    AccessGroup::handleServerGroupClientChanged(serverConnectionHandlerID, clientID, serverGroupID, true);
}

TS3_EXPORT void ts3plugin_onServerGroupClientDeletedEvent(uint64 serverConnectionHandlerID, anyID clientID, const char* clientName, const char* clientUniqueIdentity,
                                                          uint64 serverGroupID, anyID invokerClientID, const char* invokerName, const char* invokerUniqueIdentity)
{
    (void)clientName;
    (void)clientUniqueIdentity;
    (void)invokerClientID;
    (void)invokerName;
    (void)invokerUniqueIdentity;
    AccessGroup::handleServerGroupClientChanged(serverConnectionHandlerID, clientID, serverGroupID, false);
}

TS3_EXPORT void ts3plugin_onClientNeededPermissionsEvent(uint64 serverConnectionHandlerID, unsigned int permissionID, int permissionValue)
{
    AccessGroup::handleNeededPermission(serverConnectionHandlerID, permissionID, permissionValue);
}

TS3_EXPORT void ts3plugin_onClientNeededPermissionsFinishedEvent(uint64 serverConnectionHandlerID)
{
    AccessGroup::handleNeededPermissionsFinished(serverConnectionHandlerID);
}

TS3_EXPORT void ts3plugin_onChannelPermListEvent(uint64 serverConnectionHandlerID, uint64 channelID, unsigned int permissionID, int permissionValue, int permissionNegated,
                                                 int permissionSkip)
{
    (void)permissionNegated;
    (void)permissionSkip;
    AccessGroup::handleChannelPermList(serverConnectionHandlerID, channelID, permissionID, permissionValue);
}

TS3_EXPORT void ts3plugin_onChannelPermListFinishedEvent(uint64 serverConnectionHandlerID, uint64 channelID)
{
    AccessGroup::handleChannelPermListFinished(serverConnectionHandlerID, channelID);
}

TS3_EXPORT void ts3plugin_onFileInfoEvent(uint64 serverConnectionHandlerID, uint64 channelID, const char* name, uint64 size, uint64 datetime)
{
    (void)datetime;
    AccessGroup::handleFileInfo(serverConnectionHandlerID, channelID, name, size);
}

TS3_EXPORT void ts3plugin_onPermissionListEvent(uint64 serverConnectionHandlerID, unsigned int permissionID, const char* permissionName, const char* permissionDescription)
{
    (void)permissionDescription;
    AccessGroup::handlePermissionList(serverConnectionHandlerID, permissionID, permissionName);
}

TS3_EXPORT void ts3plugin_onServerUpdatedEvent(uint64 serverConnectionHandlerID)
{
    AccessGroup::handleServerUpdated(serverConnectionHandlerID);
}
