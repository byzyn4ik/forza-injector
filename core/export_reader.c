#include "host.h"
#include "support.h"

static int read_at(const unsigned char *data, LONGLONG size, LONGLONG offset, void *out, DWORD length) {
    if (offset < 0 || offset > size || (LONGLONG)length > size - offset) return 0;
    memory_copy(out, data + offset, length);
    return 1;
}

BOOL forza_file_exports(const wchar_t *path, const char *name) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    LARGE_INTEGER size;
    BOOL found = FALSE;
    unsigned char *data = NULL;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0x200 || size.QuadPart > 64 * 1024 * 1024) goto done;
    data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart);
    DWORD got = 0;
    if (!data || !ReadFile(file, data, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart) goto done;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!read_at(data, size.QuadPart, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        !read_at(data, size.QuadPart, dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        goto done;
    IMAGE_DATA_DIRECTORY directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress) goto done;
    LONGLONG sections = dos.e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    // RVA -> file offset through the section table.
#define FILE_OFFSET(rva, out)                                                                                  \
    do {                                                                                                       \
        (out) = -1;                                                                                            \
        for (WORD s_ = 0; s_ < nt.FileHeader.NumberOfSections; ++s_) {                                         \
            IMAGE_SECTION_HEADER h_;                                                                           \
            if (!read_at(data, size.QuadPart, sections + s_ * (LONGLONG)sizeof(h_), &h_, sizeof(h_))) break;   \
            if ((rva) >= h_.VirtualAddress && (rva) < h_.VirtualAddress + h_.SizeOfRawData) {                  \
                (out) = (LONGLONG)h_.PointerToRawData + (rva) - h_.VirtualAddress;                             \
                break;                                                                                         \
            }                                                                                                  \
        }                                                                                                      \
    } while (0)
    LONGLONG at;
    FILE_OFFSET(directory.VirtualAddress, at);
    IMAGE_EXPORT_DIRECTORY exports;
    if (at < 0 || !read_at(data, size.QuadPart, at, &exports, sizeof(exports))) goto done;
    LONGLONG names;
    FILE_OFFSET(exports.AddressOfNames, names);
    for (DWORD i = 0; names >= 0 && i < exports.NumberOfNames && i < 65536; ++i) {
        DWORD rva;
        LONGLONG text;
        if (!read_at(data, size.QuadPart, names + 4 * (LONGLONG)i, &rva, 4)) break;
        FILE_OFFSET(rva, text);
        if (text < 0) continue;
        LONGLONG k = 0;
        while (name[k] && text + k < size.QuadPart && data[text + k] == (unsigned char)name[k]) ++k;
        if (!name[k] && text + k < size.QuadPart && !data[text + k]) {
            found = TRUE;
            break;
        }
    }
#undef FILE_OFFSET
done:
    if (data) HeapFree(GetProcessHeap(), 0, data);
    CloseHandle(file);
    return found;
}

