#include "net.hpp"
#include "env.hpp"
#include "config.hpp"
#include "version.h"

#include <winhttp.h>
#include <wininet.h>
#include <sstream>
#include <cstdio>

namespace net {

const wchar_t* const kDefaultProbeHost = L"raw.githubusercontent.com";

namespace {

const wchar_t* kUserAgent = L"WinKick/" WINKICK_VERSION_WSTRING;

// Prefer the system/PAC auto-proxy WinHTTP understands on Win8.1+ (corporate networks commonly
// use WPAD/PAC with no netsh winhttp configuration; DEFAULT_PROXY reads only the legacy static
// config and would bypass it). Fall back to DEFAULT_PROXY on kernels that reject it.
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

HINTERNET OpenSession() {
    HINTERNET s = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) return s;
    return WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
}

struct UrlParts {
    wchar_t scheme[16]{};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
};

bool CrackUrl(const std::wstring& url, UrlParts& out, std::string& outError) {
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszScheme = out.scheme; uc.dwSchemeLength = _countof(out.scheme);
    uc.lpszHostName = out.host; uc.dwHostNameLength = _countof(out.host);
    uc.lpszUrlPath = out.path;  uc.dwUrlPathLength = _countof(out.path);
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
        outError = "Could not parse URL (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    // https only — plaintext fetches would let an on-path attacker inject commands
    if (_wcsicmp(out.scheme, L"https") != 0) {
        outError = "Unsupported URL scheme (only https:// is supported).";
        return false;
    }
    out.port = uc.nPort;
    out.secure = true;
    return true;
}

// Append cache-buster for GitHub-hosted URLs (host match only; strips the fragment
// from the request target as WinHTTP does not expect one)
std::wstring CacheBustGitHub(const std::wstring& url) {
    // Separate host to decide whether to bust
    size_t schemeEnd = url.find(L"://");
    if (schemeEnd == std::wstring::npos) return url;
    size_t hostStart = schemeEnd + 3;
    size_t hostEnd = url.find_first_of(L"/?#", hostStart);
    std::wstring host = url.substr(hostStart, hostEnd == std::wstring::npos ? std::wstring::npos : hostEnd - hostStart);
    // Strip userinfo (user:pass@) and port (:port) before matching
    size_t at = host.find(L'@');
    if (at != std::wstring::npos) host = host.substr(at + 1);
    size_t colon = host.find(L':');
    if (colon != std::wstring::npos) host = host.substr(0, colon);
    std::wstring hostLower = host;
    for (auto& c : hostLower) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    auto endsWith = [](const std::wstring& h, const wchar_t* suffix) {
        size_t sl = wcslen(suffix);
        return h.size() > sl && h.compare(h.size() - sl, sl, suffix) == 0;
    };
    // Exact or proper domain-suffix match only (not "contains github anywhere")
    bool isGitHub = hostLower == L"github.com" || endsWith(hostLower, L".github.com") ||
                    hostLower == L"githubusercontent.com" || endsWith(hostLower, L".githubusercontent.com");
    if (!isGitHub) return url;

    // Drop any fragment from the request target (WinHTTP ignores/errors on fragments)
    size_t hash = url.find(L'#');
    std::wstring target = (hash == std::wstring::npos) ? url : url.substr(0, hash);

    wchar_t tick[32];
    swprintf(tick, _countof(tick), L"c=%llu", (unsigned long long)GetTickCount64());
    if (target.find(L'?') != std::wstring::npos) target += L"&";
    else                                        target += L"?";
    target += tick;
    return target;
}

} // namespace

// ============================================================================
// Connectivity: InternetGetConnectedState + quick WinHTTP probe
// ============================================================================
bool IsOnline(const wchar_t* probeHostUtf16) {
    DWORD flags = 0;
    if (!InternetGetConnectedState(&flags, 0)) return false;

    const wchar_t* host = probeHostUtf16 ? probeHostUtf16 : kDefaultProbeHost;

    HINTERNET session = OpenSession();
    if (!session) return false;

    // Short timeouts for the probe (resolve included — default is INFINITE)
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);

    bool ok = false;
    if (HINTERNET conn = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0)) {
        if (HINTERNET req = WinHttpOpenRequest(conn, L"HEAD", L"/", nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE)) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                ok = true; // any HTTP response at all => host reachable
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return ok;
}

