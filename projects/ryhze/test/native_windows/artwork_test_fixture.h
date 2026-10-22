#ifndef RYHZE_ARTWORK_TEST_FIXTURE_H_
#define RYHZE_ARTWORK_TEST_FIXTURE_H_
#include <windows.h>
#include <atomic>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
namespace artwork_test {
inline void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(std::string(message) + " (Win32=" + std::to_string(GetLastError()) + ")");
}
inline std::string Key(unsigned value) {
  std::string key(64, '0');
  constexpr char hex[] = "0123456789abcdef";
  for (size_t index = 0; index < 8; ++index) key[63 - index] = hex[(value >> (index * 4)) & 15];
  return key;
}
inline std::wstring Name(unsigned value) { const auto key = Key(value); return std::wstring(key.begin(), key.end()) + L".rart"; }
inline void Write(const std::wstring& path, const std::vector<uint8_t>& bytes, bool replace = false) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
      replace ? TRUNCATE_EXISTING : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(file != INVALID_HANDLE_VALUE, "fixture file open");
  DWORD count = 0;
  const bool okay = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count == bytes.size();
  CloseHandle(file);
  Check(okay, "fixture file write");
}
inline std::vector<uint8_t> Read(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(file != INVALID_HANDLE_VALUE, "fixture file read open");
  LARGE_INTEGER size{};
  Check(GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= 9 * 1024 * 1024, "fixture read bound");
  std::vector<uint8_t> result(static_cast<size_t>(size.QuadPart));
  DWORD count = 0;
  const bool okay = ReadFile(file, result.data(), static_cast<DWORD>(result.size()), &count, nullptr) && count == result.size();
  CloseHandle(file);
  Check(okay, "fixture file read");
  return result;
}
struct Fixture {
  std::wstring parent;
  Fixture() {
    static std::atomic<unsigned> sequence{0};
    wchar_t temp[32768]{};
    const auto length = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    Check(length > 0 && length < std::size(temp), "temporary path");
    const std::wstring created = std::wstring(temp) + L"ryhze-artwork-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence);
    Check(CreateDirectoryW(created.c_str(), nullptr) != FALSE, "unique temporary fixture");
    HANDLE directory = CreateFileW(created.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Check(directory != INVALID_HANDLE_VALUE, "fixture canonical handle");
    wchar_t canonical[32768]{};
    const auto count = GetFinalPathNameByHandleW(directory, canonical, static_cast<DWORD>(std::size(canonical)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(directory);
    Check(count > 4 && count < std::size(canonical), "canonical fixture length");
    parent = std::wstring(canonical, count);
    Check(parent.rfind(L"\\\\?\\", 0) == 0, "canonical fixture namespace");
    parent.erase(0, 4);
    std::wcout << L"FIXTURE retained: " << parent << L"\n";
  }
  std::wstring File(unsigned value) const { return parent + L"\\artwork\\" + Name(value); }
};
}  // namespace artwork_test
#endif
