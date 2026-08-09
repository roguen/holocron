// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See vault.hpp.

#include <holocron/vault.hpp>

#include <holocron/archive.hpp>
#include <holocron/crystal.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace holocron {

std::vector<VaultEntry> scan_vault(const std::string& dir,
                                   std::vector<VaultProblem>& out_problems)
{
    namespace fs = std::filesystem;

    std::vector<VaultEntry> entries;

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        // Deliberately a problem rather than an empty result. A typo in a path
        // and an empty directory are the same thing to a caller that only counts
        // entries, and they need completely different fixes.
        out_problems.push_back(VaultProblem{dir, "not a directory: " + dir});
        return entries;
    }

    // Collect stems first, so the loading order below is ours and not the
    // filesystem's. Non-recursive on purpose: a vault is a directory of
    // crystals, and descending would make it ambiguous whether a subdirectory is
    // part of this vault or a separate one.
    std::vector<std::string> stems;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) {
            continue;
        }
        const fs::path& p = e.path();
        if (p.extension() != ".toml") {
            continue;   // a .frag alone is not a crystal -- see the header
        }
        fs::path stem = p;
        stem.replace_extension();
        stems.push_back(stem.string());
    }

    // Sorted before loading so the PROBLEM list is deterministic too, not only
    // the entries. An error report whose order depends on the filesystem is
    // needlessly hard to diff between two runs.
    std::sort(stems.begin(), stems.end());

    for (const std::string& stem : stems) {
        std::string detail;

        // WHICH LOADER, decided by reading the manifest rather than by an
        // extension. An archive is a `.toml` with `[[layer]]` in it; a crystal is
        // a `.toml` with a `.frag` beside it. Asking the file is what lets both
        // sit in one directory and one list.
        //
        // A manifest that will not parse falls through to load_crystal, which
        // produces the real diagnostic with a line number -- more use than
        // anything the archive path could say about a file it cannot read.
        std::string text;
        {
            std::ifstream in(stem + ".toml", std::ios::binary);
            if (in) {
                std::ostringstream ss;
                ss << in.rdbuf();
                text = ss.str();
            }
        }

        if (is_archive_manifest(text)) {
            Archive            a;
            const ArchiveError err = load_archive(stem, a, detail);
            if (err != ArchiveError::kOk) {
                out_problems.push_back(VaultProblem{stem, detail});
                continue;
            }

            // EVERY CRYSTAL IT NAMES IS LOADED TOO, here and not later. The
            // vault's whole promise is that a broken thing is reported before
            // anything is drawn rather than when somebody switches to it
            // mid-track, and an archive naming a crystal that does not exist is
            // exactly as broken as a crystal that does not compile.
            bool ok = true;
            for (const ArchiveLayer& layer : a.layers) {
                Crystal            c;
                std::string        why;
                const CrystalError cerr = load_crystal(layer.crystal, c, why);
                if (cerr != CrystalError::kOk) {
                    out_problems.push_back(VaultProblem{stem, "layer `" + layer.crystal +
                                                                  "`: " + why});
                    ok = false;
                    break;
                }
            }
            if (ok) {
                entries.push_back(VaultEntry{stem, a.name, true});
            }
            continue;
        }

        Crystal            c;
        const CrystalError err = load_crystal(stem, c, detail);
        if (err != CrystalError::kOk) {
            out_problems.push_back(VaultProblem{stem, detail});
            continue;
        }
        entries.push_back(VaultEntry{stem, c.name, false});
    }

    // By NAME, which is what a person sees, with the stem breaking ties so two
    // crystals that call themselves the same thing still have a stable order.
    std::sort(entries.begin(), entries.end(), [](const VaultEntry& a, const VaultEntry& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.stem < b.stem;
    });

    return entries;
}

}  // namespace holocron
