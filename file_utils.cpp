/**
 • Copyright (C) Huawei Technologies Co., Ltd. 2024-2026. All rights reserved.

 */

#include "logger.h"
#include "file_utils.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include "utils/file_util.h"
#include "datatime_util.h"

namespace DMContext::FileUtils {
std::string RealPath(const std::string &filePath)
{
    char *canonicalPath = realpath(filePath.c_str(), nullptr);
    if (canonicalPath == nullptr) {
        LOG_ERR("RealPath filePath = %s canonical error", filePath.c_str());
        return "";
    }
    std::string result(canonicalPath);
    free(canonicalPath);
    return result;
}

bool LoadContentFromPath(const std::string &filePath, std::string &content)
{
    try {
        if (filePath.empty()) {
            LOG_ERR("Invalid file path: empty string");
            return false;
        }
        std::filesystem::path fullPath = std::filesystem::absolute(filePath);
        LOG_INFO("full path is %s", fullPath.c_str());

        std::ifstream configFile(fullPath, std::ifstream::binary);
        if (!configFile.is_open()) {
            LOG_ERR("Unable to open %s file", filePath.c_str());
            return false;
        }

        configFile.seekg(0, std::ios::end);
        std::streamsize size = configFile.tellg();
        configFile.seekg(0, std::ios::beg);
        content.resize(size);

        if (content.empty()) {
            LOG_ERR("file is empty");
            return false;
        }

        if (!configFile.read(&content[0], size)) {
            LOG_ERR("read %s file fail", filePath.c_str());
            return false;
        }
        return true;
    } catch (const std::filesystem::filesystem_error &e) {
        LOG_ERR("Filesystem error: %s", e.what());
        return false;
    } catch (const std::exception &e) {
        LOG_ERR("Unexpected error: %s", e.what());
        return false;
    }
}

bool LoadLinesFromPath(const std::string &filePath, std::vector<std::string> &lines)
{
    try {
        std::filesystem::path fullPath = std::filesystem::absolute(filePath);
        LOG_INFO("full path is %s", fullPath.c_str());

        std::ifstream configFile(fullPath, std::ifstream::binary);
        if (!configFile.is_open()) {
            LOG_ERR("Unable to open %s file", filePath.c_str());
            return false;
        }

        std::string line;
        while (getline(configFile, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
        configFile.close();
        return true;
    } catch (const std::filesystem::filesystem_error &e) {
        LOG_ERR("Filesystem error: %s", e.what());
        return false;
    } catch (const std::exception &e) {
        LOG_ERR("Unexpected error: %s", e.what());
        return false;
    }
}

bool LoadSegmentDataFromPath(const std::string &filePath, std::vector<std::vector<std::string>> &data,
                             const char segChar)
{
    size_t fieldLength = 0;
    try {
        std::filesystem::path fullPath = std::filesystem::absolute(filePath);
        LOG_INFO("full path is %s", fullPath.c_str());

        std::ifstream configFile(fullPath, std::ifstream::binary);
        if (!configFile.is_open()) {
            LOG_ERR("Unable to open %s file", filePath.c_str());
            return false;
        }

        std::string line;
        while (getline(configFile, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            std::vector<std::string> segLine;
            std::stringstream ss(line);
            std::string field;
            while (getline(ss, field, segChar)) {
                segLine.push_back(field);
            }

            if (segLine.empty()) {
                continue;
            }

            if (fieldLength == 0) {
                fieldLength = segLine.size();
            }

            if (segLine.size() != fieldLength) {
                LOG_ERR("Field length error, line: %s, length: %zu", line.c_str(), segLine.size());
                continue;
            }
            data.push_back(segLine);
        }

        configFile.close();
        return true;
    } catch (const std::filesystem::filesystem_error &e) {
        LOG_ERR("Filesystem error: %s", e.what());
        return false;
    } catch (const std::exception &e) {
        LOG_ERR("Unexpected error: %s", e.what());
        return false;
    }
}

bool Write(const std::string &filePath, const std::string &content)
{
    if (filePath.empty() || content.empty()) {
        LOG_ERR("input param err, filePath: %s, content:%s", filePath.c_str(), content.c_str());
        return false;
    }
    try {
        std::filesystem::path path(filePath);
        path = std::filesystem::absolute(path).lexically_normal();
        if (!std::filesystem::exists(path.parent_path())) {
            std::filesystem::create_directories(path.parent_path());
        }

        std::ofstream file(path);
        if (!file.is_open()) {
            CMFrm_Error("Write filePath = %s error", filePath.c_str());
            return false;
        }

        file << content;
        file.close();

        LOG_ERR("write data success, filePath: %s, content:%s", filePath.c_str(), content.c_str());
        return true;
    } catch (std::filesystem::filesystem_error& e) {
        CMFrm_Error("TouchFile filePath = %s error = %s", filePath.c_str(), e.what());
    }
    return false;
}

bool WriteFile(const std::string &filePath, const std::string &fileContent, bool append, bool flush)
{
    try {
        if (filePath.empty()) {
            CMFrm_Error("Write file failed!, file is empty.");
            return false;
        }
        size_t fileLen = filePath.size();
        if (fileLen > FILENAME_MAX) {
            CMFrm_Error("file name illegal, filename %s", filePath.c_str());
            return false;
        }
        std::filesystem::path normalizedPath = std::filesystem::absolute(filePath).lexically_normal();
        std::ofstream ofs(normalizedPath, append ? std::ios::app | std::ios::out : std::ios::out);
        if (!ofs.is_open()) {
            CMFrm_Error("file %s not open", filePath.c_str());
            return false;
        }
        if (append) {
            ofs.seekp(0, std::ios::end);
        }
        ofs.write(fileContent.c_str(), fileContent.size());
        if (flush) {
            ofs.flush();
        }
        ofs.close();
        return true;
    } catch (const std::exception& e) {
        CMFrm_Error("Write file failed with an exception: %s", e.what());
        return false;
    }
}

void WriteModelLog(const std::string &input, const std::string &output, const std::string &promptName)
{
    std::string tmpDir = "/opt/coremind/logs/ModelLogs/other/";
    if (!CMFrm::Utils::MkDirs(tmpDir)) {
        LOG_ERR("Create model logs direction failed.");
        return;
    }
    auto currentTime = DateTimeUtil::GetCurrentTimeMillisecondStr();
    std::string name = promptName + currentTime;
    std::string filePath = tmpDir + name + ".log";
    std::string logInfo = "input:\n" + input + "\n\noutput:\n" + output + "\n";
    if (!FileUtils::WriteFile(filePath, logInfo, true)) {
        LOG_ERR("Write model log failed, currentTime: %s.", currentTime.c_str());
    }
}

void WriteModelLogX(const std::string &dir, const std::string &fileName, const std::string &input)
{
    std::string tmpDir = "/opt/coremind/logs/ModelLogs/" + dir;
    if (!CMFrm::Utils::MkDirs(tmpDir)) {
        LOG_ERR("Create model logs direction failed.");
        return;
    }
    auto curtime = DateTimeUtil::GetCurrentTimeMillisecondStr();
    std::string filePath = tmpDir + "/" + fileName;
    std::string logInfo = curtime + "\n" + input + "\n";
    if (!FileUtils::WriteFile(filePath, logInfo, true)) {
        LOG_ERR("Write model log failed");
    }
}
void WriteModelLogNoTime(const std::string &dir, const std::string &fileName, const std::string &input)
{
    std::string tmpDir = "/opt/coremind/logs/ModelLogs/" + dir;
    if (!CMFrm::Utils::MkDirs(tmpDir)) {
        LOG_ERR("Create model logs direction failed.");
        return;
    }
    auto curtime = DateTimeUtil::GetCurrentTimeMillisecondStr();
    std::string filePath = tmpDir + "/" + fileName;
    std::string logInfo = input + "\n";
    if (!FileUtils::WriteFile(filePath, logInfo, true)) {
        LOG_ERR("Write model log failed");
    }
}
}