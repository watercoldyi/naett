#include "naett_internal.h"

#if __WINDOWS__ || defined(__APPLE__)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if __WINDOWS__
#include <share.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

typedef struct {
    FILE* file;
    char* data;
    uint64_t size;
    int source;
    int unknownSize;
} UploadSegment;

struct UploadBody {
    UploadSegment* segments;
#if __APPLE__
    char* filePath;
#endif
    size_t count;
    size_t index;
    uint64_t offset;
    uint64_t size;
    int hasSource;
    int unknownSize;
};

#if __WINDOWS__
static wchar_t* winPathFromUTF8(const char* path) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (length == 0) {
        return NULL;
    }
    wchar_t* widePath = malloc((size_t)length * sizeof(wchar_t));
    if (!widePath) {
        return NULL;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, widePath, length)) {
        free(widePath);
        return NULL;
    }
    return widePath;
}
#endif

static FILE* openUploadFile(const char* path, uint64_t* size) {
#if __WINDOWS__
    wchar_t* widePath = winPathFromUTF8(path);
    FILE* file = widePath ? _wfsopen(widePath, L"rb", _SH_DENYWR) : NULL;
    free(widePath);
    struct _stat64 info;
    int valid = file && _fstat64(_fileno(file), &info) == 0 && (info.st_mode & _S_IFMT) == _S_IFREG;
#else
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd == -1) {
        return NULL;
    }
    FILE* file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        return NULL;
    }
    struct stat info;
    int valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
#endif
    if (!valid || info.st_size < 0) {
        if (file) {
            fclose(file);
        }
        return NULL;
    }
    *size = (uint64_t)info.st_size;
    return file;
}

static void freeUpload(UploadBody* upload) {
    if (upload) {
        for (size_t i = 0; i < upload->count; i++) {
            UploadSegment* segment = &upload->segments[i];
            if (segment->file) {
                fclose(segment->file);
            }
            free(segment->data);
        }
        free(upload->segments);
#if __APPLE__
        free(upload->filePath);
#endif
        free(upload);
    }
}

static int uploadSegment(UploadBody* upload, FILE* file, char* data, uint64_t size) {
    upload->segments[upload->count++] = (UploadSegment){ file, data, size, 0, 0 };
    if (size > INT64_MAX - upload->size) {
        return naettGenericError;
    }
    upload->size += size;
    return 0;
}

static int uploadSource(UploadBody* upload, int index, int64_t size) {
    int unknown = size == NAETT_UNKNOWN_SOURCE_LENGTH;
    if (size < 0 && !unknown) {
        return naettProtocolError;
    }
    int error = uploadSegment(upload, NULL, NULL, unknown ? 0 : (uint64_t)size);
    UploadSegment* segment = &upload->segments[upload->count - 1];
    segment->source = index;
    segment->unknownSize = unknown;
    upload->hasSource = 1;
    upload->unknownSize |= unknown;
    return error;
}

static int uploadFile(UploadBody* upload, const char* path) {
    uint64_t size;
    FILE* file = openUploadFile(path, &size);
    return file ? uploadSegment(upload, file, NULL, size) : naettReadError;
}

static int uploadMemory(UploadBody* upload, const char* data, size_t size) {
    char* copy = size ? malloc(size) : NULL;
    if (size && !copy) {
        return naettGenericError;
    }
    if (size) {
        memcpy(copy, data, size);
    }
    return uploadSegment(upload, NULL, copy, size);
}

static int validPartHeader(const char* value) {
    if (!value || !*value) {
        return 0;
    }
    for (const unsigned char* p = (const unsigned char*)value; *p; p++) {
        if (*p < 32 || *p == 127) {
            return 0;
        }
    }
    return 1;
}

static char* escapePartParameter(const char* value) {
    size_t length = strlen(value);
    if (length > (SIZE_MAX - 1) / 2) {
        return NULL;
    }
    char* result = malloc(length * 2 + 1);
    if (result) {
        char* out = result;
        for (; *value; value++) {
            if (*value == '\\' || *value == '"') {
                *out++ = '\\';
            }
            *out++ = *value;
        }
        *out = 0;
    }
    return result;
}

static int uploadPart(UploadBody* upload, const char* boundary, const naettMultipartPart* part,
    int index, int64_t sourceLength) {
    int sourcePart = sourceLength >= 0 || sourceLength == NAETT_UNKNOWN_SOURCE_LENGTH;
    int filePart = part->path != NULL || sourcePart;
    const char* contentType = part->contentType ? part->contentType : "application/octet-stream";
    if ((!sourcePart && sourceLength != -1) || !validPartHeader(part->name) ||
        (part->path && (!*part->path || sourcePart)) || (filePart && (part->value ||
        !validPartHeader(part->filename) || !validPartHeader(contentType))) ||
        (!filePart && (!part->value || part->valueSize < 0 || part->filename || part->contentType))) {
        return naettProtocolError;
    }

    char* name = escapePartParameter(part->name);
    char* filename = filePart ? escapePartParameter(part->filename) : NULL;
    const char* format = filePart
        ? "--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\nContent-Type: %s\r\n\r\n"
        : "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n";
    int length = name && (!filePart || filename) ? snprintf(NULL, 0, format, boundary, name, filename, contentType) : -1;
    char* header = length >= 0 ? malloc((size_t)length + 1) : NULL;
    if (header) {
        snprintf(header, (size_t)length + 1, format, boundary, name, filename, contentType);
    }
    free(filename);
    free(name);
    if (!header) {
        return naettGenericError;
    }

    int error = uploadSegment(upload, NULL, header, (size_t)length);
    if (!error) {
        if (sourcePart) {
            error = uploadSource(upload, index, sourceLength);
        } else {
            error = part->path ? uploadFile(upload, part->path) : uploadMemory(upload, part->value, part->valueSize);
        }
    }
    return error ? error : uploadMemory(upload, "\r\n", 2);
}

