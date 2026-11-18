// WinKick - networking: WinHTTP fetch, CDN cache-buster, connectivity check
#pragma once

#include <string>

namespace net {

// InternetGetConnectedState() + a quick WinHTTP probe of the given host (or default host).
bool IsOnline(const wchar_t* probeHostUtf16 = nullptr);

// Fetch a URL over HTTPS (WINHTTP_FLAG_SECURE when scheme is https).
// Applies a cache-buster "?c=<tick>" / "&c=<tick>" to GitHub-hosted raw URLs.
// Returns true only for HTTP 200 with a (possibly large) body.
bool FetchUrl(const std::wstring& url, std::string& outBody, std::string& outError);

// Resolve the configuration source at startup. Fills config; sets isOnline.
// Order: --config > --offline > explicit --url > embedded catalog.
struct SourceResolution {
    bool ok = false;
    std::string error;        // set when ok == false
    std::wstring sourceLabel; // UTF-16 label used by the GUI for the combo entry
    std::string configText;   // UTF-8 INI text
};

struct SourceOptions {
    bool hasConfigPath = false;  std::wstring configPath;
    bool offline = false;
    bool hasUrl = false;         std::wstring url;
    bool cleanupConfig = false;  // UAC hand-off instance: launcher already established connectivity
};

SourceResolution ResolveSource(const SourceOptions& opts, bool& isOnlineOut);

// True when the body contains at least one [Section] line worth parsing as a WinKick INI.
bool LooksLikeIniContent(const std::string& body);

} // namespace net
