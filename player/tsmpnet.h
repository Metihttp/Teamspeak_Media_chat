#pragma once

// 2.6 chat proxy (design 3.6.1, 3.6.2, 3.6.5): the player's network rules, pure and header-only, shared by the helper
// (player/tsmp.cpp: no Qt) and the plugin (src/ytplayer/ytplayer.cpp, the tests). Nothing here touches a window, the
// registry or the network.
//
//  * argsFor: the browser arguments of an environment per network family. Every family gets YouTube's autoplay switch
//    and the WebRTC switch (no UDP outside the proxy settings, S10); direct adds --no-proxy-server; the bridge adds
//    --proxy-server=http://127.0.0.1:<port>. No user text ever enters them; never a bypass list or direct:// (fail
//    closed). UNSET: no environment at all.
//  * answersChallenge: the helper answers a BasicAuthenticationRequested only for the bridge's exact address and realm
//    (with the bridge's user and token, never the user's password); everything else is cancelled.
//  * overrideMentionsProxy: a WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS value or an AdditionalBrowserArguments policy that
//    mentions a proxy would replace the bridge in a non-elevated host (S9): the chat proxy doesn't play then.

#include "tsmp.h"

#include <string>
#include <vector>

namespace tsmpnet {

struct Net {
    int          mode       = TSMP_NET_UNSET;
    int          bridgePort = 0;
    std::wstring realm; // "tsmedia-<16 hex>"
    std::wstring user;  // "tsm-p"
    std::wstring token; // the bridge's token (43 base64url characters)
};

// "tsmedia-" and 16 lower-case hex digits (proxy::newBridgeRealm).
inline bool validRealm(const std::wstring& realm)
{
    static const wchar_t prefix[] = L"tsmedia-";
    if (realm.size() != 8 + 16 || realm.compare(0, 8, prefix) != 0)
        return false;
    for (size_t i = 8; i < realm.size(); ++i) {
        const wchar_t c = realm[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')))
            return false;
    }
    return true;
}

// "tsm-p" and at most 20 decimal digits (the bridge's user grammar for the player, design 3.4.3).
inline bool validUser(const std::wstring& user)
{
    if (user.size() < 5 || user.size() > 25 || user.compare(0, 5, L"tsm-p") != 0)
        return false;
    for (size_t i = 5; i < user.size(); ++i) {
        if (user[i] < L'0' || user[i] > L'9')
            return false;
    }
    return true;
}

// 16..128 base64url characters (the bridge's token is 43).
inline bool validToken(const std::wstring& token)
{
    if (token.size() < 16 || token.size() > 128)
        return false;
    for (const wchar_t c : token) {
        if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_'))
            return false;
    }
    return true;
}

// A network tsmp_setNetwork accepts: a known mode, and for the bridge a port 1024..65535, a realm, a user and a token.
inline bool validNetwork(const Net& net)
{
    switch (net.mode) {
    case TSMP_NET_UNSET:
    case TSMP_NET_SYSTEM:
    case TSMP_NET_DIRECT:
        return true;
    case TSMP_NET_BRIDGE:
        return net.bridgePort >= 1024 && net.bridgePort <= 65535 && validRealm(net.realm) && validUser(net.user) && validToken(net.token);
    default:
        return false;
    }
}

// An open may make (or use) an environment for this network: a valid network other than UNSET (S9).
inline bool openAllowed(const Net& net)
{
    return net.mode != TSMP_NET_UNSET && validNetwork(net);
}

// The browser arguments of an environment for a network family (empty: UNSET or unknown: no environment).
inline std::wstring argsFor(int mode, int bridgePort)
{
    if (mode != TSMP_NET_SYSTEM && mode != TSMP_NET_DIRECT && mode != TSMP_NET_BRIDGE)
        return {};
    std::wstring args = L"--autoplay-policy=no-user-gesture-required --force-webrtc-ip-handling-policy=disable_non_proxied_udp";
    if (mode == TSMP_NET_DIRECT)
        args += L" --no-proxy-server";
    if (mode == TSMP_NET_BRIDGE) {
        if (bridgePort < 1024 || bridgePort > 65535)
            return {};
        args += L" --proxy-server=http://127.0.0.1:" + std::to_wstring(bridgePort);
    }
    return args;
}

inline std::wstring argsFor(const Net& net)
{
    return argsFor(net.mode, net.bridgePort);
}

// The bridge's login request: the network is the bridge, the address is exactly http://127.0.0.1:<port>/ (or without
// the slash) and the challenge names the bridge's realm. Anything else (a website's 401, another proxy, https, another
// port or realm) is cancelled.
inline bool answersChallenge(const Net& net, const wchar_t* uri, const wchar_t* challenge)
{
    if (net.mode != TSMP_NET_BRIDGE || !validNetwork(net) || !uri || !challenge)
        return false;
    const std::wstring base = L"http://127.0.0.1:" + std::to_wstring(net.bridgePort);
    const std::wstring u    = uri;
    if (u != base && u != base + L"/")
        return false;
    const std::wstring realm = L"realm=\"" + net.realm + L"\"";
    return std::wstring(challenge).find(realm) != std::wstring::npos;
}

// "proxy" anywhere in the value, ASCII case-insensitive.
inline bool mentionsProxy(const std::wstring& value)
{
    static const wchar_t word[] = L"proxy";
    if (value.size() < 5)
        return false;
    for (size_t i = 0; i + 5 <= value.size(); ++i) {
        bool same = true;
        for (size_t k = 0; k < 5 && same; ++k) {
            wchar_t c = value[i + k];
            if (c >= L'A' && c <= L'Z')
                c = static_cast<wchar_t>(c - L'A' + L'a');
            same = c == word[k];
        }
        if (same)
            return true;
    }
    return false;
}

// Any of the override values (the environment variable, the HKCU and HKLM policies) mentions a proxy.
inline bool overrideMentionsProxy(const std::vector<std::wstring>& values)
{
    for (const std::wstring& v : values) {
        if (mentionsProxy(v))
            return true;
    }
    return false;
}

} // namespace tsmpnet