// ============================================================================
// Fetch
// ============================================================================
bool FetchUrl(const std::wstring& url, std::string& outBody, std::string& outError) {
    outBody.clear();
    outError.clear();

    // WinHTTP does not expect a URL fragment; strip it for every target, not only GitHub
    std::wstring cleanUrl = url;
    size_t hash = cleanUrl.find(L'#');
    if (hash != std::wstring::npos) cleanUrl.erase(hash);

    std::wstring fetchUrl = CacheBustGitHub(cleanUrl);

    UrlParts parts;
    if (!CrackUrl(fetchUrl, parts, outError)) return false;

    HINTERNET session = OpenSession();
    if (!session) { outError = "WinHttpOpen failed (error " + std::to_string(GetLastError()) + ")."; return false; }

    // Resolve timeout was left at INFINITE before; blackholed DNS could stall startup for
    // the full OS DNS retry schedule instead of ~8 s.
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);

    bool ok = false;
    if (HINTERNET conn = WinHttpConnect(session, parts.host, parts.port, 0)) {
        DWORD flags = parts.secure ? WINHTTP_FLAG_SECURE : 0;
        if (HINTERNET req = WinHttpOpenRequest(conn, L"GET", parts.path, nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               flags)) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {

                // HTTP status
                DWORD statusCode = 0, len = sizeof(statusCode);
                BOOL qok = WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                               WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &len,
                                               WINHTTP_NO_HEADER_INDEX);
                if (!qok) {
                    outError = "Failed to query HTTP status (error " + std::to_string(GetLastError()) + ").";
                } else if (statusCode != 200) {
                    std::ostringstream ss;
                    ss << "HTTP " << statusCode;
                    outError = ss.str();
                } else {
                    std::string body;
                    DWORD available = 0;
                    bool readFailed = false;
                    for (;;) {
                        if (!WinHttpQueryDataAvailable(req, &available)) { readFailed = true; break; }
                        if (available == 0) break; // end of body
                        if (available > 1024 * 1024) available = 1024 * 1024; // clamp per-read chunk
                        std::string chunk(available, '\0');
                        DWORD read = 0;
                        if (!WinHttpReadData(req, chunk.data(), available, &read)) { readFailed = true; break; }
                        if (read == 0) break;  // defensive: never spin on a 0-byte success
                        chunk.resize(read);
                        body += chunk;
                        if (body.size() > 32u * 1024 * 1024) { outError = "Response too large (>32 MB)."; break; }
                    }
                    if (readFailed) outError = "Connection was lost while reading the response.";
                    if (outError.empty()) {
                        // Strip UTF-8 BOM if present
                        if (body.size() >= 3 && (unsigned char)body[0] == 0xEF &&
                            (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF) {
                            body.erase(0, 3);
                        }
                        outBody = std::move(body);
                        ok = true;
                    }
                }
            } else {
                outError = "HTTP request failed (error " + std::to_string(GetLastError()) + ").";
            }
            WinHttpCloseHandle(req);
        } else {
            outError = "WinHttpOpenRequest failed (error " + std::to_string(GetLastError()) + ").";
        }
        WinHttpCloseHandle(conn);
    } else {
        outError = "Could not connect to host '" + env::WideToUtf8(parts.host) + "' (error " +
                   std::to_string(GetLastError()) + ").";
    }
    WinHttpCloseHandle(session);
    return ok;
}

// ============================================================================
// Startup source resolution (priority: --config > --offline > --url > embedded)
// ============================================================================

