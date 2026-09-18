#include "VdfParser.h"
#include "RaiiGuards.h"
#include "Utils.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string_view>

namespace OST::ExtractTickets {

std::vector<std::string> FindSteamLibraryFolders(const std::string& steamPath) {
    std::vector<std::string> libraries;
    if (!steamPath.empty()) {
        libraries.push_back(NormalizeDir(steamPath));
    }

    const std::string libraryVdfPath = JoinPath(steamPath, "steamapps\\libraryfolders.vdf");
    std::ifstream file(libraryVdfPath);
    if (!file) return libraries;

    auto addLibrary = [&](std::string lib) {
        std::string unescaped;
        unescaped.reserve(lib.size());
        for (size_t i = 0; i < lib.size(); ++i) {
            if (lib[i] == '\\' && i + 1 < lib.size() && lib[i + 1] == '\\') {
                unescaped += '\\';
                ++i;
            } else {
                unescaped += lib[i];
            }
        }
        unescaped = NormalizeDir(unescaped);
        if (!unescaped.empty() && std::none_of(libraries.begin(), libraries.end(), [&](const auto& existing) {
            return _stricmp(existing.c_str(), unescaped.c_str()) == 0;
        })) {
            libraries.push_back(std::move(unescaped));
        }
    };

    std::string line;
    while (std::getline(file, line)) {
        if (size_t comment = line.find("//"); comment != std::string::npos) {
            line.erase(comment);
        }

        auto tokens = TokenizeQuoted(line);

        if (tokens.size() >= 2) {
            if (_stricmp(tokens[0].c_str(), "path") == 0) {
                addLibrary(tokens[1]);
            } else if (IsDecimal(tokens[0])) {
                if (tokens[1].find(':') != std::string::npos ||
                    tokens[1].find('\\') != std::string::npos ||
                    tokens[1].find('/') != std::string::npos) {
                    addLibrary(tokens[1]);
                }
            }
        }
    }
    return libraries;
}

bool IsAppInstalledLocally(const std::string& steamPath, uint32_t appId) {
    if (steamPath.empty() || appId == 0) return false;
    const std::string manifestName = "appmanifest_" + std::to_string(appId) + ".acf";
    const auto libraries = FindSteamLibraryFolders(steamPath);
    for (const auto& lib : libraries) {
        const std::string manifestPath = JoinPath(lib, "steamapps\\" + manifestName);
        const DWORD attr = GetFileAttributesA(manifestPath.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> GetDepotcacheDirs(const std::string& steamPath, const std::vector<std::string>& libraries) {
    std::vector<std::string> dirs;
    auto addDir = [&](std::string d) {
        if (!d.empty() && std::none_of(dirs.begin(), dirs.end(), [&](const auto& existing) {
            return _stricmp(existing.c_str(), d.c_str()) == 0;
        })) {
            dirs.push_back(std::move(d));
        }
    };

    if (!steamPath.empty()) {
        addDir(JoinPath(steamPath, "depotcache"));
    }

    for (const auto& lib : libraries) {
        addDir(JoinPath(lib, "depotcache"));
        addDir(JoinPath(lib, "steamapps\\depotcache"));
    }
    return dirs;
}

std::vector<std::string> GetDepotcacheDirs(const std::string& steamPath) {
    return GetDepotcacheDirs(steamPath, FindSteamLibraryFolders(steamPath));
}

std::string FindDepotManifestFile(const std::vector<std::string>& depotcacheDirs,
                                  uint32_t depotId,
                                  std::string& inOutManifestId) {
    // 1. If inOutManifestId is known and valid decimal GID, check if <depotId>_<manifestId>.manifest exists directly
    if (IsValidManifestId(inOutManifestId)) {
        std::string expectedName = std::to_string(depotId) + "_" + inOutManifestId + ".manifest";
        for (const auto& dc : depotcacheDirs) {
            std::string fullPath = JoinPath(dc, expectedName);
            DWORD attr = GetFileAttributesA(fullPath.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return fullPath;
            }
        }
    }

    // 2. Search for <depotId>_*.manifest in depotcache dirs, choosing the latest modified file
    std::string bestPath;
    FILETIME bestTime{};
    std::string bestManifestId;

    for (const auto& dc : depotcacheDirs) {
        std::string pattern = JoinPath(dc, std::to_string(depotId) + "_*.manifest");
        WIN32_FIND_DATAA fd{};
        ScopedFindHandle hFind{FindFirstFileA(pattern.c_str(), &fd)};
        if (hFind.IsValid()) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::string_view fname{fd.cFileName};
                    size_t under = fname.find('_');
                    size_t dot = fname.rfind('.');
                    if (under != std::string_view::npos && dot != std::string_view::npos && dot > under + 1) {
                        std::string_view candidate = fname.substr(under + 1, dot - under - 1);
                        if (IsValidManifestId(candidate)) {
                            if (bestPath.empty() || CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) {
                                bestTime = fd.ftLastWriteTime;
                                bestPath = JoinPath(dc, fname);
                                bestManifestId = std::string(candidate);
                            }
                        }
                    }
                }
            } while (FindNextFileA(hFind, &fd));
        }
    }

    if (!bestPath.empty()) {
        if (!bestManifestId.empty()) {
            inOutManifestId = bestManifestId;
        }
        return bestPath;
    }

    return "";
}

void ParseAcfDepots(const std::string& acfPath,
                    std::unordered_map<uint32_t, std::string>& outDepots,
                    std::unordered_set<uint32_t>& outDlcIds,
                    std::unordered_map<uint32_t, uint32_t>& outDepotToDlc) {
    std::ifstream file(acfPath);
    if (!file) return;

    std::string line;
    bool inInstalledDepots = false;
    bool inMountedDepots = false;
    bool pendingInstalledDepots = false;
    bool pendingMountedDepots = false;
    uint32_t currentDepotId = 0;
    int braceDepth = 0;
    int installedDepth = -1;
    int mountedDepth = -1;

    while (std::getline(file, line)) {
        if (size_t comment = line.find("//"); comment != std::string::npos) {
            line.erase(comment);
        }

        auto tokens = TokenizeQuoted(line);

        if (!inInstalledDepots && !inMountedDepots) {
            for (const auto& tok : tokens) {
                if (_stricmp(tok.c_str(), "InstalledDepots") == 0) {
                    pendingInstalledDepots = true;
                    break;
                }
                if (_stricmp(tok.c_str(), "MountedDepots") == 0) {
                    pendingMountedDepots = true;
                    break;
                }
            }
        }

        bool inQuote = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (c == '"' && (i == 0 || line[i - 1] != '\\')) {
                inQuote = !inQuote;
            }
            if (inQuote) continue;

            if (c == '{') {
                braceDepth++;
                if (pendingInstalledDepots) {
                    inInstalledDepots = true;
                    installedDepth = braceDepth;
                    pendingInstalledDepots = false;
                } else if (pendingMountedDepots) {
                    inMountedDepots = true;
                    mountedDepth = braceDepth;
                    pendingMountedDepots = false;
                }
            } else if (c == '}') {
                if (inInstalledDepots && braceDepth == installedDepth) {
                    inInstalledDepots = false;
                    installedDepth = -1;
                    currentDepotId = 0;
                }
                if (inMountedDepots && braceDepth == mountedDepth) {
                    inMountedDepots = false;
                    mountedDepth = -1;
                }
                braceDepth--;
            }
        }

        if (inInstalledDepots) {
            if (tokens.size() == 1) {
                if (auto parsed = ParseAppId(tokens[0])) {
                    currentDepotId = *parsed;
                    outDepots.try_emplace(currentDepotId, "");
                    outDepotToDlc.try_emplace(currentDepotId, 0);
                }
            } else if (tokens.size() >= 2 && currentDepotId != 0) {
                if (_stricmp(tokens[0].c_str(), "manifest") == 0) {
                    outDepots[currentDepotId] = tokens[1];
                } else if (_stricmp(tokens[0].c_str(), "dlcappid") == 0) {
                    if (auto dlc = ParseAppId(tokens[1])) {
                        outDlcIds.insert(*dlc);
                        outDepotToDlc[currentDepotId] = *dlc;
                        outDepotToDlc[*dlc] = *dlc;
                        outDepots.try_emplace(*dlc, "");
                    }
                }
            }
        } else if (inMountedDepots) {
            if (tokens.size() >= 2 && IsValidManifestId(tokens[1])) {
                if (auto dId = ParseAppId(tokens[0])) {
                    if (outDepots[*dId].empty()) {
                        outDepots[*dId] = tokens[1];
                    }
                }
            }
        }
    }
}

