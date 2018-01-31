/*
** Taiga
** Copyright (C) 2010-2018, Eren Okka
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <fstream>
#include <shlobj.h>

#include <windows/win/error.h>
#include <windows/win/registry.h>

#include "file.h"
#include "format.h"
#include "string.h"
#include "time.h"

HANDLE OpenFileForGenericRead(const std::wstring& path) {
  return ::CreateFile(GetExtendedLengthPath(path).c_str(),
                      GENERIC_READ, FILE_SHARE_READ, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

HANDLE OpenFileForGenericWrite(const std::wstring& path) {
  return ::CreateFile(GetExtendedLengthPath(path).c_str(),
                      GENERIC_WRITE, 0, nullptr,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

////////////////////////////////////////////////////////////////////////////////

unsigned long GetFileAge(const std::wstring& path) {
  HANDLE file_handle = OpenFileForGenericRead(path);

  if (file_handle == INVALID_HANDLE_VALUE)
    return 0;

  // Get the time the file was last modified
  FILETIME ft_file;
  BOOL result = GetFileTime(file_handle, nullptr, nullptr, &ft_file);
  CloseHandle(file_handle);

  if (!result)
    return 0;

  // Get current time
  SYSTEMTIME st_now;
  GetSystemTime(&st_now);
  FILETIME ft_now;
  SystemTimeToFileTime(&st_now, &ft_now);

  // Convert to ULARGE_INTEGER
  ULARGE_INTEGER ul_file;
  ul_file.LowPart = ft_file.dwLowDateTime;
  ul_file.HighPart = ft_file.dwHighDateTime;
  ULARGE_INTEGER ul_now;
  ul_now.LowPart = ft_now.dwLowDateTime;
  ul_now.HighPart = ft_now.dwHighDateTime;

  // Return difference in seconds
  return static_cast<unsigned long>(
      (ul_now.QuadPart - ul_file.QuadPart) / 10000000);
}

std::wstring GetFileLastModifiedDate(const std::wstring& path) {
  HANDLE file_handle = OpenFileForGenericRead(path);
  if (file_handle != INVALID_HANDLE_VALUE) {
    FILETIME ft_file = {0};
    BOOL result = GetFileTime(file_handle, nullptr, nullptr, &ft_file);
    CloseHandle(file_handle);
    if (result) {
      SYSTEMTIME st_file = {0};
      result = FileTimeToSystemTime(&ft_file, &st_file);
      if (result)
        return static_cast<std::wstring>(Date(st_file));
    }
  }

  return std::wstring();
}

QWORD GetFileSize(const std::wstring& path) {
  QWORD file_size = 0;

  HANDLE file_handle = OpenFileForGenericRead(path);

  if (file_handle != INVALID_HANDLE_VALUE) {
    LARGE_INTEGER size;
    if (::GetFileSizeEx(file_handle, &size))
      file_size = size.QuadPart;
    CloseHandle(file_handle);
  }

  return file_size;
}

QWORD GetFolderSize(const std::wstring& path, bool recursive) {
  QWORD folder_size = 0;
  const QWORD max_dword = static_cast<QWORD>(MAXDWORD) + 1;

  auto OnFile = [&](const std::wstring& root, const std::wstring& name,
                    const WIN32_FIND_DATA& data) {
    folder_size += static_cast<QWORD>(data.nFileSizeHigh) * max_dword +
                   static_cast<QWORD>(data.nFileSizeLow);
    return false;
  };

  FileSearchHelper helper;
  helper.set_log_errors(false);
  helper.set_skip_subdirectories(!recursive);
  helper.Search(path, nullptr, OnFile);

  return folder_size;
}

////////////////////////////////////////////////////////////////////////////////

bool Execute(const std::wstring& path, const std::wstring& parameters,
             int show_command) {
  if (path.empty())
    return false;

  if (path.length() > MAX_PATH)
    return ExecuteFile(path, parameters);

  auto value = ShellExecute(nullptr, L"open", path.c_str(), parameters.c_str(),
                            nullptr, show_command);
  return reinterpret_cast<int>(value) > 32;
}

bool ExecuteFile(const std::wstring& path, std::wstring parameters) {
  std::wstring exe_path = GetDefaultAppPath(L"." + GetFileExtension(path), L"");

  if (exe_path.empty())
    return false;

  parameters = LR"("{}" "{}" {})"_format(
      exe_path, GetExtendedLengthPath(path), parameters);

  PROCESS_INFORMATION process_information = {0};
  STARTUPINFO startup_info = {sizeof(STARTUPINFO)};

  if (!CreateProcess(nullptr, const_cast<wchar_t*>(parameters.c_str()),
                     nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                     &startup_info, &process_information)) {
    return false;
  }

  CloseHandle(process_information.hProcess);
  CloseHandle(process_information.hThread);

  return true;
}

void ExecuteLink(const std::wstring& link) {
  ShellExecute(nullptr, nullptr, link.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

////////////////////////////////////////////////////////////////////////////////

bool OpenFolderAndSelectFile(const std::wstring& path) {
  HRESULT result = S_FALSE;
  const auto pidl = ::ILCreateFromPath(path.c_str());
  if (pidl) {
    result = ::SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ::ILFree(pidl);
  }
  return result == S_OK;
}

bool CreateFolder(const std::wstring& path) {
  auto result = SHCreateDirectoryEx(nullptr, path.c_str(), nullptr);
  return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS;
}

int DeleteFolder(std::wstring path) {
  if (path.back() == '\\')
    path.pop_back();

  path.push_back('\0');

  SHFILEOPSTRUCT fos = {0};
  fos.wFunc = FO_DELETE;
  fos.pFrom = path.c_str();
  fos.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

  return SHFileOperation(&fos);
}

// Extends the length limit from 260 to 32767 characters
// See: http://msdn.microsoft.com/en-us/library/windows/desktop/aa365247%28v=vs.85%29.aspx#maxpath
std::wstring GetExtendedLengthPath(const std::wstring& path) {
  const std::wstring prefix = LR"(\\?\)";

  if (StartsWith(path, prefix))
    return path;

  // "\\computer\path" -> "\\?\UNC\computer\path"
  if (StartsWith(path, LR"(\\)"))
    return prefix + LR"(UNC\)" + path.substr(2);

  // "C:\path" -> "\\?\C:\path"
  return prefix + path;
}

bool IsDirectory(const WIN32_FIND_DATA& find_data) {
  return (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsHiddenFile(const WIN32_FIND_DATA& find_data) {
  return (find_data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
}

bool IsSystemFile(const WIN32_FIND_DATA& find_data) {
  return (find_data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;
}

bool IsValidDirectory(const WIN32_FIND_DATA& find_data) {
  return wcscmp(find_data.cFileName, L".") != 0 &&
         wcscmp(find_data.cFileName, L"..") != 0;
}

////////////////////////////////////////////////////////////////////////////////

bool FileExists(const std::wstring& file) {
  if (file.empty())
    return false;

  HANDLE file_handle = OpenFileForGenericRead(file);

  if (file_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(file_handle);
    return true;
  }

  return false;
}

bool FolderExists(const std::wstring& path) {
  win::ErrorMode error_mode(SEM_FAILCRITICALERRORS);

  auto file_attr = GetFileAttributes(GetExtendedLengthPath(path).c_str());

  return (file_attr != INVALID_FILE_ATTRIBUTES) &&
         (file_attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool PathExists(const std::wstring& path) {
  win::ErrorMode error_mode(SEM_FAILCRITICALERRORS);

  auto file_attr = GetFileAttributes(GetExtendedLengthPath(path).c_str());

  return file_attr != INVALID_FILE_ATTRIBUTES;
}

void ValidateFileName(std::wstring& file) {
  EraseChars(file, L"\\/:*?\"<>|");
}

////////////////////////////////////////////////////////////////////////////////

std::wstring ExpandEnvironmentStrings(const std::wstring& path) {
  WCHAR buff[MAX_PATH];

  if (::ExpandEnvironmentStrings(path.c_str(), buff, MAX_PATH))
    return buff;

  return path;
}

std::wstring GetDefaultAppPath(const std::wstring& extension,
                               const std::wstring& default_value) {
  auto query_root_value = [](const std::wstring& subkey) {
    win::Registry reg;
    reg.OpenKey(HKEY_CLASSES_ROOT, subkey, 0, KEY_QUERY_VALUE);
    return reg.QueryValue(L"");
  };

  std::wstring path = query_root_value(extension);

  if (!path.empty())
    path = query_root_value(path + L"\\shell\\open\\command");

  if (!path.empty()) {
    size_t position = 0;
    bool inside_quotes = false;
    for ( ; position < path.size(); ++position) {
      if (path.at(position) == ' ') {
        if (!inside_quotes)
          break;
      } else if (path.at(position) == '"') {
        inside_quotes = !inside_quotes;
      }
    }
    if (position != path.size())
      path.resize(position);
    Trim(path, L"\" ");
  }

  return path.empty() ? default_value : path;
}

std::wstring GetKnownFolderPath(REFKNOWNFOLDERID rfid) {
  std::wstring output;

  PWSTR path = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(rfid, KF_FLAG_CREATE, nullptr, &path))) {
    output = path;
  }
  CoTaskMemFree(path);

  return output;
}

////////////////////////////////////////////////////////////////////////////////

unsigned int PopulateFiles(std::vector<std::wstring>& file_list,
                           const std::wstring& path,
                           const std::wstring& extension,
                           bool recursive,
                           bool trim_extension) {
  unsigned int file_count = 0;

  auto OnFile = [&](const std::wstring& root, const std::wstring& name,
                    const WIN32_FIND_DATA& data) {
    if (extension.empty() || IsEqual(GetFileExtension(name), extension)) {
      file_list.push_back(trim_extension ? GetFileWithoutExtension(name) : name);
      file_count++;
    }
    return false;
  };

  FileSearchHelper helper;
  helper.set_log_errors(false);
  helper.set_skip_subdirectories(!recursive);
  helper.Search(path, nullptr, OnFile);

  return file_count;
}

int PopulateFolders(std::vector<std::wstring>& folder_list,
                    const std::wstring& path) {
  unsigned int folder_count = 0;

  auto OnDirectory = [&](const std::wstring& root, const std::wstring& name,
                         const WIN32_FIND_DATA& data) {
    folder_list.push_back(name);
    folder_count++;
    return false;
  };

  FileSearchHelper helper;
  helper.set_log_errors(false);
  helper.set_skip_subdirectories(true);
  helper.Search(path, OnDirectory, nullptr);

  return folder_count;
}

////////////////////////////////////////////////////////////////////////////////

bool ReadFromFile(const std::wstring& path, std::string& output) {
  HANDLE file_handle = OpenFileForGenericRead(path);

  if (file_handle == INVALID_HANDLE_VALUE)
    return false;

  LARGE_INTEGER file_size{};
  if (::GetFileSizeEx(file_handle, &file_size) == FALSE)
    return false;
  output.resize(static_cast<size_t>(file_size.QuadPart));

  DWORD bytes_read = 0;
  const BOOL result = ::ReadFile(file_handle, output.data(),
                                 output.size(), &bytes_read, nullptr);
  ::CloseHandle(file_handle);

  return result != FALSE && bytes_read == output.size();
}

bool SaveToFile(LPCVOID data, DWORD length, const string_t& path,
                bool take_backup) {
  // Make sure the path is available
  CreateFolder(GetPathOnly(path));

  // Take a backup if needed
  if (take_backup) {
    std::wstring new_path = path + L".bak";
    MoveFileEx(path.c_str(), new_path.c_str(),
               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  }

  // Save the data
  BOOL result = FALSE;
  HANDLE file_handle = OpenFileForGenericWrite(path);
  if (file_handle != INVALID_HANDLE_VALUE) {
    DWORD bytes_written = 0;
    result = ::WriteFile(file_handle, data, length, &bytes_written, nullptr);
    ::CloseHandle(file_handle);
  }

  return result != FALSE;
}

bool SaveToFile(const std::string& data, const std::wstring& path,
                bool take_backup) {
  if (data.empty())
    return false;

  return SaveToFile((LPCVOID)&data.front(), data.size(), path, take_backup);
}

////////////////////////////////////////////////////////////////////////////////

UINT64 ParseSizeString(std::wstring value) {
  TrimRight(value, L".\r");
  EraseChars(value, L" ");

  std::wstring unit;
  if (value.length() >= 2) {
    for (auto it = value.rbegin(); it != value.rend(); ++it) {
      if (IsNumericChar(*it))
        break;
      unit.insert(unit.begin(), *it);
    }
    value.resize(value.length() - unit.length());
    Trim(unit);
  }

  UINT64 size = 1;
  if (IsEqual(unit, L"KB")) {
    size = 1000;
  } else if (IsEqual(unit, L"KiB")) {
    size = 1024;
  } else if (IsEqual(unit, L"MB")) {
    size = 1000 * 1000;
  } else if (IsEqual(unit, L"MiB")) {
    size = 1024 * 1024;
  } else if (IsEqual(unit, L"GB")) {
    size = 1000 * 1000 * 1000;
  } else if (IsEqual(unit, L"GiB")) {
    size = 1024 * 1024 * 1024;
  }

  return static_cast<UINT64>(size * ToDouble(value));
}

std::wstring ToSizeString(QWORD qwSize) {
  std::wstring size, unit;

  if (qwSize > 1073741824) {      // 2^30
    size = ToWstr(static_cast<double>(qwSize) / 1073741824, 2);
    unit = L" GB";
  } else if (qwSize > 1048576) {  // 2^20
    size = ToWstr(static_cast<double>(qwSize) / 1048576, 2);
    unit = L" MB";
  } else if (qwSize > 1024) {     // 2^10
    size = ToWstr(static_cast<double>(qwSize) / 1024, 2);
    unit = L" KB";
  } else {
    size = ToWstr(qwSize);
    unit = L" bytes";
  }

  return size + unit;
}