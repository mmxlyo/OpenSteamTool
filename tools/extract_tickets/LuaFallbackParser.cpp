#include "LuaFallbackParser.h"
#include "RaiiGuards.h"
#include "Utils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <windows.h>

namespace OST::ExtractTickets {

namespace {

void ParseTomlLuaPaths(const std::string& tomlFilePath, std::vector<std::string>& outPaths) {
    std::ifstream file(tomlFilePath);
    if (!file) return;

    std::filesystem::path tomlPath(tomlFilePath);
    std::filesystem::path baseDir = tomlPath.parent_path();

    std::string line;
    bool inLuaSection = false;
    bool inPathsArray = false;

    while (std::getline(file, line)) {
        std::string_view sv = TrimWhitespace(line);
        if (sv.empty() || sv.starts_with('#')) continue;

        if (sv.starts_with('[') && sv.ends_with(']')) {
            std::string_view sec = sv.substr(1, sv.size() - 2);
            sec = TrimWhitespace(sec);
            std::string lowerSec(sec);
            std::transform(lowerSec.begin(), lowerSec.end(), lowerSec.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            inLuaSection = (lowerSec == "lua");
            inPathsArray = false;
            continue;
        }

        if (!inLuaSection) continue;

        if (!inPathsArray) {
            size_t pPos = sv.find("paths");
            if (pPos != std::string_view::npos) {
                size_t eqPos = sv.find('=', pPos + 5);
                if (eqPos != std::string_view::npos) {
                    size_t bracket = sv.find('[', eqPos + 1);
                    if (bracket != std::string_view::npos) {
                        inPathsArray = true;
                        sv = sv.substr(bracket + 1);
                    }
                }
            }
        }

        if (inPathsArray) {
            size_t pos = 0;
            while (pos < sv.size()) {
                if (sv[pos] == ']') {
                    inPathsArray = false;
                    break;
                }
                if (sv[pos] == '"') {
                    size_t closeQ = sv.find('"', pos + 1);
                    if (closeQ != std::string_view::npos) {
                        std::string pStr(sv.substr(pos + 1, closeQ - pos - 1));
                        std::filesystem::path p(pStr);
                        if (p.is_relative()) {
                            outPaths.push_back((baseDir / p).lexically_normal().string());
                        } else {
                            outPaths.push_back(p.lexically_normal().string());
                        }
                        pos = closeQ + 1;
                        continue;
                    }
                }
                ++pos;
            }
        }
    }
}

std::vector<std::string_view> SplitArgs(std::string_view argsStr) {
    std::vector<std::string_view> args;
    size_t start = 0;
    bool inQ = false;
    for (size_t i = 0; i < argsStr.size(); ++i) {
        if (argsStr[i] == '"') {
            inQ = !inQ;
        } else if (argsStr[i] == ',' && !inQ) {
            args.push_back(TrimWhitespace(argsStr.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (start <= argsStr.size()) {
        args.push_back(TrimWhitespace(argsStr.substr(start)));
    }
    return args;
}

std::string StripQuotes(std::string_view str) {
    std::string_view trimmed = TrimWhitespace(str);
    if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"') {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }
    return std::string(trimmed);
}

void ParseLuaContent(std::string_view content, uint32_t targetAppId, LuaFallbackData& out) {
    size_t pos = 0;
    uint32_t currentDlcId = 0;

    while (pos < content.size()) {
        size_t nextPos = content.find('\n', pos);
        std::string_view rawLine = (nextPos != std::string_view::npos)
            ? content.substr(pos, nextPos - pos)
            : content.substr(pos);
        pos = (nextPos != std::string_view::npos) ? nextPos + 1 : content.size();

        std::string_view line = TrimWhitespace(rawLine);
        if (line.empty() || line.starts_with("--")) {
            continue;
        }

        std::string comment;
        size_t commentPos = line.find("--");
        std::string_view code = line;
        if (commentPos != std::string_view::npos) {
            comment = SanitizeComment(TrimWhitespace(line.substr(commentPos + 2)));
            code = TrimWhitespace(line.substr(0, commentPos));
        }

        std::string lowerCode;
        lowerCode.reserve(code.size());
        for (char ch : code) {
            lowerCode.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }

        // 1. addappid(...)
        if (size_t fPos = lowerCode.find("addappid"); fPos != std::string::npos) {
            size_t openP = code.find('(', fPos);
            size_t closeP = code.rfind(')');
            if (openP != std::string_view::npos && closeP != std::string_view::npos && closeP > openP) {
                std::string_view argsStr = code.substr(openP + 1, closeP - openP - 1);
                auto args = SplitArgs(argsStr);
                if (!args.empty()) {
                    if (auto parsedId = ParseAppId(StripQuotes(args[0]))) {
                        uint32_t dId = *parsedId;
                        if (args.size() >= 3) {
                            std::string key = StripQuotes(args[2]);
                            if (key.size() == 64 && IsHex64(key)) {
                                out.depotKeys[dId] = key;
                            }
                        }

                        if (dId != targetAppId) {
                            out.dlcIds.insert(dId);
                            if (!comment.empty()) {
                                out.dlcNames.try_emplace(dId, comment);
                                currentDlcId = dId;
                            } else if (currentDlcId != 0 && dId > currentDlcId && dId <= currentDlcId + 20) {
                                out.depotToDlc.try_emplace(dId, currentDlcId);
                            }
                        }
                    }
                }
            }
        }
        // 2. setmanifestid(...)
        else if (size_t fPos = lowerCode.find("setmanifestid"); fPos != std::string::npos) {
            size_t openP = code.find('(', fPos);
            size_t closeP = code.rfind(')');
            if (openP != std::string_view::npos && closeP != std::string_view::npos && closeP > openP) {
                std::string_view argsStr = code.substr(openP + 1, closeP - openP - 1);
                auto args = SplitArgs(argsStr);
                if (args.size() >= 2) {
                    if (auto parsedId = ParseAppId(StripQuotes(args[0]))) {
                        uint32_t dId = *parsedId;
                        std::string manId = StripQuotes(args[1]);
                        if (IsValidManifestId(manId)) {
                            out.depotManifests[dId] = manId;
                        }
                    }
                }
            }
        }
        // 3. addtoken(...)
        else if (size_t fPos = lowerCode.find("addtoken"); fPos != std::string::npos) {
            size_t openP = code.find('(', fPos);
            size_t closeP = code.rfind(')');
            if (openP != std::string_view::npos && closeP != std::string_view::npos && closeP > openP) {
                std::string_view argsStr = code.substr(openP + 1, closeP - openP - 1);
                auto args = SplitArgs(argsStr);
                if (args.size() >= 2) {
                    if (auto parsedId = ParseAppId(StripQuotes(args[0]))) {
                        uint32_t tAppId = *parsedId;
                        std::string tokStr = StripQuotes(args[1]);
                        uint64_t tokenVal = 0;
                        auto [ptr, ec] = std::from_chars(tokStr.data(), tokStr.data() + tokStr.size(), tokenVal);
                        if (ec == std::errc{} && ptr == tokStr.data() + tokStr.size() && tokenVal != 0) {
                            out.appTokens[tAppId] = tokenVal;
                        }
                    }
                }
            }
        }
    }
}

void ScanManifestFilesInDir(const std::string& dir, LuaFallbackData& out) {
    std::string pattern = JoinPath(dir, "*.manifest");
    WIN32_FIND_DATAA fd{};
    ScopedFindHandle hFind{FindFirstFileA(pattern.c_str(), &fd)};
    if (!hFind.IsValid()) return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::string_view fname{fd.cFileName};
            size_t under = fname.find('_');
            size_t dot = fname.rfind('.');
            if (under != std::string_view::npos && dot != std::string_view::npos && dot > under + 1) {
                std::string_view depotStr = fname.substr(0, under);
                std::string_view candidateMan = fname.substr(under + 1, dot - under - 1);
                if (auto dId = ParseAppId(depotStr)) {
                    if (IsValidManifestId(candidateMan)) {
                        out.manifestFiles[*dId] = JoinPath(dir, fname);
                        out.depotManifests.try_emplace(*dId, std::string(candidateMan));
                    }
                }
            }
        }
    } while (FindNextFileA(hFind, &fd));
}

} // namespace

std::vector<std::string> GetOstLuaSearchDirectories(const std::string& steamPath) {
    std::vector<std::string> dirs;

    auto addDir = [&](const std::string& path) {
        if (path.empty()) return;
        std::string norm = NormalizeDir(path);
        if (norm.empty()) return;

        DWORD attr = GetFileAttributesA(norm.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            if (std::none_of(dirs.begin(), dirs.end(), [&](const std::string& existing) {
                return _stricmp(existing.c_str(), norm.c_str()) == 0;
            })) {
                dirs.push_back(std::move(norm));
            }
        }
    };

    // 1. Steam official config directories
    if (!steamPath.empty()) {
        addDir(JoinPath(steamPath, "config\\lua"));
        addDir(JoinPath(steamPath, "config\\st_config"));
    }

    // 2. Executable directory
    char exeBuf[MAX_PATH]{};
    std::string exeDir;
    if (GetModuleFileNameA(nullptr, exeBuf, MAX_PATH) > 0) {
        std::string exePath{exeBuf};
        size_t slash = exePath.find_last_of("\\/");
        if (slash != std::string_view::npos) {
            exeDir = exePath.substr(0, slash);
            addDir(JoinPath(exeDir, "config\\lua"));
            addDir(JoinPath(exeDir, "lua"));
            addDir(JoinPath(exeDir, "st_config"));
            addDir(exeDir);
        }
    }

    // 3. Current working directory
    char cwdBuf[MAX_PATH]{};
    std::string cwd;
    if (GetCurrentDirectoryA(MAX_PATH, cwdBuf) > 0) {
        cwd = std::string{cwdBuf};
        addDir(JoinPath(cwd, "config\\lua"));
        addDir(JoinPath(cwd, "lua"));
        addDir(JoinPath(cwd, "st_config"));
        addDir(cwd);
    }

    // 4. opensteamtool.toml configured lua paths
    std::vector<std::string> tomlLocations;
    if (!exeDir.empty()) tomlLocations.push_back(JoinPath(exeDir, "opensteamtool.toml"));
    if (!cwd.empty() && _stricmp(cwd.c_str(), exeDir.c_str()) != 0) {
        tomlLocations.push_back(JoinPath(cwd, "opensteamtool.toml"));
    }
    if (!steamPath.empty()) {
        tomlLocations.push_back(JoinPath(steamPath, "opensteamtool.toml"));
    }

    std::vector<std::string> tomlLuaPaths;
    for (const auto& tomlFile : tomlLocations) {
        if (GetFileAttributesA(tomlFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ParseTomlLuaPaths(tomlFile, tomlLuaPaths);
        }
    }

    for (const auto& p : tomlLuaPaths) {
        addDir(p);
    }

    return dirs;
}

LuaFallbackData ParseLuaFallbackData(const std::string& steamPath, uint32_t targetAppId) {
    LuaFallbackData result;
    if (targetAppId == 0) return result;

    const std::vector<std::string> searchDirs = GetOstLuaSearchDirectories(steamPath);
    const std::string targetLuaName = std::to_string(targetAppId) + ".lua";

    std::vector<std::string> matchedFiles;
    std::vector<std::string> manifestDirs;

    // First, look specifically for <targetAppId>.lua and dedicated <targetAppId>/ folder
    for (const auto& dir : searchDirs) {
        std::string directFile = JoinPath(dir, targetLuaName);
        DWORD attr = GetFileAttributesA(directFile.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            matchedFiles.push_back(directFile);
            manifestDirs.push_back(dir);
        }

        std::string subDir = JoinPath(dir, std::to_string(targetAppId));
        DWORD subAttr = GetFileAttributesA(subDir.c_str());
        if (subAttr != INVALID_FILE_ATTRIBUTES && (subAttr & FILE_ATTRIBUTE_DIRECTORY)) {
            std::string subFile = JoinPath(subDir, targetLuaName);
            if (GetFileAttributesA(subFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
                matchedFiles.push_back(subFile);
            }
            manifestDirs.push_back(subDir);
        }
    }

    // If no direct <targetAppId>.lua was found, scan all .lua files in the search directories
    if (matchedFiles.empty()) {
        for (const auto& dir : searchDirs) {
            std::string pattern = JoinPath(dir, "*.lua");
            WIN32_FIND_DATAA fd{};
            ScopedFindHandle hFind{FindFirstFileA(pattern.c_str(), &fd)};
            if (hFind.IsValid()) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        matchedFiles.push_back(JoinPath(dir, fd.cFileName));
                        manifestDirs.push_back(dir);
                    }
                } while (FindNextFileA(hFind, &fd));
            }
        }
    }

    for (const auto& filePath : matchedFiles) {
        std::ifstream file(filePath, std::ios::binary);
        if (!file) continue;

        file.seekg(0, std::ios::end);
        const auto size = file.tellg();
        if (size <= 0 || size > 16 * 1024 * 1024) continue;

        file.seekg(0, std::ios::beg);
        std::string content(static_cast<size_t>(size), '\0');
        file.read(content.data(), size);

        // If scanning a generic Lua file, ensure it refers to targetAppId
        if (filePath.find(targetLuaName) == std::string::npos) {
            std::string targetPattern = "addappid(" + std::to_string(targetAppId);
            std::string lowerContent;
            lowerContent.reserve(content.size());
            for (char ch : content) {
                if (ch != ' ' && ch != '\t') {
                    lowerContent.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
                }
            }
            if (lowerContent.find(targetPattern) == std::string::npos) {
                continue;
            }
        }

        ParseLuaContent(content, targetAppId, result);
    }

    // Scan for co-located .manifest files in the matched directories
    for (const auto& mDir : manifestDirs) {
        ScanManifestFilesInDir(mDir, result);
    }

    return result;
}

} // namespace OST::ExtractTickets
