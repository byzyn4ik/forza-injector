// A stand-in for azimuth.exe's import table (tests/saves_tests.py): every
// KERNEL32 file function the saves redirect patches is imported here and
// called through this DLL's own imports, as Azimuth calls them.
// SPDX-License-Identifier: GPL-3.0-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

__declspec(dllexport) DWORD ProbeCall(int operation, const wchar_t *path, const wchar_t *other, wchar_t *output,
                                      DWORD capacity) {
    WIN32_FIND_DATAW find;
    WIN32_FILE_ATTRIBUTE_DATA data;
    HANDLE handle;
    DWORD written = 0;
    switch (operation) {
    case 0: return CreateDirectoryW(path, NULL);
    case 1:  // a new file holding `other`
        handle = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle == INVALID_HANDLE_VALUE) return 0;
        WriteFile(handle, other, (DWORD)(lstrlenW(other) * sizeof(wchar_t)), &written, NULL);
        CloseHandle(handle);
        return 1;
    case 2: return GetFileAttributesW(path);
    case 3: return MoveFileExW(path, other, MOVEFILE_REPLACE_EXISTING);
    case 4: return DeleteFileW(path);
    case 5: return RemoveDirectoryW(path);
    case 6:
        handle = FindFirstFileExW(path, FindExInfoBasic, &find, FindExSearchNameMatch, NULL, 0);
        if (handle == INVALID_HANDLE_VALUE) return 0;
        FindClose(handle);
        return 1;
    case 7: return GetFileAttributesExW(path, GetFileExInfoStandard, &data) ? data.nFileSizeLow + 1 : 0;
    case 8:
        handle = FindFirstFileW(path, &find);
        if (handle == INVALID_HANDLE_VALUE) return 0;
        FindClose(handle);
        return 1;
    case 9:  // the final path Azimuth would see
        handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle == INVALID_HANDLE_VALUE) return 0;
        written = GetFinalPathNameByHandleW(handle, output, capacity, 0);
        CloseHandle(handle);
        return written;
    case 10: {  // rename by handle into `other`
        handle = CreateFileW(path, DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle == INVALID_HANDLE_VALUE) return 0;
        static unsigned char buffer[sizeof(FILE_RENAME_INFO) + 2048 * sizeof(wchar_t)];
        FILE_RENAME_INFO *info = (FILE_RENAME_INFO *)buffer;
        DWORD length = (DWORD)lstrlenW(other);
        ZeroMemory(buffer, sizeof(buffer));
        info->FileNameLength = length * sizeof(wchar_t);
        CopyMemory(info->FileName, other, (length + 1) * sizeof(wchar_t));
        BOOL ok = SetFileInformationByHandle(handle, FileRenameInfo, info,
                                             (DWORD)(sizeof(FILE_RENAME_INFO) + length * sizeof(wchar_t)));
        CloseHandle(handle);
        return ok;
    }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    (void)module;
    (void)reason;
    (void)reserved;
    return TRUE;
}