bool LooksLikeIniContent(const std::string& body) {
    // Tolerate a leading UTF-8 BOM on the first line, and require BOTH a bracketed section header
    // and at least one `cmd = ...` key — a lone [...] line (JSON array, captive-portal "[notice]")
    // no longer passes as config.
    std::string stripped = body;
    if (stripped.size() >= 3 && (unsigned char)stripped[0] == 0xEF &&
        (unsigned char)stripped[1] == 0xBB && (unsigned char)stripped[2] == 0xBF) {
        stripped.erase(0, 3);
    }
    bool sawSection = false;
    bool sawCmd = false;
    size_t pos = 0;
    while (pos < stripped.size()) {
        size_t eol = stripped.find('\n', pos);
        if (eol == std::string::npos) eol = stripped.size();
        std::string line = stripped.substr(pos, eol - pos);
        pos = (eol < stripped.size()) ? eol + 1 : stripped.size();
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        size_t e = line.find_last_not_of(" \t\r\n");
        std::string t = line.substr(b, e - b + 1);
        // Skip comment lines (same rule as ParseIniBlocks): a commented-out "[section]" or
        // "cmd = ..." must not satisfy the validation check
        if (t.front() == '#' || t.front() == ';') continue;
        if (t.front() == '[' && t.back() == ']' && t.size() > 2) sawSection = true;
        size_t eq = t.find('=');
        if (sawSection && eq != std::string::npos) {
            std::string key = env::ToLower(env::Trim(t.substr(0, eq)));
            if (key == "cmd") sawCmd = true;
        }
        if (sawSection && sawCmd) return true;
    }
    return false;
}

SourceResolution ResolveSource(const SourceOptions& opts, bool& isOnlineOut) {
    SourceResolution res;
    isOnlineOut = false;

    // 1) --config: local file, explicit error if unreadable
    if (opts.hasConfigPath) {
        std::string data;
        if (!env::ReadFileUtf8(opts.configPath, data)) {
            res.ok = false;
            res.error = "Cannot read config file '" + env::WideToUtf8(opts.configPath) + "'.";
            isOnlineOut = false; // caller exits 2 without using it; never probe here
            return res;
        }
        res.ok = true;
        res.configText = std::move(data);
        res.sourceLabel = L"File: " + opts.configPath;
        if (!opts.offline) {
            // UAC hand-off instance: the unelevated launcher already established
            // connectivity before spawning us (it forwards --offline when not).
            // Invariant: --cleanup-config is only ever passed by HandOffForElevation
            // alongside --config, and HandOffForElevation forwards --offline whenever
            // the unelevated instance is not online — so cleanupConfig == true here
            // implies "launcher was online". Guard with IsTempJobFilePath so an arbitrary
            // --cleanup-config --config <file> cannot spoof connectivity or delete it.
            bool isJobFile = env::IsTempJobFilePath(opts.configPath);
            isOnlineOut = (opts.cleanupConfig && isJobFile) ? true : IsOnline();
        }
        return res;
    }

    // 2) --offline: skip network entirely
    if (opts.offline) {
        std::string embedded;
        if (!LoadEmbeddedDefaultTasksIni(embedded)) {
            res.ok = false; res.error = "Embedded default configuration resource is missing."; return res;
        }
        res.ok = true; res.configText = std::move(embedded);
        res.sourceLabel = L"Built-in (Embedded)";
        isOnlineOut = false;
        return res;
    }

    // Connectivity probe for network sources (used by the 'net' guard as well)
    if (opts.hasUrl) {
        // 3) --url: custom URL; explicit error if unreachable
        // The GET itself determines reachability (no separate HEAD probe needed)
        std::string body, err;
        bool fetchOk = FetchUrl(opts.url, body, err);
        if (!fetchOk) {
            res.ok = false;
            res.error = "Failed to fetch '" + env::WideToUtf8(opts.url) + "': " + err;
            isOnlineOut = false; // caller exits 2 without using it; never probe here
            return res;
        }
        if (!LooksLikeIniContent(body)) {
            res.ok = false;
            res.error = "Response from '" + env::WideToUtf8(opts.url) + "' is not a valid WinKick INI configuration (missing a [Section] header or cmd = entry).";
            return res;
        }
        res.ok = true; res.configText = std::move(body);
        res.sourceLabel = L"URL: " + opts.url;
        isOnlineOut = true;   // HTTP 200 from the target host: reachable, net guard may enable
        return res;
    }

    // 4) The default catalog is the one reviewed and embedded in this exact build.
    // Never fetch executable task definitions from a mutable URL implicitly.
    std::string embedded;
    if (!LoadEmbeddedDefaultTasksIni(embedded)) {
        res.ok = false; res.error = "Embedded default configuration resource is missing."; return res;
    }
    res.ok = true; res.configText = std::move(embedded);
    res.sourceLabel = L"Built-in (Embedded)";
    isOnlineOut = IsOnline(); // reachability only; no task definitions are downloaded
    return res;
}

} // namespace net