static int prepareUpload(InternalRequest* req, const char* bodyFile, int count,
    const naettMultipartPart* parts, const int64_t* sourceLengths, char* contentType, size_t contentTypeSize) {
    int rawSource = sourceLengths && count == 0 && !parts;
    if ((bodyFile && (!*bodyFile || count != 0 || parts || sourceLengths)) ||
        (!bodyFile && !rawSource && (count <= 0 || !parts)) ||
        req->options.bodyReader || req->options.body.data || req->options.body.size != 0) {
        return naettProtocolError;
    }
    for (KVLink* header = req->options.headers; header; header = header->next) {
        if (strcasecmp(header->key, "Content-Length") == 0 || strcasecmp(header->key, "Transfer-Encoding") == 0 ||
            (parts && strcasecmp(header->key, "Content-Type") == 0)) {
            return naettProtocolError;
        }
    }
    if ((size_t)count > (SIZE_MAX / sizeof(UploadSegment) - 1) / 3) {
        return naettGenericError;
    }
    UploadBody* upload = calloc(1, sizeof(*upload));
    if (!upload) {
        return naettGenericError;
    }
    req->upload = upload;
    upload->segments = calloc((size_t)count * 3 + 1, sizeof(*upload->segments));
    if (!upload->segments) {
        return naettGenericError;
    }
    if (bodyFile) {
#if __APPLE__
        upload->filePath = strdup(bodyFile);
        if (!upload->filePath) {
            return naettGenericError;
        }
#endif
        return uploadFile(upload, bodyFile);
    }
    if (rawSource) {
        return uploadSource(upload, 1, sourceLengths[0]);
    }

    unsigned int random[4];
#if __WINDOWS__
    for (int i = 0; i < 4; i++) {
        if (rand_s(&random[i]) != 0) {
            return naettGenericError;
        }
    }
#else
    arc4random_buf(random, sizeof(random));
#endif
    char boundary[40];
    snprintf(boundary, sizeof(boundary), "naett-%08x%08x%08x%08x", random[0], random[1], random[2], random[3]);
    for (int i = 0; i < count; i++) {
        int error = uploadPart(upload, boundary, &parts[i], i + 1, sourceLengths ? sourceLengths[i] : -1);
        if (error) {
            return error;
        }
    }
    char end[sizeof(boundary) + 6];
    int length = snprintf(end, sizeof(end), "--%s--\r\n", boundary);
    snprintf(contentType, contentTypeSize, "multipart/form-data; boundary=%s", boundary);
    return uploadMemory(upload, end, (size_t)length);
}

static int resetUpload(InternalResponse* res) {
    UploadBody* upload = res->request->upload;
    for (size_t i = 0; i < upload->count; i++) {
        FILE* file = upload->segments[i].file;
        if (file) {
            clearerr(file);
#if __WINDOWS__
            if (_fseeki64(file, 0, SEEK_SET) != 0) {
#else
            if (fseeko(file, 0, SEEK_SET) != 0) {
#endif
                return 0;
            }
        }
    }
    upload->index = 0;
    upload->offset = 0;
    changeFeed(res->feed, FeedRestart);
    return 1;
}

static int readUpload(InternalResponse* res, char* buffer, int capacity) {
    UploadBody* upload = res->request->upload;
    int total = 0;
    while (total < capacity && upload->index < upload->count) {
        UploadSegment* segment = &upload->segments[upload->index];
        int length = capacity - total;
        if (!segment->unknownSize && segment->size - upload->offset < (uint64_t)length) {
            length = (int)(segment->size - upload->offset);
        }
        if (length > 0) {
            if (segment->source) {
                length = readFeed(res->feed, segment->source, buffer + total, length);
                if (length < 0 || (length == 0 && !segment->unknownSize)) {
                    return -1;
                }
            } else if (segment->file) {
                if (fread(buffer + total, 1, (size_t)length, segment->file) != (size_t)length) {
                    return -1;
                }
            } else {
                memcpy(buffer + total, segment->data + (size_t)upload->offset, (size_t)length);
            }
            if (!segment->unknownSize) {
                upload->offset += (uint64_t)length;
            }
            total += length;
            if (segment->unknownSize && length > 0) {
                break;
            }
        }
        if (segment->unknownSize ? length == 0 : upload->offset == segment->size) {
            upload->index++;
            upload->offset = 0;
        }
    }
    if (upload->index == upload->count) {
        finishFeed(res->feed);
    }
    return total;
}

#endif
