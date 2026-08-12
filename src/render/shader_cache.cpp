// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See shader_cache.hpp.

#include <holocron/shader_cache.hpp>

#include "gl_api.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace holocron {

namespace {

// FNV-1a, 64-bit. Not a cryptographic choice and does not need to be: the
// consequence of a collision here is a wrong program, and the header also
// carries the source LENGTH and the driver strings, all of which must match. A
// collision would have to agree on every one of those.
std::uint64_t fnv1a(const std::string& text)
{
    // The cast is explicit because `char` is signed on both of this project's
    // compilers and -Wsign-conversion is an error on the Linux job. It also has
    // to happen before the widening: a byte above 0x7F would otherwise
    // sign-extend to a 64-bit value with the top 56 bits set, and the hash of a
    // shader containing any non-ASCII byte would differ between a signed-char and
    // an unsigned-char platform. Both would be stable, and they would disagree.
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : text) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ull;
    }
    return h;
}

// The file layout. Written and read in one place each, and versioned so a change
// to it is a miss rather than a misread.
//
// A truncated or corrupt file must be a MISS, never a crash: this runs on a
// television that loses power at the wall.
constexpr char          kMagic[8]  = {'H', 'O', 'L', 'O', 'S', 'H', 'D', '\2'};
constexpr std::size_t   kMaxBinary = 64u * 1024u * 1024u;

struct Header {
    char          magic[8];
    std::uint64_t driver_hash;
    std::uint64_t source_hash;
    std::uint64_t source_length;
    std::uint64_t format;
    std::uint64_t binary_length;
};

}  // namespace

struct ShaderCache::Impl {
    std::filesystem::path dir;
    std::string           driver;
    std::uint64_t         driver_hash = 0;
    bool                  ok          = false;
    std::string           why;

    mutable std::uint64_t hits   = 0;
    mutable std::uint64_t misses = 0;
    mutable std::uint64_t writes = 0;

    std::filesystem::path path_for(const std::string& source) const
    {
        char name[64];
        std::snprintf(name, sizeof(name), "%016llx-%016llx.bin",
                      static_cast<unsigned long long>(driver_hash),
                      static_cast<unsigned long long>(fnv1a(source)));
        return dir / name;
    }
};

ShaderCache::ShaderCache() : impl_(new Impl) {}
ShaderCache::~ShaderCache() { delete impl_; }

void ShaderCache::open(const std::string& directory)
{
    impl_->ok = false;
    impl_->why.clear();

    if (directory.empty()) {
        impl_->why = "no directory";
        return;
    }

    // A driver that reports no binary formats cannot round-trip a program, and
    // the spec permits exactly that. Checked before anything is written, because
    // the alternative is a directory quietly filling with files that can never
    // be read back.
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    if (formats < 1) {
        impl_->why = "this driver offers no program binary formats";
        return;
    }

    const auto str = [](GLenum name) -> std::string {
        const GLubyte* s = glGetString(name);
        return s == nullptr ? std::string{} : std::string(reinterpret_cast<const char*>(s));
    };
    // EVERY ONE OF THESE IS PART OF THE KEY. A driver update changes the version
    // string, which changes the key, which retires every binary written under
    // the old one without anybody having to remember to clear a directory.
    impl_->driver      = str(GL_VENDOR) + "|" + str(GL_RENDERER) + "|" + str(GL_VERSION);
    impl_->driver_hash = fnv1a(impl_->driver);

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        impl_->why = "cannot create " + directory + " -- " + ec.message();
        return;
    }

    // Writability is proved rather than assumed, and proved HERE rather than at
    // the first store(): a cache that reports itself available and then fails
    // every write is worse than one that says up front it is off.
    const auto probe = std::filesystem::path(directory) / ".writable";
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            impl_->why = directory + " is not writable";
            return;
        }
    }
    std::filesystem::remove(probe, ec);

    impl_->dir = directory;
    impl_->ok  = true;
}

