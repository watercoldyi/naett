#include "naett_internal.h"

#if __WINDOWS__ || defined(__APPLE__)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if __WINDOWS__
#include <wchar.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

struct DownloadFile {
#if __WINDOWS__
    HANDLE handle;
    wchar_t* destination;
    wchar_t* temporary;
#else
    FILE* stream;
    char* destination;
    char* temporary;
#endif
    int ownsTemporary;
};

static int closeDownloadFile(DownloadFile* file) {
#if __WINDOWS__
    HANDLE handle = file->handle;
    file->handle = NULL;
    return handle == NULL || CloseHandle(handle);
#else
    FILE* stream = file->stream;
    file->stream = NULL;
    return stream == NULL || fclose(stream) == 0;
#endif
}

static void freeDownloadFile(DownloadFile* file) {
    if (file) {
        closeDownloadFile(file);
        if (file->ownsTemporary) {
#if __WINDOWS__
            DeleteFileW(file->temporary);
#else
            unlink(file->temporary);
#endif
        }
        free(file->temporary);
        free(file->destination);
        free(file);
    }
}

#if __WINDOWS__
static int openDownloadFile(DownloadFile* file, const char* path) {
    wchar_t* widePath = winPathFromUTF8(path);
    if (!widePath) {
        return 0;
    }
    DWORD capacity = GetFullPathNameW(widePath, 0, NULL, NULL);
    file->destination = capacity ? malloc((size_t)capacity * sizeof(wchar_t)) : NULL;
    wchar_t* name = NULL;
    DWORD length = file->destination ? GetFullPathNameW(widePath, capacity, file->destination, &name) : 0;
    free(widePath);
    if (!length || length >= capacity || !name || !*name) {
        return 0;
    }
    DWORD attributes = GetFileAttributesW(file->destination);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return 0;
    }

    size_t prefix = (size_t)(name - file->destination);
    enum { suffixCapacity = 32 };
    file->temporary = malloc((prefix + suffixCapacity) * sizeof(wchar_t));
    if (!file->temporary) {
        return 0;
    }
    memcpy(file->temporary, file->destination, prefix * sizeof(wchar_t));
    for (int attempt = 0; attempt < 16; attempt++) {
        unsigned int first, second;
        if (rand_s(&first) || rand_s(&second)) {
            return 0;
        }
        swprintf(file->temporary + prefix, suffixCapacity, L".naett-%08x%08x.tmp", first, second);
        HANDLE handle = CreateFileW(file->temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            file->ownsTemporary = 1;
            file->handle = handle;
            return 1;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) {
            return 0;
        }
    }
    return 0;
}
#else
static int openDownloadFile(DownloadFile* file, const char* path) {
    const char* separator = strrchr(path, '/');
    const char* name = separator ? separator + 1 : path;
    if (!*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    char* directory = separator ? strndup(path, separator == path ? 1 : (size_t)(separator - path)) : strdup(".");
    if (!directory) {
        return 0;
    }
    char* parent = realpath(directory, NULL);
    free(directory);
    if (!parent) {
        return 0;
    }

    const char* join = strcmp(parent, "/") == 0 ? "" : "/";
    size_t prefix = strlen(parent) + strlen(join);
    const char pattern[] = ".naett-XXXXXX";
    size_t destinationSize = prefix + strlen(name) + 1;
    file->destination = malloc(destinationSize);
    file->temporary = malloc(prefix + sizeof(pattern));
    int fd = -1;
    if (file->destination && file->temporary) {
        snprintf(file->destination, destinationSize, "%s%s%s", parent, join, name);
        struct stat info;
        if (stat(file->destination, &info) != 0 || !S_ISDIR(info.st_mode)) {
            snprintf(file->temporary, prefix + sizeof(pattern), "%s%s%s", parent, join, pattern);
            fd = mkstemp(file->temporary);
        }
    }
    free(parent);
    if (fd < 0) {
        return 0;
    }
    file->ownsTemporary = 1;
    file->stream = fdopen(fd, "wb");
    if (!file->stream) {
        close(fd);
        return 0;
    }
    return 1;
}
#endif

static DownloadFile* createDownloadFile(const char* path) {
    DownloadFile* file = calloc(1, sizeof(*file));
    if (file && (!path || !*path || !openDownloadFile(file, path))) {
        freeDownloadFile(file);
        return NULL;
    }
    return file;
}

static int writeDownloadFile(DownloadFile* file, const void* data, int bytes) {
#if __WINDOWS__
    DWORD written;
    return WriteFile(file->handle, data, (DWORD)bytes, &written, NULL) ? (int)written : -1;
#else
    return (int)fwrite(data, 1, (size_t)bytes, file->stream);
#endif
}

static int commitDownloadFile(DownloadFile* file) {
    if (!file->ownsTemporary) {
        return 1;
    }
#if __WINDOWS__
    if (!file->handle || !closeDownloadFile(file)) {
        return 0;
    }
    int moved = MoveFileExW(file->temporary, file->destination, MOVEFILE_REPLACE_EXISTING);
#else
    if (!file->stream || !closeDownloadFile(file)) {
        return 0;
    }
    int moved = rename(file->temporary, file->destination) == 0;
#endif
    if (moved) {
        file->ownsTemporary = 0;
    }
    return moved;
}

#endif
