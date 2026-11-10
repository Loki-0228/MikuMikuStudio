#pragma once

#include "mikumikustudio/recording_output_path.hpp"
#include <cstring>
#include <initializer_list>
#include <string>

template<class Check>
void CheckRecordingOutputPath(Check check) {
    using namespace mikumikustudio;
    using namespace recording_output_path;
    wchar_t temporary[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temporary);
    check(length != 0 && length < MAX_PATH, "find the temporary output-check directory");
    if (length == 0 || length >= MAX_PATH) return;
    const std::wstring root = std::wstring(temporary) + L"MMS-recording-path-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const bool created = CreateDirectoryW(root.c_str(), nullptr) != FALSE;
    check(created, "create an isolated recording output fixture");
    if (!created) return;
    const auto path = root + L"\\视频.mp4";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(file != INVALID_HANDLE_VALUE, "create an existing video fixture");
    if (file != INVALID_HANDLE_VALUE) {
        const char original[] = "preserve existing video bytes";
        DWORD written = 0;
        check(WriteFile(file, original, sizeof original, &written, nullptr) && written == sizeof original,
              "write the original output contents");
        CloseHandle(file);
        check(ProbeWritable(path.c_str()) == ERROR_SUCCESS, "existing writable output passes preflight");
        file = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        check(file != INVALID_HANDLE_VALUE, "open original output after preflight");
        if (file != INVALID_HANDLE_VALUE) {
            char actual[sizeof original + 1]{};
            DWORD read = 0;
            check(ReadFile(file, actual, sizeof actual, &read, nullptr) && read == sizeof original &&
                  std::memcmp(actual, original, sizeof original) == 0,
                  "preflight does not truncate or modify existing output bytes");
            check(ProbeWritable(path.c_str()) == ERROR_SHARING_VIOLATION,
                  "a conflicting file lock reports its Win32 sharing error");
            CloseHandle(file);
        }
        check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
              "make an existing output read-only");
        check(ProbeWritable(path.c_str()) == ERROR_ACCESS_DENIED,
              "read-only output reports access denied");
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        check(DeleteFileW(path.c_str()) != FALSE, "remove the owned existing output fixture");
    }
    check(ProbeWritable(path.c_str()) == ERROR_SUCCESS, "a new output can be created");
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    check(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) &&
          attributes.nFileSizeHigh == 0 && attributes.nFileSizeLow == 0,
          "new output retains the empty placeholder without writing sample data");
    check(DeleteFileW(path.c_str()) != FALSE, "remove the owned new output placeholder");
    check(ProbeWritable(root.c_str()) == ERROR_ACCESS_DENIED,
          "a directory cannot be used as the output file");
    check(ProbeWritable((root + L"\\missing\\video.mp4").c_str()) == ERROR_PATH_NOT_FOUND,
          "a missing parent directory reports the actual path error");
    check(ProbeWritable(nullptr) == ERROR_INVALID_NAME && ProbeWritable(L"") == ERROR_INVALID_NAME,
          "empty output paths report an invalid name");
    for (const unsigned char language : {kUiJapanese, kUiEnglish, kUiChinese}) {
        const auto message = FailureMessage(path.c_str(), ERROR_ACCESS_DENIED, language);
        check(message.find(path) != std::wstring::npos && message.find(L"Win32 5:") != std::wstring::npos,
              "each language reports the actual output path and error code");
    }
    check(RemoveDirectoryW(root.c_str()) != FALSE, "remove only the owned empty fixture directory");
}
