// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/platform/asset_seed.cpp
//
// See asset_seed.hpp for why the vault is copied out rather than read in place.
//
// Compiled on every platform, like the two files beside it. Off Android the body
// is one line.

#include <holocron/asset_seed.hpp>

namespace holocron {

const char* to_string(SeedState s)
{
    switch (s) {
    case SeedState::kDone:        return "vault seeded from the package";
    case SeedState::kUnsupported: return "this build has no packaged vault";
    case SeedState::kUnavailable: return "the packaged vault could not be opened";
    case SeedState::kFailed:      return "the packaged vault could not be written out";
    }
    return "unknown";
}

}  // namespace holocron

#ifdef __ANDROID__

#include <holocron/android_jni.hpp>

#include "android_jni_internal.hpp"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace holocron {

namespace {

// The directory inside the APK. Matches what scripts/android-apk.sh stages under
// `assets/`, and the two have to agree by hand -- there is no build step that
// checks, which is worth knowing before renaming either.
constexpr const char* kAssetVaultDir = "crystals";

// Copy one asset out, unless something is already there.
//
// STREAMING RATHER THAN AASSET_MODE_BUFFER. `aapt add` stores entries
// compressed, and a compressed asset has no mappable buffer -- AAsset_getBuffer
// returns null for exactly the entries this ships. Reading in chunks works for
// both, so the packaging tool's compression choice stops being something this
// code depends on.
bool copy_one(AAssetManager* mgr, const std::string& asset_path,
              const std::filesystem::path& out_path)
{
    AAsset* asset = AAssetManager_open(mgr, asset_path.c_str(), AASSET_MODE_STREAMING);
    if (asset == nullptr) {
        return false;
    }

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        AAsset_close(asset);
        return false;
    }

    std::vector<char> buffer(64 * 1024);
    for (;;) {
        const int n = AAsset_read(asset, buffer.data(), buffer.size());
        if (n < 0) {
            AAsset_close(asset);
            return false;
        }
        if (n == 0) {
            break;
        }
        out.write(buffer.data(), n);
        if (!out) {
            AAsset_close(asset);
            return false;
        }
    }
    AAsset_close(asset);
    out.close();
    return out.good();
}

}  // namespace

SeedReport seed_vault_from_assets(const std::string& destination)
{
    SeedReport report;

    android::ScopedEnv env;
    if (!env || android::activity() == nullptr) {
        report.state = SeedState::kUnavailable;
        return report;
    }

    // activity.getAssets()
    android::Local<jclass> cls(env.get(), env->GetObjectClass(android::activity()));
    if (env.failed("GetObjectClass Activity") || !cls) {
        report.state = SeedState::kUnavailable;
        return report;
    }
    const jmethodID get_assets =
        env->GetMethodID(cls.get(), "getAssets", "()Landroid/content/res/AssetManager;");
    if (env.failed("getAssets id") || get_assets == nullptr) {
        report.state = SeedState::kUnavailable;
        return report;
    }

    // HELD IN SCOPE FOR THE WHOLE FUNCTION. AAssetManager_fromJava hands back a
    // pointer that borrows from this object; letting the local reference die
    // while the pointer is still in use is a use-after-free with a Java object
    // on the other end of it.
    android::Local<jobject> assets(env.get(),
                                   env->CallObjectMethod(android::activity(), get_assets));
    if (env.failed("getAssets") || !assets) {
        report.state = SeedState::kUnavailable;
        return report;
    }

    AAssetManager* mgr = AAssetManager_fromJava(env.get(), assets.get());
    if (mgr == nullptr) {
        report.state = SeedState::kUnavailable;
        return report;
    }

    AAssetDir* dir = AAssetManager_openDir(mgr, kAssetVaultDir);
    if (dir == nullptr) {
        report.state = SeedState::kUnavailable;
        return report;
    }

    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec) {
        AAssetDir_close(dir);
        report.state = SeedState::kFailed;
        return report;
    }

    // AAssetDir_getNextFileName LISTS FILES ONLY AND DOES NOT RECURSE. That is
    // fine because the vault is flat -- a crystal is `<stem>.frag` plus
    // `<stem>.toml` and an archive is one `.toml`, all in one directory. If the
    // vault ever grows subdirectories this silently ships fewer files than it
    // should, which is why the shape is written down here rather than assumed.
    bool any_failed = false;
    while (const char* name = AAssetDir_getNextFileName(dir)) {
        const std::filesystem::path out_path =
            std::filesystem::path(destination) / name;

        if (std::filesystem::exists(out_path)) {
            ++report.skipped;  // never overwritten; see the header
            continue;
        }
        if (copy_one(mgr, std::string(kAssetVaultDir) + "/" + name, out_path)) {
            ++report.copied;
        } else {
            any_failed = true;
            // A partial file is worse than none: scan_vault would try to load it
            // and report a broken crystal, which sends the reader after the
            // wrong thing entirely.
            std::filesystem::remove(out_path, ec);
        }
    }
    AAssetDir_close(dir);

    report.state = any_failed ? SeedState::kFailed : SeedState::kDone;
    return report;
}

}  // namespace holocron

#else

namespace holocron {

// On Windows and Linux the vault ships beside the executable and there is
// nothing packaged to unpack. kUnsupported is the honest answer; the header says
// explicitly that it is not an error.
SeedReport seed_vault_from_assets(const std::string&)
{
    return SeedReport{};
}

}  // namespace holocron

#endif
