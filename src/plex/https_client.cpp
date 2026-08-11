// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See https_client.hpp.

#include <holocron/https_client.hpp>

#include <string>

#if defined(_WIN32)
// clang-format off
#include <windows.h>
#include <winhttp.h>
// clang-format on
#elif defined(__ANDROID__)
#include "../platform/android_jni_internal.hpp"

#include <holocron/android_jni.hpp>

#include <vector>
#endif

namespace holocron {

const char* to_string(HttpError e)
{
    switch (e) {
    case HttpError::kOk:            return "ok";
    case HttpError::kUnsupported:   return "this build has no HTTPS client";
    case HttpError::kConnectFailed: return "could not reach the host";
    case HttpError::kRequestFailed: return "the request failed";
    case HttpError::kBadUrl:        return "the address could not be used";
    }
    return "unknown";
}

std::string url_encode(const std::string& value)
{
    static constexpr char kHex[] = "0123456789ABCDEF";

    std::string out;
    out.reserve(value.size());
    for (const char raw : value) {
        // Explicit, because `char` is signed on the Linux job and an implicit
        // narrowing here is a -Wsign-conversion error under -Werror. The cast is
        // also the correct thing on its own terms: byte values above 127 must be
        // percent-encoded as unsigned, not as negative numbers.
        const auto c = static_cast<unsigned char>(raw);
        // RFC 3986 unreserved. Everything else is escaped, including '/' -- this
        // encodes VALUES, and a value that contains a slash is not a path
        // separator.
        const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s)
{
    if (s.empty()) {
        return {};
    }
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

struct Handle {
    HINTERNET h = nullptr;
    Handle()    = default;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle()
    {
        if (h != nullptr) {
            ::WinHttpCloseHandle(h);
        }
    }
    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

}  // namespace

HttpError https_request(const char* method, const std::string& host, std::uint16_t port,
                        const std::string& path,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        HttpsResponse& out, std::string& out_detail)
{
    out = HttpsResponse{};
    out_detail.clear();

    if (host.empty() || path.empty()) {
        out_detail = "empty host or path";
        return HttpError::kBadUrl;
    }

    Handle session(::WinHttpOpen(L"Holocron", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        out_detail = "WinHttpOpen failed (" + std::to_string(::GetLastError()) + ")";
        return HttpError::kConnectFailed;
    }

    Handle connect(::WinHttpConnect(session.h, widen(host).c_str(), port, 0));
    if (!connect) {
        out_detail = "cannot reach " + host + " (" + std::to_string(::GetLastError()) + ")";
        return HttpError::kConnectFailed;
    }

    Handle request(::WinHttpOpenRequest(connect.h, widen(method).c_str(), widen(path).c_str(),
                                        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE));
    if (!request) {
        out_detail = "WinHttpOpenRequest failed (" + std::to_string(::GetLastError()) + ")";
        return HttpError::kRequestFailed;
    }

    std::string header_block;
    for (const auto& [name, value] : headers) {
        header_block += name + ": " + value + "\r\n";
    }
    const std::wstring wide_headers = widen(header_block);

    if (::WinHttpSendRequest(request.h,
                             wide_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                  : wide_headers.c_str(),
                             static_cast<DWORD>(wide_headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0,
                             0) == FALSE) {
        // 12175 is ERROR_WINHTTP_SECURE_FAILURE, and it is the one worth naming:
        // it means the certificate did not validate, which for a Plex server
        // almost always means the address was rewritten to a bare IP instead of
        // using the *.plex.direct name the play command supplied.
        const DWORD code = ::GetLastError();
        out_detail       = "send failed (" + std::to_string(code) + ")";
        if (code == ERROR_WINHTTP_SECURE_FAILURE) {
            out_detail += " -- certificate did not validate; use the address from the "
                          "play command verbatim rather than an IP";
        }
        return HttpError::kRequestFailed;
    }
    if (::WinHttpReceiveResponse(request.h, nullptr) == FALSE) {
        out_detail = "no response (" + std::to_string(::GetLastError()) + ")";
        return HttpError::kRequestFailed;
    }

    DWORD status      = 0;
    DWORD status_size = sizeof(status);
    ::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX);
    out.status = static_cast<int>(status);

    for (;;) {
        DWORD available = 0;
        if (::WinHttpQueryDataAvailable(request.h, &available) == FALSE || available == 0) {
            break;
        }
        std::string chunk(available, '\0');
        DWORD       read = 0;
        if (::WinHttpReadData(request.h, chunk.data(), available, &read) == FALSE) {
            break;
        }
        out.body.append(chunk, 0, read);
    }

    // A 4xx is a successful REQUEST that got an unwelcome answer, and the body
    // is the only thing that says what was wrong. Returning an error here would
    // throw that away.
    return HttpError::kOk;
}

#elif defined(__ANDROID__)

// ---------------------------------------------------------------------------
// Android: java.net.HttpURLConnection through JNI.
//
// THE SAME TRADE THE HEADER ALREADY MADE. https_client.hpp declined OpenSSL for
// four small requests on the grounds that the platform SDK already ships a
// client. Android ships one too, it is the one every app on the device uses, and
// it validates against the system trust store -- which is the property the
// header calls out as worth saying aloud, because Plex makes turning it off
// tempting. Nothing here disables it.
//
// OpenSSL WAS THE ALTERNATIVE AND IS STILL REJECTED. cpp-httplib is already a
// dependency and can do TLS against OpenSSL, so adding `openssl` to vcpkg.json
// would give both platforms one code path and delete this file's two branches.
// It was rejected for the reason it was rejected the first time -- a large
// dependency, with its own GPL licence-exception handling, for four requests --
// and for a new one: it would mean shipping a second trust store inside the APK
// and being responsible for keeping it current, on a box in a rack that nobody
// logs into.
//
// TWO THINGS ABOUT ANDROID THAT ARE NOT VISIBLE IN THIS CODE
//
// 1. THE MANIFEST MUST DECLARE android.permission.INTERNET. Without it every
//    call here fails with a SecurityException and the symptom is "Plex cannot be
//    reached", which looks like a network problem and is not one.
//
// 2. NETWORK ON THE MAIN THREAD IS A HARD ERROR, not a warning:
//    NetworkOnMainThreadException, thrown by the VM since API 11. The link flow
//    and the track resolve must therefore not be called from the thread that
//    runs the render loop. They are not today -- the artwork and lyric fetches
//    already run on a worker, for the generation-counter reason in
//    track_context -- but `holocron --link` is a startup path, and on Android
//    that would have to move onto a thread of its own.
//
//    It is caught rather than crashed: the exception check below turns it into
//    kRequestFailed with the Java stack trace in logcat.
// ---------------------------------------------------------------------------

namespace {

// One read chunk. The largest thing fetched here is a Plex XML resource list,
// which is tens of kilobytes; the audio never comes through this path.
constexpr jsize kChunk = 16 * 1024;

}  // namespace

HttpError https_request(const char* method, const std::string& host, std::uint16_t port,
                        const std::string& path,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        HttpsResponse& out, std::string& out_detail)
{
    out = HttpsResponse{};
    out_detail.clear();

    if (method == nullptr || host.empty() || path.empty() || path[0] != '/') {
        return HttpError::kBadUrl;
    }

    holocron::android::ScopedEnv env;
    if (!env) {
        out_detail = holocron::android::has_java_vm()
                         ? "could not attach this thread to the Java VM"
                         : "no JavaVM; the entry point must call holocron::android::set_java_vm";
        return HttpError::kUnsupported;
    }

    if (env->PushLocalFrame(32) != JNI_OK) {
        return HttpError::kRequestFailed;
    }
    struct FramePopper {
        JNIEnv* e;
        ~FramePopper() { e->PopLocalFrame(nullptr); }
    } popper{env.get()};

    // The port is always written out. A Plex media server is reached on a
    // non-default port constantly, and `plex.direct` certificates are issued to
    // the name in the play command -- which is why the host string is used
    // verbatim here rather than resolved to an address first. See the header.
    std::string url = "https://" + host + ":" + std::to_string(port) + path;

    jclass c_url  = env->FindClass("java/net/URL");
    jclass c_conn = env->FindClass("java/net/HttpURLConnection");
    jclass c_in   = env->FindClass("java/io/InputStream");
    if (env.failed("FindClass")) {
        return HttpError::kRequestFailed;
    }

    jmethodID m_url_ctor = env->GetMethodID(c_url, "<init>", "(Ljava/lang/String;)V");
    jmethodID m_open =
        env->GetMethodID(c_url, "openConnection", "()Ljava/net/URLConnection;");
    if (env.failed("URL ids")) {
        return HttpError::kRequestFailed;
    }

    jstring j_url = env->NewStringUTF(url.c_str());
    jobject url_o = env->NewObject(c_url, m_url_ctor, j_url);
    if (env.failed("new URL")) {
        return HttpError::kBadUrl;
    }

    jobject conn = env->CallObjectMethod(url_o, m_open);
    if (env.failed("openConnection") || conn == nullptr) {
        return HttpError::kConnectFailed;
    }

    // openConnection() on an https: URL returns an HttpsURLConnection, which is
    // an HttpURLConnection. Everything used below is declared on the latter, so
    // there is no cast and no reason to name the TLS subclass at all.
    jmethodID m_set_method =
        env->GetMethodID(c_conn, "setRequestMethod", "(Ljava/lang/String;)V");
    jmethodID m_set_prop = env->GetMethodID(c_conn, "setRequestProperty",
                                            "(Ljava/lang/String;Ljava/lang/String;)V");
    jmethodID m_set_ctimeout  = env->GetMethodID(c_conn, "setConnectTimeout", "(I)V");
    jmethodID m_set_rtimeout  = env->GetMethodID(c_conn, "setReadTimeout", "(I)V");
    jmethodID m_set_redirects = env->GetMethodID(c_conn, "setInstanceFollowRedirects", "(Z)V");
    jmethodID m_code          = env->GetMethodID(c_conn, "getResponseCode", "()I");
    jmethodID m_input   = env->GetMethodID(c_conn, "getInputStream", "()Ljava/io/InputStream;");
    jmethodID m_error   = env->GetMethodID(c_conn, "getErrorStream", "()Ljava/io/InputStream;");
    jmethodID m_disconnect = env->GetMethodID(c_conn, "disconnect", "()V");
    if (env.failed("HttpURLConnection ids")) {
        return HttpError::kRequestFailed;
    }

    {
        jstring j_method = env->NewStringUTF(method);
        env->CallVoidMethod(conn, m_set_method, j_method);
        env->DeleteLocalRef(j_method);
        if (env.failed("setRequestMethod")) {
            return HttpError::kRequestFailed;
        }
    }

    // NO REDIRECTS, matching the Windows branch and the header's promise. Left
    // on, a redirect to plain http would silently downgrade the connection.
    env->CallVoidMethod(conn, m_set_redirects, JNI_FALSE);

    // Milliseconds. The Windows branch relies on WinHTTP's own defaults; these
    // are chosen to be shorter than a person's patience, because the one caller
    // that blocks a startup path is the link flow.
    env->CallVoidMethod(conn, m_set_ctimeout, static_cast<jint>(10000));
    env->CallVoidMethod(conn, m_set_rtimeout, static_cast<jint>(20000));
    if (env.failed("timeouts")) {
        return HttpError::kRequestFailed;
    }

    for (const auto& [name, value] : headers) {
        jstring j_name  = env->NewStringUTF(name.c_str());
        jstring j_value = env->NewStringUTF(value.c_str());
        env->CallVoidMethod(conn, m_set_prop, j_name, j_value);
        env->DeleteLocalRef(j_name);
        env->DeleteLocalRef(j_value);
        if (env.failed("setRequestProperty")) {
            return HttpError::kRequestFailed;
        }
    }

    const jint status = env->CallIntMethod(conn, m_code);
    if (env.failed("getResponseCode")) {
        // Connect failures, TLS failures and NetworkOnMainThreadException all
        // surface here. The stack trace is in logcat; this is what the caller
        // sees.
        out_detail = "the request threw -- see logcat for the Java stack trace";
        env->CallVoidMethod(conn, m_disconnect);
        env->ExceptionClear();
        return HttpError::kConnectFailed;
    }
    out.status = static_cast<int>(status);

    // A 4xx or 5xx IS NOT AN ERROR HERE -- the header says so, and the body of a
    // Plex error response is the only thing that says what was actually wrong.
    // Java splits that body onto a different accessor: getInputStream() throws
    // for a failing status and getErrorStream() is where the bytes are.
    jobject stream = env->CallObjectMethod(conn, m_input);
    if (env->ExceptionCheck() == JNI_TRUE) {
        env->ExceptionClear();
        stream = env->CallObjectMethod(conn, m_error);
        if (env->ExceptionCheck() == JNI_TRUE) {
            env->ExceptionClear();
            stream = nullptr;
        }
    }

    if (stream != nullptr) {
        jmethodID m_read  = env->GetMethodID(c_in, "read", "([B)I");
        jmethodID m_close = env->GetMethodID(c_in, "close", "()V");
        if (env.failed("InputStream ids")) {
            env->CallVoidMethod(conn, m_disconnect);
            return HttpError::kRequestFailed;
        }

        jbyteArray buffer = env->NewByteArray(kChunk);
        if (env.failed("NewByteArray") || buffer == nullptr) {
            env->CallVoidMethod(conn, m_disconnect);
            return HttpError::kRequestFailed;
        }

        std::vector<char> scratch(static_cast<std::size_t>(kChunk));
        for (;;) {
            const jint n = env->CallIntMethod(stream, m_read, buffer);
            if (env.failed("InputStream.read")) {
                out_detail = "the response body could not be read";
                env->CallVoidMethod(stream, m_close);
                env->ExceptionClear();
                env->CallVoidMethod(conn, m_disconnect);
                return HttpError::kRequestFailed;
            }
            if (n <= 0) {
                break;  // -1 is end of stream; 0 cannot happen for a non-empty array
            }

            env->GetByteArrayRegion(buffer, 0, n, reinterpret_cast<jbyte*>(scratch.data()));
            if (env.failed("GetByteArrayRegion")) {
                env->CallVoidMethod(stream, m_close);
                env->ExceptionClear();
                env->CallVoidMethod(conn, m_disconnect);
                return HttpError::kRequestFailed;
            }
            out.body.append(scratch.data(), static_cast<std::size_t>(n));
        }

        env->CallVoidMethod(stream, m_close);
        env->ExceptionClear();
    }

    env->CallVoidMethod(conn, m_disconnect);
    env->ExceptionClear();

    return HttpError::kOk;
}

#else  // no platform HTTPS client

HttpError https_request(const char*, const std::string&, std::uint16_t, const std::string&,
                        const std::vector<std::pair<std::string, std::string>>&, HttpsResponse& out,
                        std::string& out_detail)
{
    out        = HttpsResponse{};
    out_detail = "built without the WinHTTP path; see https_client.hpp";
    return HttpError::kUnsupported;
}

#endif  // _WIN32

}  // namespace holocron