bool ShaderCache::available() const { return impl_->ok; }

const std::string& ShaderCache::unavailable_reason() const { return impl_->why; }

void ShaderCache::prepare(std::uint32_t program) const
{
    if (!impl_->ok || program == 0) {
        return;
    }
    glProgramParameteri(static_cast<GLuint>(program), GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
}

std::uint32_t ShaderCache::load(const std::string& source) const
{
    if (!impl_->ok) {
        return 0;
    }

    const auto    path = impl_->path_for(source);
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        ++impl_->misses;
        return 0;
    }

    Header header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 ||
        header.driver_hash != impl_->driver_hash || header.source_hash != fnv1a(source) ||
        header.source_length != source.size() || header.binary_length == 0 ||
        header.binary_length > kMaxBinary) {
        ++impl_->misses;
        return 0;
    }

    std::vector<char> blob(static_cast<std::size_t>(header.binary_length));
    in.read(blob.data(), static_cast<std::streamsize>(blob.size()));
    if (in.gcount() != static_cast<std::streamsize>(blob.size())) {
        ++impl_->misses;
        return 0;
    }

    const GLuint program = glCreateProgram();
    if (program == 0) {
        ++impl_->misses;
        return 0;
    }
    glProgramBinary(program, static_cast<GLenum>(header.format), blob.data(),
                    static_cast<GLsizei>(blob.size()));

    // THE SPEC ALLOWS A DRIVER TO REFUSE A BINARY IT WROTE ITSELF, and to do it
    // without saying why. So the link status is checked exactly as it would be
    // after a real link, and a refusal is an ordinary miss.
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == 0) {
        glDeleteProgram(program);
        ++impl_->misses;
        return 0;
    }

    // Retrievable again, so a program restored from disk can still be re-stored
    // if the file is ever lost. Cheap, and it stops the cache from being
    // one-shot per file.
    prepare(program);

    ++impl_->hits;
    return program;
}

void ShaderCache::store(const std::string& source, std::uint32_t program) const
{
    if (!impl_->ok || program == 0) {
        return;
    }

    GLint length = 0;
    glGetProgramiv(static_cast<GLuint>(program), GL_PROGRAM_BINARY_LENGTH, &length);
    if (length <= 0 || static_cast<std::size_t>(length) > kMaxBinary) {
        // Zero means the driver kept nothing -- usually because the retrievable
        // hint was not set before linking. Silent, because there is nothing the
        // person watching can do about it and the player still works.
        return;
    }

    std::vector<char> blob(static_cast<std::size_t>(length));
    GLenum            format = 0;
    GLsizei           got    = 0;
    glGetProgramBinary(static_cast<GLuint>(program), length, &got, &format, blob.data());
    if (got <= 0) {
        return;
    }
    blob.resize(static_cast<std::size_t>(got));

    Header header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.driver_hash   = impl_->driver_hash;
    header.source_hash   = fnv1a(source);
    header.source_length = source.size();
    header.format        = static_cast<std::uint64_t>(format);
    header.binary_length = blob.size();

    // WRITTEN TO A TEMPORARY AND RENAMED. A half-written file is a file whose
    // header says one length and whose body is shorter, and while load() rejects
    // that, the rejection costs a compile every time until somebody deletes it.
    // The rename is the only step that makes the entry visible.
    const auto final_path = impl_->path_for(source);
    const auto temp_path  = std::filesystem::path(final_path).concat(".part");
    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return;
        }
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        if (!out) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(temp_path, ec);
            return;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temp_path, final_path, ec);
    if (ec) {
        std::filesystem::remove(temp_path, ec);
        return;
    }
    ++impl_->writes;
}

std::uint64_t ShaderCache::hits() const { return impl_->hits; }
std::uint64_t ShaderCache::misses() const { return impl_->misses; }
std::uint64_t ShaderCache::writes() const { return impl_->writes; }

}  // namespace holocron