std::unordered_map<uint32_t, std::string> ParseConfigVdfDepotKeys(const std::string& steamPath) {
    std::unordered_map<uint32_t, std::string> depotKeys;
    const std::string configPath = JoinPath(steamPath, "config\\config.vdf");
    std::ifstream file(configPath);
    if (!file) return depotKeys;

    std::string line;
    uint32_t currentDepotId = 0;
    bool inDepots = false;
    bool pendingDepots = false;
    int braceDepth = 0;
    int depotsDepth = -1;

    while (std::getline(file, line)) {
        if (size_t comment = line.find("//"); comment != std::string::npos) {
            line.erase(comment);
        }

        auto tokens = TokenizeQuoted(line);

        if (!inDepots) {
            for (const auto& tok : tokens) {
                if (_stricmp(tok.c_str(), "depots") == 0) {
                    pendingDepots = true;
                    break;
                }
            }
        }

        bool inQuote = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (c == '"' && (i == 0 || line[i - 1] != '\\')) {
                inQuote = !inQuote;
            }
            if (inQuote) continue;

            if (c == '{') {
                braceDepth++;
                if (pendingDepots) {
                    inDepots = true;
                    depotsDepth = braceDepth;
                    pendingDepots = false;
                }
            } else if (c == '}') {
                if (inDepots && braceDepth == depotsDepth) {
                    inDepots = false;
                    depotsDepth = -1;
                    currentDepotId = 0;
                }
                braceDepth--;
            }
        }

        if (!inDepots) continue;

        if (tokens.size() == 1) {
            if (auto parsed = ParseAppId(tokens[0])) {
                currentDepotId = *parsed;
            }
        } else if (tokens.size() >= 2) {
            if (_stricmp(tokens[0].c_str(), "DecryptionKey") == 0 && IsHex64(tokens[1]) && currentDepotId != 0) {
                depotKeys[currentDepotId] = tokens[1];
            }
        }
    }

    if (depotKeys.empty()) {
        file.clear();
        file.seekg(0, std::ios::end);
        const auto fileSize = file.tellg();
        if (fileSize > 0 && fileSize < 64 * 1024 * 1024) {
            file.seekg(0, std::ios::beg);
            std::string fullText(static_cast<size_t>(fileSize), '\0');
            file.read(fullText.data(), fileSize);

            size_t offset = 0;
            while ((offset = fullText.find("\"DecryptionKey\"", offset)) != std::string::npos) {
                size_t keyStart = fullText.find('"', offset + 15);
                if (keyStart != std::string::npos) {
                    size_t keyEnd = fullText.find('"', keyStart + 1);
                    if (keyEnd != std::string::npos && (keyEnd - keyStart - 1) == 64) {
                        std::string key = fullText.substr(keyStart + 1, 64);
                        if (IsHex64(key)) {
                            size_t searchBack = offset;
                            while (searchBack > 0 && fullText[searchBack] != '{') searchBack--;
                            size_t q2 = fullText.rfind('"', searchBack);
                            if (q2 != std::string::npos && q2 > 0) {
                                size_t q1 = fullText.rfind('"', q2 - 1);
                                if (q1 != std::string::npos) {
                                    std::string candidateId = fullText.substr(q1 + 1, q2 - q1 - 1);
                                    if (auto dId = ParseAppId(candidateId)) {
                                        depotKeys[*dId] = key;
                                    }
                                }
                            }
                        }
                    }
                }
                offset += 15;
            }
        }
    }

    return depotKeys;
}

} // namespace OST::ExtractTickets
