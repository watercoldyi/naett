// Inlined amalgam.h: //
#include "naett.h"
// Inlined naett_sink.c: //
// Inlined naett_internal.h: //
#ifndef NAETT_INTERNAL_H
#define NAETT_INTERNAL_H

#include <stdint.h>

#ifdef _MSC_VER
    #define strcasecmp _stricmp
    #undef strdup
    #define strdup _strdup
#endif

#ifdef _WIN32
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#ifndef min
    #define min(x,y) ((x) < (y) ? (x) : (y))
#endif
#define __WINDOWS__ 1
#endif

#if __linux__ && !__ANDROID__
#define __LINUX__ 1
#include <curl/curl.h>
#endif

#if __ANDROID__
#include <jni.h>
#include <pthread.h>
#endif

#ifdef __APPLE__
#include <pthread.h>
#include <stdatomic.h>
#include "TargetConditionals.h"
#include <objc/objc.h>
#include <CoreFoundation/CFStream.h>
#if TARGET_OS_IPHONE
#define __IOS__ 1
#else
#define __MACOS__ 1
#endif
#endif

#define naettAlloc(TYPE, VAR) TYPE* VAR = (TYPE*)calloc(1, sizeof(TYPE))

typedef struct KVLink {
    const char* key;
    const char* value;
    struct KVLink* next;
} KVLink;

typedef struct Buffer {
    void* data;
    int size;
    int capacity;
    int position;
} Buffer;

typedef struct {
    const char* method;
    const char* userAgent;
    int timeoutMS;
    naettReadFunc bodyReader;
    void* bodyReaderData;
    naettWriteFunc bodyWriter;
    void* bodyWriterData;
    KVLink* headers;
    Buffer body;
} RequestOptions;

typedef struct UploadBody UploadBody;
typedef struct DownloadSink DownloadSink;
typedef struct DownloadFile DownloadFile;
typedef struct UploadFeed UploadFeed;

typedef struct {
    RequestOptions options;
    const char* url;
    UploadBody* upload;
#if __APPLE__
    id urlRequest;
    id uploadFileURL;
#endif
#if __ANDROID__
    jobject urlObject;
#endif
#if __WINDOWS__
    HINTERNET session;
    HINTERNET connection;
    HINTERNET request;
    LPWSTR host;
    LPWSTR resource;
    int secure;
#endif
} InternalRequest;

typedef struct {
    InternalRequest* request;
    DownloadSink* sink;
    DownloadFile* downloadFile;
    UploadFeed* feed;
    int cancellable;
    int code;
#if __WINDOWS__
    volatile LONG complete;
#elif defined(__APPLE__)
    atomic_int complete;
#else
    int complete;
#endif
    KVLink* headers;
    Buffer body;
#if __WINDOWS__
    DECLSPEC_ALIGN(8) volatile LONG64 contentLength;
    DECLSPEC_ALIGN(8) volatile LONG64 downloadLength;
    DECLSPEC_ALIGN(8) volatile LONG64 totalBytesRead;
#elif defined(__APPLE__)
    atomic_llong contentLength;
    atomic_llong downloadLength;
    atomic_ullong totalBytesRead;
#else
    int contentLength;  // 0 if headers not read, -1 if Content-Length missing.
    uint64_t totalBytesRead;
#endif
#if __APPLE__
    id session;
    id task;
    CFWriteStreamRef uploadStream;
    pthread_t uploadWorker;
    atomic_int uploadStop;
    atomic_int cancelRequested;
    atomic_ullong uploaded;
    int uploadWorkerActive;
    int uploadReadError;
#endif
#if __ANDROID__
    pthread_t workerThread;
    int closeRequested;
#endif
#if __LINUX__
    struct curl_slist* headerList;
#endif
#if __WINDOWS__
    char buffer[10240];
    size_t bytesLeft;
    DWORD uploadSize;
    DWORD uploadOffset;
    DWORD uploadStart;
    DWORD uploadPayloadStart;
    DWORD uploadPayloadEnd;
    int uploadEnded;
    unsigned int uploadRetries;
    DECLSPEC_ALIGN(8) volatile LONG64 uploaded;
    volatile LONG streamClosing;
    HANDLE requestStarter;
    volatile LONG starterDone;
    volatile LONG cancelRequested;
#endif
} InternalResponse;

void naettPlatformInit(naettInitData initData);
int naettPlatformInitRequest(InternalRequest* req);
void naettPlatformMakeRequest(InternalResponse* res);
void naettPlatformFreeRequest(InternalRequest* req);
void naettPlatformCloseResponse(InternalResponse* res);
#if __WINDOWS__ || defined(__APPLE__)
void naettPlatformCancel(InternalResponse* res);
#endif

#endif  // NAETT_INTERNAL_H
// End of inlined naett_internal.h //


#if __WINDOWS__ || defined(__APPLE__)

#include <stdlib.h>
#include <string.h>
#if __APPLE__
#include <time.h>
#endif

#if __WINDOWS__
typedef CRITICAL_SECTION SinkMutex;
typedef CONDITION_VARIABLE SinkCondition;
#define sinkLock EnterCriticalSection
#define sinkUnlock LeaveCriticalSection
#define sinkSignal WakeAllConditionVariable
#else
typedef pthread_mutex_t SinkMutex;
typedef pthread_cond_t SinkCondition;
#define sinkLock pthread_mutex_lock
#define sinkUnlock pthread_mutex_unlock
#define sinkSignal pthread_cond_broadcast
#endif

static void waitForStream(SinkCondition* condition, SinkMutex* mutex, int timeoutMS) {
#if __WINDOWS__
    SleepConditionVariableCS(condition, mutex, timeoutMS < 0 ? INFINITE : (DWORD)timeoutMS);
#else
    if (timeoutMS < 0) {
        pthread_cond_wait(condition, mutex);
        return;
    }
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) == 0) {
        deadline.tv_sec += timeoutMS / 1000;
        deadline.tv_nsec += (timeoutMS % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(condition, mutex, &deadline);
    }
#endif
}

#if __WINDOWS__
static SRWLOCK activityMutex = SRWLOCK_INIT;
static CONDITION_VARIABLE activityChanged = CONDITION_VARIABLE_INIT;
#define activityLock() AcquireSRWLockExclusive(&activityMutex)
#define activityUnlock() ReleaseSRWLockExclusive(&activityMutex)
#else
static pthread_mutex_t activityMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t activityChanged = PTHREAD_COND_INITIALIZER;
#define activityLock() pthread_mutex_lock(&activityMutex)
#define activityUnlock() pthread_mutex_unlock(&activityMutex)
#endif
static uint64_t activityVersion;

static void signalActivity(void) {
    activityLock();
    activityVersion++;
    sinkSignal(&activityChanged);
    activityUnlock();
}

static uint64_t getActivityVersion(void) {
    activityLock();
    uint64_t version = activityVersion;
    activityUnlock();
    return version;
}

static void waitForActivity(uint64_t version, int timeoutMS) {
    activityLock();
    if (version == activityVersion && timeoutMS > 0) {
#if __WINDOWS__
        SleepConditionVariableSRW(&activityChanged, &activityMutex, (DWORD)timeoutMS, 0);
#else
        waitForStream(&activityChanged, &activityMutex, timeoutMS);
#endif
    }
    activityUnlock();
}

struct DownloadSink {
    SinkMutex mutex;
    SinkCondition readable;
    SinkCondition writable;
    unsigned char buffer[65536];
    int offset;
    int size;
    int finished;
    int stopped;
};

static DownloadSink* createSink(void) {
    DownloadSink* sink = calloc(1, sizeof(*sink));
    if (!sink) {
        return NULL;
    }
#if __WINDOWS__
    InitializeCriticalSection(&sink->mutex);
    InitializeConditionVariable(&sink->readable);
    InitializeConditionVariable(&sink->writable);
#else
    if (pthread_mutex_init(&sink->mutex, NULL) != 0) {
        free(sink);
        return NULL;
    }
    if (pthread_cond_init(&sink->readable, NULL) != 0) {
        pthread_mutex_destroy(&sink->mutex);
        free(sink);
        return NULL;
    }
    if (pthread_cond_init(&sink->writable, NULL) != 0) {
        pthread_cond_destroy(&sink->readable);
        pthread_mutex_destroy(&sink->mutex);
        free(sink);
        return NULL;
    }
#endif
    return sink;
}

static void freeSink(DownloadSink* sink) {
    if (sink) {
#if __WINDOWS__
        DeleteCriticalSection(&sink->mutex);
#else
        pthread_cond_destroy(&sink->writable);
        pthread_cond_destroy(&sink->readable);
        pthread_mutex_destroy(&sink->mutex);
#endif
        free(sink);
    }
}

static void finishSink(DownloadSink* sink) {
    sinkLock(&sink->mutex);
    sink->finished = 1;
    sinkSignal(&sink->readable);
    sinkUnlock(&sink->mutex);
}

static void stopSink(DownloadSink* sink) {
    sinkLock(&sink->mutex);
    sink->stopped = 1;
    sink->size = 0;
    sinkSignal(&sink->writable);
    sinkSignal(&sink->readable);
    sinkUnlock(&sink->mutex);
}

static int sinkStopped(DownloadSink* sink) {
    sinkLock(&sink->mutex);
    int stopped = sink->stopped;
    sinkUnlock(&sink->mutex);
    return stopped;
}

static int writeSink(DownloadSink* sink, const void* source, int bytes) {
    const unsigned char* data = source;
    int copied = 0;
    sinkLock(&sink->mutex);
    while (copied < bytes) {
        while (sink->size != 0 && !sink->stopped) {
            waitForStream(&sink->writable, &sink->mutex, -1);
        }
        if (sink->stopped) {
            sinkUnlock(&sink->mutex);
            return -1;
        }
        int count = bytes - copied;
        if (count > (int)sizeof(sink->buffer)) {
            count = (int)sizeof(sink->buffer);
        }
        memcpy(sink->buffer, data + copied, (size_t)count);
        copied += count;
        sink->offset = 0;
        sink->size = count;
        sinkSignal(&sink->readable);
        signalActivity();
    }
    sinkUnlock(&sink->mutex);
    return copied;
}

static int readSink(DownloadSink* sink, void* dest, int capacity, int timeoutMS) {
    sinkLock(&sink->mutex);
    if (sink->size == 0 && !sink->finished && !sink->stopped && timeoutMS > 0) {
        waitForStream(&sink->readable, &sink->mutex, timeoutMS);
    }
    int count = capacity < sink->size ? capacity : sink->size;
    if (count > 0) {
        memcpy(dest, sink->buffer + sink->offset, (size_t)count);
        sink->offset += count;
        sink->size -= count;
        if (sink->size == 0) {
            sinkSignal(&sink->writable);
        }
    }
    sinkUnlock(&sink->mutex);
    return count;
}

struct UploadFeed {
    SinkMutex mutex;
    SinkCondition changed;
    uint64_t attempt;
    void* buffer;
    int capacity;
    int index;
    int supplied;
    int finished;
    int stopped;
};

static UploadFeed* createFeed(void) {
    UploadFeed* feed = calloc(1, sizeof(*feed));
    if (!feed) {
        return NULL;
    }
#if __WINDOWS__
    InitializeCriticalSection(&feed->mutex);
    InitializeConditionVariable(&feed->changed);
#else
    if (pthread_mutex_init(&feed->mutex, NULL) != 0) {
        free(feed);
        return NULL;
    }
    if (pthread_cond_init(&feed->changed, NULL) != 0) {
        pthread_mutex_destroy(&feed->mutex);
        free(feed);
        return NULL;
    }
#endif
    return feed;
}

static void freeFeed(UploadFeed* feed) {
    if (feed) {
#if __WINDOWS__
        DeleteCriticalSection(&feed->mutex);
#else
        pthread_cond_destroy(&feed->changed);
        pthread_mutex_destroy(&feed->mutex);
#endif
        free(feed);
    }
}

typedef enum { FeedRestart, FeedInterrupt, FeedCancel } FeedChange;

static void changeFeed(UploadFeed* feed, FeedChange change) {
    if (feed) {
        sinkLock(&feed->mutex);
        feed->attempt++;
        feed->buffer = NULL;
        feed->finished = change != FeedRestart;
        feed->stopped |= change == FeedCancel;
        sinkSignal(&feed->changed);
        sinkUnlock(&feed->mutex);
    }
}

static void finishFeed(UploadFeed* feed) {
    if (feed) {
        sinkLock(&feed->mutex);
        feed->finished = 1;
        sinkSignal(&feed->changed);
        sinkUnlock(&feed->mutex);
    }
}

static int readFeed(UploadFeed* feed, int index, void* buffer, int capacity) {
    sinkLock(&feed->mutex);
    int count = -1;
    if (!feed->stopped && !feed->finished) {
        uint64_t attempt = feed->attempt;
        feed->buffer = buffer;
        feed->capacity = capacity;
        feed->index = index;
        sinkSignal(&feed->changed);
        signalActivity();
        while (feed->buffer && feed->attempt == attempt) {
            waitForStream(&feed->changed, &feed->mutex, -1);
        }
        if (feed->attempt == attempt) {
            count = feed->supplied;
        }
    }
    sinkUnlock(&feed->mutex);
    return count;
}

static int requestFeed(UploadFeed* feed, int* capacity, uint64_t* attempt, int timeoutMS) {
    sinkLock(&feed->mutex);
    if (!feed->buffer && !feed->finished && !feed->stopped && timeoutMS > 0) {
        waitForStream(&feed->changed, &feed->mutex, timeoutMS);
    }
    int index = feed->finished || feed->stopped ? -1 : 0;
    if (feed->buffer) {
        index = feed->index;
        *capacity = feed->capacity;
        *attempt = feed->attempt;
    }
    sinkUnlock(&feed->mutex);
    return index;
}

static int supplyFeed(UploadFeed* feed, uint64_t attempt, const void* data, int size) {
    sinkLock(&feed->mutex);
    int accepted = feed->buffer && attempt == feed->attempt && size >= 0 && size <= feed->capacity &&
        (size == 0 || data);
    if (accepted) {
        if (size > 0) {
            memcpy(feed->buffer, data, (size_t)size);
        }
        feed->supplied = size;
        feed->buffer = NULL;
        sinkSignal(&feed->changed);
    }
    sinkUnlock(&feed->mutex);
    return accepted;
}

#endif
// End of inlined naett_sink.c //

// Inlined naett_upload.c: //
//#include "naett_internal.h"

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
// End of inlined naett_upload.c //

// Inlined naett_download_file.c: //
//#include "naett_internal.h"

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
// End of inlined naett_download_file.c //

// Inlined naett_core.c: //
//#include "naett_internal.h"
#include <stdarg.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>

typedef struct InternalParam* InternalParamPtr;
typedef void (*ParamSetter)(InternalParamPtr param, InternalRequest* req);

typedef struct InternalParam {
        ParamSetter setter;
        int offset;
        union {
            int integer;
            const char* string;
            struct {
                const char* key;
                const char* value;
            } kv;
            void* ptr;
            void (*func)(void);
        };
} InternalParam;

typedef struct InternalOption {
    #define maxParams 2
    int numParams;
    InternalParam params[maxParams];
} InternalOption;

static void stringSetter(InternalParamPtr param, InternalRequest* req) {
    char* stringCopy = strdup(param->string);
    char* opaque = (char*)&req->options;
    char** stringField = (char**)(opaque + param->offset);
    if (*stringField) {
        free(*stringField);
    }
    *stringField = stringCopy;
}

static void intSetter(InternalParamPtr param, InternalRequest* req) {
    char* opaque = (char*)&req->options;
    int* intField = (int*)(opaque + param->offset);
    *intField = param->integer;
}

static void ptrSetter(InternalParamPtr param, InternalRequest* req) {
    char* opaque = (char*)&req->options;
    void** ptrField = (void**)(opaque + param->offset);
    *ptrField = param->ptr;
}

static void kvSetter(InternalParamPtr param, InternalRequest* req) {
    char* opaque = (char*)&req->options;
    KVLink** kvField = (KVLink**)(opaque + param->offset);

    naettAlloc(KVLink, newNode);
    newNode->key = strdup(param->kv.key);
    newNode->value = strdup(param->kv.value);
    newNode->next = *kvField;

    *kvField = newNode;
}

static int defaultBodyReader(void* dest, int bufferSize, void* userData) {
    Buffer* buffer = (Buffer*) userData;

    if (dest == NULL) {
        return buffer->size;
    }

    int bytesToRead = buffer->size - buffer->position;
    if (bytesToRead > bufferSize) {
        bytesToRead = bufferSize;
    }
    if (bytesToRead == 0) {
        return 0;
    }

    const char* source = ((const char*)buffer->data) + buffer->position;
    memcpy(dest, source, bytesToRead);
    buffer->position += bytesToRead;
    return bytesToRead;
}

static int defaultBodyWriter(const void* source, int bytes, void* userData) {
    Buffer* buffer = (Buffer*) userData;
    int newCapacity = buffer->capacity;
    if (newCapacity == 0) {
        newCapacity = bytes;
    }
    while (newCapacity - buffer->size < bytes) {
        newCapacity *= 2;
    }
    if (newCapacity != buffer->capacity) {
        buffer->data = realloc(buffer->data, newCapacity);
        buffer->capacity = newCapacity;
    }
    char* dest = ((char*)buffer->data) + buffer->size;
    memcpy(dest, source, bytes);
    buffer->size += bytes;
    return bytes;
}

static int initialized = 0;

static void initRequest(InternalRequest* req, const char* url) {
    assert(initialized);
    req->options.method = strdup("GET");
    req->options.timeoutMS = 5000;
    req->url = strdup(url);
}

static void applyOptionParams(InternalRequest* req, InternalOption* option) {
    for (int j = 0; j < option->numParams; j++) {
        InternalParam* param = option->params + j;
        param->setter(param, req);
    }
}

// Public API

void naettInit(naettInitData initData) {
    assert(!initialized);
    naettPlatformInit(initData);
    initialized = 1;
}

naettOption* naettMethod(const char* method) {
    naettAlloc(InternalOption, option);
    option->numParams = 1;
    InternalParam* param = &option->params[0];

    param->string = method;
    param->offset = offsetof(RequestOptions, method);
    param->setter = stringSetter;

    return (naettOption*)option;
}

naettOption* naettHeader(const char* name, const char* value) {
    naettAlloc(InternalOption, option);
    option->numParams = 1;
    InternalParam* param = &option->params[0];

    param->kv.key = name;
    param->kv.value = value;
    param->offset = offsetof(RequestOptions, headers);
    param->setter = kvSetter;

    return (naettOption*)option;
}

naettOption* naettUserAgent(const char* userAgent) {
    naettAlloc(InternalOption, option);
    option->numParams = 1;
    InternalParam* param = &option->params[0];

    param->string = userAgent;
    param->offset = offsetof(RequestOptions, userAgent);
    param->setter = stringSetter;

    return (naettOption*)option;
}

naettOption* naettTimeout(int timeoutMS) {
    naettAlloc(InternalOption, option);
    option->numParams = 1;
    InternalParam* param = &option->params[0];

    param->integer = timeoutMS;
    param->offset = offsetof(RequestOptions, timeoutMS);
    param->setter = intSetter;

    return (naettOption*)option;
}

naettOption* naettBody(const char* body, int size) {
    naettAlloc(InternalOption, option);
    option->numParams = 2;

    InternalParam* bodyParam = &option->params[0];
    InternalParam* sizeParam = &option->params[1];

    bodyParam->ptr = (void*)body;
    bodyParam->offset = offsetof(RequestOptions, body) + offsetof(Buffer, data);
    bodyParam->setter = ptrSetter;

    sizeParam->integer = size;
    sizeParam->offset = offsetof(RequestOptions, body) + offsetof(Buffer, size);
    sizeParam->setter = intSetter;

    return (naettOption*)option;
}

naettOption* naettBodyReader(naettReadFunc reader, void* userData) {
    naettAlloc(InternalOption, option);
    option->numParams = 2;

    InternalParam* readerParam = &option->params[0];
    InternalParam* dataParam = &option->params[1];

    readerParam->func = (void (*)(void)) reader;
    readerParam->offset = offsetof(RequestOptions, bodyReader);
    readerParam->setter = ptrSetter;

    dataParam->ptr = userData;
    dataParam->offset = offsetof(RequestOptions, bodyReaderData);
    dataParam->setter = ptrSetter;

    return (naettOption*)option;
}

naettOption* naettBodyWriter(naettWriteFunc writer, void* userData) {
    naettAlloc(InternalOption, option);
    option->numParams = 2;

    InternalParam* writerParam = &option->params[0];
    InternalParam* dataParam = &option->params[1];

    writerParam->func = (void(*)(void)) writer;
    writerParam->offset = offsetof(RequestOptions, bodyWriter);
    writerParam->setter = ptrSetter;

    dataParam->ptr = userData;
    dataParam->offset = offsetof(RequestOptions, bodyWriterData);
    dataParam->setter = ptrSetter;

    return (naettOption*)option;
}

void setupDefaultRW(InternalRequest* req) {
    if (req->options.bodyReader == NULL) {
        req->options.bodyReader = defaultBodyReader;
        req->options.bodyReaderData = (void*) &req->options.body;
    }
    if (req->options.bodyReader == defaultBodyReader) {
        req->options.body.position = 0;
    }
    if (req->options.bodyWriter == NULL) {
        req->options.bodyWriter = defaultBodyWriter;
    }
}

naettReq* naettRequest_va(const char* url, int numArgs, ...) {
    assert(url != NULL);

    va_list args;
    InternalOption* option;
    naettAlloc(InternalRequest, req);
    initRequest(req, url);

    va_start(args, numArgs);
    for (int i = 0; i < numArgs; i++) {
        option = va_arg(args, InternalOption*);
        applyOptionParams(req, option);
        free(option);
    }
    va_end(args);

    setupDefaultRW(req);

    if (naettPlatformInitRequest(req)) {
        return (naettReq*)req;
    }

    naettFree((naettReq*) req);
    return NULL;
}

static InternalRequest* requestWithOptions(const char* url, int numOptions, const naettOption** options) {
    assert(url != NULL);
    assert(numOptions == 0 || options != NULL);

    naettAlloc(InternalRequest, req);
    initRequest(req, url);

    for (int i = 0; i < numOptions; i++) {
        InternalOption* option = (InternalOption*)options[i];
        applyOptionParams(req, option);
        free(option);
    }
    return req;
}

naettReq* naettRequestWithOptions(const char* url, int numOptions, const naettOption** options) {
    InternalRequest* req = requestWithOptions(url, numOptions, options);
    setupDefaultRW(req);

    if (naettPlatformInitRequest(req)) {
        return (naettReq*)req;
    }

    naettFree((naettReq*) req);
    return NULL;
}

static naettReq* requestWithUpload(const char* url, int numOptions, const naettOption** options,
    const char* bodyFile, int numParts, const naettMultipartPart* parts, const int64_t* sourceLengths, int* error) {
    InternalRequest* req = requestWithOptions(url, numOptions, options);
    int status = naettProtocolError;
#if __WINDOWS__ || defined(__APPLE__)
    char contentType[80];
    status = prepareUpload(req, bodyFile, numParts, parts, sourceLengths, contentType, sizeof(contentType));
    if (!status) {
        if (parts) {
            InternalOption* header = (InternalOption*)naettHeader("Content-Type", contentType);
            applyOptionParams(req, header);
            free(header);
        }
        setupDefaultRW(req);
        if (naettPlatformInitRequest(req)) {
            if (error) {
                *error = 0;
            }
            return (naettReq*)req;
        }
        status = naettGenericError;
    }
#endif
    naettFree((naettReq*)req);
    if (error) {
        *error = status;
    }
    return NULL;
}

naettReq* naettRequestWithUpload(const char* url, int numOptions, const naettOption** options,
    const char* bodyFile, int numParts, const naettMultipartPart* parts, int* error) {
    return requestWithUpload(url, numOptions, options, bodyFile, numParts, parts, NULL, error);
}

naettReq* naettRequestWithSources(const char* url, int numOptions, const naettOption** options,
    int numParts, const naettMultipartPart* parts, const int64_t* sourceLengths, int* error) {
    return requestWithUpload(url, numOptions, options, NULL, numParts, parts, sourceLengths, error);
}

static naettRes* makeResponse(naettReq* request, DownloadSink* sink, DownloadFile* file, int cancellable) {
    assert(initialized);
    assert(request != NULL);

    InternalRequest* req = (InternalRequest*)request;
    naettAlloc(InternalResponse, res);
    if (!res) {
        return NULL;
    }
    res->request = req;
    res->sink = sink;
    res->downloadFile = file;
    res->cancellable = cancellable;
#if __WINDOWS__ || defined(__APPLE__)
    if (req->upload && req->upload->hasSource) {
        res->feed = createFeed();
        if (!res->feed) {
            free(res);
            return NULL;
        }
        res->cancellable = 1;
    }
#endif
#if __WINDOWS__
    res->downloadLength = -1;
#elif defined(__APPLE__)
    atomic_init(&res->complete, 0);
    atomic_init(&res->contentLength, 0);
    atomic_init(&res->downloadLength, -1);
    atomic_init(&res->totalBytesRead, 0);
    atomic_init(&res->uploadStop, 0);
    atomic_init(&res->cancelRequested, 0);
    atomic_init(&res->uploaded, 0);
#endif

    if (req->options.bodyWriter == defaultBodyWriter) {
        req->options.bodyWriterData = (void*) &res->body;
    }

    naettPlatformMakeRequest(res);
    return (naettRes*) res;
}

naettRes* naettMake(naettReq* request) {
    return makeResponse(request, NULL, NULL, 0);
}

naettRes* naettMakeCancellable(naettReq* request) {
    return makeResponse(request, NULL, NULL, 1);
}

naettRes* naettMakeWithSink(naettReq* request) {
#if __WINDOWS__ || defined(__APPLE__)
    DownloadSink* sink = createSink();
    if (sink) {
        naettRes* response = makeResponse(request, sink, NULL, 1);
        if (response) {
            return response;
        }
        freeSink(sink);
    }
#endif
    return NULL;
}

naettRes* naettMakeWithResponseFile(naettReq* request, const char* path, int* error) {
    int status = naettGenericError;
#if __WINDOWS__ || defined(__APPLE__)
    DownloadFile* file = createDownloadFile(path);
    if (file) {
        naettRes* response = makeResponse(request, NULL, file, 1);
        if (response) {
            if (error) {
                *error = 0;
            }
            return response;
        }
        freeDownloadFile(file);
    } else {
        status = naettWriteError;
    }
#endif
    if (error) {
        *error = status;
    }
    return NULL;
}

int naettCommitResponseFile(naettRes* response) {
#if __WINDOWS__ || defined(__APPLE__)
    InternalResponse* res = (InternalResponse*)response;
    if (res && res->downloadFile && naettComplete(response) && res->code >= 200 && res->code < 300) {
        return commitDownloadFile(res->downloadFile) ? 0 : naettWriteError;
    }
#endif
    return naettProtocolError;
}

int naettReadSink(naettRes* response, void* buffer, int capacity, int timeoutMS) {
#if __WINDOWS__ || defined(__APPLE__)
    assert(response != NULL && buffer != NULL && capacity > 0);
    InternalResponse* res = (InternalResponse*)response;
    assert(res->sink != NULL);
    return readSink(res->sink, buffer, capacity, timeoutMS);
#else
    return 0;
#endif
}

#if __WINDOWS__ || defined(__APPLE__)
static void cancelResponseStreams(naettRes* response) {
    assert(response != NULL);
    InternalResponse* res = (InternalResponse*)response;
    if (res->sink) {
        stopSink(res->sink);
    }
    changeFeed(res->feed, FeedCancel);
    if (!naettComplete(response)) {
        naettPlatformCancel(res);
    }
}
#endif

void naettCancel(naettRes* response) {
#if __WINDOWS__ || defined(__APPLE__)
    InternalResponse* res = (InternalResponse*)response;
    if (res && res->cancellable) {
        cancelResponseStreams(response);
    }
#endif
}

void naettStopSink(naettRes* response) {
    naettCancel(response);
}

uint64_t naettActivityVersion(void) {
#if __WINDOWS__ || defined(__APPLE__)
    return getActivityVersion();
#else
    return 0;
#endif
}

void naettWaitForActivity(uint64_t version, int timeoutMS) {
#if __WINDOWS__ || defined(__APPLE__)
    waitForActivity(version, timeoutMS);
#endif
}

int naettGetUploadSourceRequest(naettRes* response, int* capacity, uint64_t* attempt, int timeoutMS) {
#if __WINDOWS__ || defined(__APPLE__)
    InternalResponse* res = (InternalResponse*)response;
    if (res && res->feed && capacity && attempt) {
        return requestFeed(res->feed, capacity, attempt, timeoutMS);
    }
#endif
    return -1;
}

int naettSupplyUploadSource(naettRes* response, uint64_t attempt, const void* data, int size) {
#if __WINDOWS__ || defined(__APPLE__)
    InternalResponse* res = (InternalResponse*)response;
    if (res && res->feed) {
        return supplyFeed(res->feed, attempt, data, size);
    }
#endif
    return 0;
}

void naettStopUploadSource(naettRes* response) {
    naettCancel(response);
}

const void* naettGetBody(naettRes* response, int* size) {
    assert(response != NULL);
    assert(size != NULL);

    InternalResponse* res = (InternalResponse*)response;
    *size = res->body.size;
    return res->body.data;
}

static int64_t responseLength(InternalResponse* res) {
#if __WINDOWS__
    return InterlockedCompareExchange64(&res->contentLength, 0, 0);
#elif defined(__APPLE__)
    return atomic_load_explicit(&res->contentLength, memory_order_relaxed);
#else
    return res->contentLength;
#endif
}

static uint64_t responseBytesRead(InternalResponse* res) {
#if __WINDOWS__
    return (uint64_t)InterlockedCompareExchange64(&res->totalBytesRead, 0, 0);
#elif defined(__APPLE__)
    return atomic_load_explicit(&res->totalBytesRead, memory_order_acquire);
#else
    return res->totalBytesRead;
#endif
}

int naettGetTotalBytesRead(naettRes* response, int* totalSize) {
    assert(response != NULL);
    assert(totalSize != NULL);
    InternalResponse* res = (InternalResponse*)response;
    int64_t length = responseLength(res);
    uint64_t read = responseBytesRead(res);
    *totalSize = length > INT_MAX ? -1 : (int)length;
    return read > INT_MAX ? INT_MAX : (int)read;
}

#if __WINDOWS__ || defined(__APPLE__)
static int64_t downloadLength(InternalResponse* res) {
#if __WINDOWS__
    return InterlockedCompareExchange64(&res->downloadLength, 0, 0);
#else
    return atomic_load_explicit(&res->downloadLength, memory_order_acquire);
#endif
}

static int64_t expectedDownloadLength(InternalResponse* res, int64_t headerLength) {
    int status = res->code;
    const char* method = res->request->options.method;
    if (status == naettProcessing) {
        return -1;
    }
    if (status < 200 || status == 204 || status == 205 || status == 304 || strcmp(method, "HEAD") == 0 ||
        (status < 300 && strcmp(method, "CONNECT") == 0)) {
        return 0;
    }
    naettRes* response = (naettRes*)res;
    if (naettGetHeader(response, "Transfer-Encoding")) {
        return -1;
    }
#if __APPLE__
    const char* encoding = naettGetHeader(response, "Content-Encoding");
    if (encoding && strcasecmp(encoding, "identity") != 0) {
        return -1;
    }
#endif
    return headerLength;
}

static void finishResponse(InternalResponse* res) {
    int notify = res->cancellable;
    changeFeed(res->feed, FeedInterrupt);
    if (res->sink || res->downloadFile) {
        int64_t expected = downloadLength(res);
        if (res->code >= 200 && expected >= 0 && responseBytesRead(res) != (uint64_t)expected) {
            res->code = naettReadError;
        }
        if (res->sink) {
            finishSink(res->sink);
        }
    }
#if __WINDOWS__
    InterlockedExchange(&res->complete, 1);
#else
    atomic_store_explicit(&res->complete, 1, memory_order_release);
#endif
    if (notify) {
        signalActivity();
    }
}

static int writeResponseBody(InternalResponse* res, const void* bytes, int length) {
    if (res->sink) {
        return writeSink(res->sink, bytes, length);
    }
    if (res->downloadFile) {
        return res->code >= 200 && res->code < 300 ? writeDownloadFile(res->downloadFile, bytes, length) : length;
    }
    RequestOptions* options = &res->request->options;
    return options->bodyWriter(bytes, length, options->bodyWriterData);
}

static void readResponseLength(InternalResponse* res) {
    const char* header = naettGetHeader((naettRes*)res, "Content-Length");
    int64_t length = -1;
    if (header) {
        char* end;
        errno = 0;
        long long value = strtoll(header, &end, 10);
        if (end != header && errno != ERANGE && value >= 0 && end[strspn(end, " \t")] == '\0') {
            length = value;
        }
    }
    int64_t expected = expectedDownloadLength(res, length);
#if __WINDOWS__
    InterlockedExchange64(&res->contentLength, length);
    InterlockedExchange64(&res->downloadLength, expected);
#else
    atomic_store_explicit(&res->contentLength, length, memory_order_relaxed);
    atomic_store_explicit(&res->downloadLength, expected, memory_order_release);
#endif
    if (res->cancellable) {
        signalActivity();
    }
}

static void recordResponseBytes(InternalResponse* res, int bytes) {
#if __WINDOWS__
    InterlockedExchangeAdd64(&res->totalBytesRead, bytes);
#else
    atomic_fetch_add_explicit(&res->totalBytesRead, (uint64_t)bytes, memory_order_release);
#endif
    if (res->cancellable) {
        signalActivity();
    }
}
#endif

const char* naettGetHeader(naettRes* response, const char* name) {
    assert(response != NULL);
    assert(name != NULL);

    InternalResponse* res = (InternalResponse*)response;
    KVLink* node = res->headers;
    while (node) {
        if (strcasecmp(name, node->key) == 0) {
            return node->value;
        }
        node = node->next;
    }
    return NULL;
}

void naettListHeaders(naettRes* response, naettHeaderLister lister, void* userData) {
    assert(response != NULL);
    assert(lister != NULL);

    InternalResponse* res = (InternalResponse*)response;
    KVLink* node = res->headers;
    while (node) {
        if (!lister(node->key, node->value, userData)) {
            return;
        }
        node = node->next;
    }
}

naettReq* naettGetRequest(naettRes* response) {
    assert(response != NULL);
    InternalResponse* res = (InternalResponse*)response;
    return (naettReq*) res->request;
}

int naettComplete(const naettRes* response) {
    assert(response != NULL);
    InternalResponse* res = (InternalResponse*)response;
#if __WINDOWS__
    return InterlockedCompareExchange(&res->complete, 0, 0);
#elif defined(__APPLE__)
    return atomic_load_explicit(&res->complete, memory_order_acquire);
#else
    return res->complete;
#endif
}

int naettGetStatus(const naettRes* response) {
    assert(response != NULL);
    InternalResponse* res = (InternalResponse*)response;
    return res->code;
}

uint64_t naettGetUploadProgress(const naettRes* response, uint64_t* totalSize) {
    assert(response != NULL);
    assert(totalSize != NULL);
    *totalSize = 0;
#if __WINDOWS__ || defined(__APPLE__)
    InternalResponse* res = (InternalResponse*)response;
    if (res->request->upload) {
        UploadBody* upload = res->request->upload;
        *totalSize = upload->unknownSize ? UINT64_MAX : upload->size;
#if __WINDOWS__
        return (uint64_t)InterlockedCompareExchange64(&res->uploaded, 0, 0);
#else
        return atomic_load_explicit(&res->uploaded, memory_order_relaxed);
#endif
    }
#endif
    return 0;
}

uint64_t naettGetDownloadProgress(const naettRes* response, int64_t* totalSize) {
    assert(response != NULL && totalSize != NULL);
    InternalResponse* res = (InternalResponse*)response;
    uint64_t received = responseBytesRead(res);
#if __WINDOWS__ || defined(__APPLE__)
    *totalSize = downloadLength(res);
#else
    *totalSize = -1;
#endif
    return received;
}

static void freeKVList(KVLink* node) {
    while (node != NULL) {
        free((void*) node->key);
        free((void*) node->value);
        KVLink* next = node->next;
        free(node);
        node = next;
    }
}

void naettFree(naettReq* request) {
    assert(request != NULL);

    InternalRequest* req = (InternalRequest*)request;
    naettPlatformFreeRequest(req);
#if __WINDOWS__ || defined(__APPLE__)
    freeUpload(req->upload);
#endif
    KVLink* node = req->options.headers;
    freeKVList(node);
    free((void*)req->options.method);
    free((void*)req->options.userAgent);
    free((void*)req->url);
    free(request);
}

void naettClose(naettRes* response) {
    assert(response != NULL);

    InternalResponse* res = (InternalResponse*)response;
    naettPlatformCloseResponse(res);
    res->request = NULL;
    KVLink* node = res->headers;
    freeKVList(node);
    free(res->body.data);
#if __WINDOWS__ || defined(__APPLE__)
    freeSink(res->sink);
    freeDownloadFile(res->downloadFile);
    freeFeed(res->feed);
#endif
    free(response);
}
// End of inlined naett_core.c //

// Inlined naett_osx.c: //
//#include "naett_internal.h"

#ifdef __APPLE__

// Inlined naett_objc.h: //
#ifndef NAETT_OBJC_H
#define NAETT_OBJC_H

#if defined(__IOS__) || defined (__MACOS__)
#include <assert.h>
#include <math.h>

#include <objc/NSObjCRuntime.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

#if defined(__OBJC__) && __has_feature(objc_arc)
#error "ARC is not supported"
#endif

// ABI is a bit different between platforms
#ifdef __arm64__
#define abi_objc_msgSend_stret objc_msgSend
#else
#define abi_objc_msgSend_stret objc_msgSend_stret
#endif
#ifdef __i386__
#define abi_objc_msgSend_fpret objc_msgSend_fpret
#else
#define abi_objc_msgSend_fpret objc_msgSend
#endif

#define objc_msgSendSuper_t(RET, ...) ((RET(*)(struct objc_super*, SEL, ##__VA_ARGS__))objc_msgSendSuper)
#define objc_msgSend_t(RET, ...) ((RET(*)(id, SEL, ##__VA_ARGS__))objc_msgSend)
#define objc_msgSend_stret_t(RET, ...) ((RET(*)(id, SEL, ##__VA_ARGS__))abi_objc_msgSend_stret)
#define objc_msgSend_id objc_msgSend_t(id)
#define objc_msgSend_void objc_msgSend_t(void)
#define objc_msgSend_void_id objc_msgSend_t(void, id)
#define objc_msgSend_void_bool objc_msgSend_t(void, bool)

#define sel(NAME) sel_registerName(NAME)
#define class(NAME) ((id)objc_getClass(NAME))
#define makeClass(NAME, SUPER) \
    objc_allocateClassPair((Class)objc_getClass(SUPER), NAME, 0)

// Check here to get the signature right:
// https://nshipster.com/type-encodings/
// https://ko9.org/posts/encode-types/
#define addMethod(CLASS, NAME, IMPL, SIGNATURE) \
    if (!class_addMethod(CLASS, sel(NAME), (IMP) (IMPL), (SIGNATURE))) assert(false)

#define addIvar(CLASS, NAME, SIZE, SIGNATURE) \
    if (!class_addIvar(CLASS, NAME, SIZE, rint(log2(SIZE)), SIGNATURE)) assert(false)

#define objc_alloc(CLASS) objc_msgSend_id(class(CLASS), sel("alloc"))
#define autorelease(OBJ) objc_msgSend_void(OBJ, sel("autorelease"))
#define retain(OBJ) objc_msgSend_void(OBJ, sel("retain"))
#define release(OBJ) objc_msgSend_void(OBJ, sel("release"))

#if __LP64__ || NS_BUILD_32_LIKE_64
#define NSIntegerEncoding "q"
#define NSUIntegerEncoding "L"
#else
#define NSIntegerEncoding "i"
#define NSUIntegerEncoding "I"
#endif

#endif // defined(__IOS__) || defined (__MACOS__)
#endif // NAETT_OBJC_H
// End of inlined naett_objc.h //

#include <CoreFoundation/CFRunLoop.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef DEBUG
static void _showPools(const char* context) {
    fprintf(stderr, "NSAutoreleasePool@%s:\n", context);
    objc_msgSend_void(class("NSAutoreleasePool"), sel("showPools"));
}
#define showPools(x) _showPools((x))
#else
#define showPools(x)
#endif

static id pool(void) {
    return objc_msgSend_id(objc_alloc("NSAutoreleasePool"), sel("init"));
}

static id sessionConfiguration = nil;

void naettPlatformInit(naettInitData initData) {
    id NSThread = class("NSThread");
    SEL isMultiThreaded = sel("isMultiThreaded");

    if (!objc_msgSend_t(bool)(NSThread, isMultiThreaded)) {
        // Make a dummy call from a new thread to kick Cocoa into multi-threaded mode
        objc_msgSend_t(void, SEL, id, id)(
            NSThread, sel("detachNewThreadSelector:toTarget:withObject:"), isMultiThreaded, NSThread, nil);
    }

    sessionConfiguration = objc_msgSend_id(class("NSURLSessionConfiguration"), sel("ephemeralSessionConfiguration"));
    retain(sessionConfiguration);
}

id NSString(const char* string) {
    return objc_msgSend_t(id, const char*)(class("NSString"), sel("stringWithUTF8String:"), string);
}

int naettPlatformInitRequest(InternalRequest* req) {
    id p = pool();

    id urlString = NSString(req->url);
    id url = urlString == nil ? nil : objc_msgSend_t(id, id)(class("NSURL"), sel("URLWithString:"), urlString);
    if (url == nil) {
        release(p);
        return 0;
    }

    id request = objc_msgSend_t(id, id)(class("NSMutableURLRequest"), sel("requestWithURL:"), url);

    objc_msgSend_t(void, double)(request, sel("setTimeoutInterval:"), (double)(req->options.timeoutMS) / 1000.0);
    id methodString = NSString(req->options.method);
    objc_msgSend_t(void, id)(request, sel("setHTTPMethod:"), methodString);

    {
        id name = NSString("User-Agent");
        id value = NSString(req->options.userAgent ? req->options.userAgent : NAETT_UA);
        objc_msgSend_t(void, id, id)(request, sel("setValue:forHTTPHeaderField:"), value, name);
    }

    KVLink* header = req->options.headers;
    while (header != NULL) {
        id name = NSString(header->key);
        id value = NSString(header->value);
        objc_msgSend_t(void, id, id)(request, sel("setValue:forHTTPHeaderField:"), value, name);
        header = header->next;
    }

    char byteBuffer[10240];
    int bytesRead = 0;

    if (req->upload) {
        if (req->upload->filePath) {
            id path = NSString(req->upload->filePath);
            if (path == nil) {
                release(p);
                return 0;
            }
            req->uploadFileURL = objc_msgSend_t(id, id)(class("NSURL"), sel("fileURLWithPath:"), path);
            if (req->uploadFileURL == nil) {
                release(p);
                return 0;
            }
            retain(req->uploadFileURL);
        }
    } else if (req->options.bodyReader != NULL) {
        id bodyData =
            objc_msgSend_t(id, NSUInteger)(class("NSMutableData"), sel("dataWithCapacity:"), sizeof(byteBuffer));

        int totalBytesRead = 0;
        do {
            bytesRead = req->options.bodyReader(byteBuffer, sizeof(byteBuffer), req->options.bodyReaderData);
            totalBytesRead += bytesRead;
            objc_msgSend_t(void, const void*, NSUInteger)(bodyData, sel("appendBytes:length:"), byteBuffer, bytesRead);
        } while (bytesRead > 0);

        if (totalBytesRead > 0) {
            objc_msgSend_t(void, id)(request, sel("setHTTPBody:"), bodyData);
        }
    }

    retain(request);
    req->urlRequest = request;

    release(p);
    return 1;
}

static void readResponseHeaders(InternalResponse* res, id response) {
    res->code = objc_msgSend_t(NSInteger)(response, sel("statusCode"));
    id allHeaders = objc_msgSend_t(id)(response, sel("allHeaderFields"));

    NSUInteger headerCount = objc_msgSend_t(NSUInteger)(allHeaders, sel("count"));
    if (headerCount > 0) {
        id headerNames[headerCount];
        id headerValues[headerCount];

        objc_msgSend_t(NSInteger, id*, id*, NSUInteger)(
            allHeaders, sel("getObjects:andKeys:count:"), headerValues, headerNames, headerCount);
        KVLink* firstHeader = NULL;
        for (int i = 0; i < headerCount; i++) {
            naettAlloc(KVLink, node);
            node->key = strdup(objc_msgSend_t(const char*)(headerNames[i], sel("UTF8String")));
            node->value = strdup(objc_msgSend_t(const char*)(headerValues[i], sel("UTF8String")));
            node->next = firstHeader;
            firstHeader = node;
        }
        res->headers = firstHeader;
    }

    readResponseLength(res);
}

static void didReceiveResponse(id self, SEL _sel, id session, id dataTask, id response, void (^reply)(NSInteger)) {
    InternalResponse* res = NULL;
    object_getInstanceVariable(self, "response", (void**)&res);
    if (res && res->code == naettProcessing) {
        id p = pool();
        readResponseHeaders(res, response);
        release(p);
    }
    reply(1); // NSURLSessionResponseAllow
}

void didReceiveData(id self, SEL _sel, id session, id dataTask, id data) {
    InternalResponse* res = NULL;
    id p = pool();

    object_getInstanceVariable(self, "response", (void**)&res);

    if (res->code == naettProcessing) {
        readResponseHeaders(res, objc_msgSend_id(dataTask, sel("response")));
    }

    const unsigned char* bytes = objc_msgSend_t(const unsigned char*)(data, sel("bytes"));
    NSUInteger length = objc_msgSend_t(NSUInteger)(data, sel("length"));

    while (length > 0) {
        int count = length > INT_MAX ? INT_MAX : (int)length;
        if (writeResponseBody(res, bytes, count) != count && (res->sink || res->downloadFile)) {
            res->code = naettWriteError;
            objc_msgSend_void(dataTask, sel("cancel"));
            break;
        }
        recordResponseBytes(res, count);
        bytes += count;
        length -= (NSUInteger)count;
    }

    release(p);
}

typedef struct {
    InternalResponse* response;
    char buffer[16384];
    int size;
    int offset;
    int done;
} UploadPump;

static void pumpUploadStream(CFWriteStreamRef stream, CFStreamEventType event, void* context) {
    UploadPump* pump = context;
    InternalResponse* res = pump->response;
    if (event != kCFStreamEventCanAcceptBytes) {
        pump->done = 1;
        return;
    }
    while (!pump->done && !atomic_load_explicit(&res->uploadStop, memory_order_relaxed) &&
        CFWriteStreamCanAcceptBytes(stream)) {
        if (pump->offset == pump->size) {
            pump->size = readUpload(res, pump->buffer, sizeof(pump->buffer));
            if (pump->size <= 0) {
                res->uploadReadError = pump->size < 0 &&
                    !atomic_load_explicit(&res->uploadStop, memory_order_relaxed);
                pump->done = 1;
                break;
            }
            pump->offset = 0;
        }
        CFIndex written = CFWriteStreamWrite(stream, (const UInt8*)pump->buffer + pump->offset,
            pump->size - pump->offset);
        if (written <= 0) {
            pump->done = 1;
            break;
        }
        pump->offset += (int)written;
    }
}

static void* writeUploadStream(void* context) {
    InternalResponse* res = context;
    CFWriteStreamRef stream = res->uploadStream;
    UploadPump pump = { .response = res };
    CFStreamClientContext client = { .info = &pump };
    CFOptionFlags events = kCFStreamEventCanAcceptBytes | kCFStreamEventErrorOccurred | kCFStreamEventEndEncountered;
    if (CFWriteStreamSetClient(stream, events, pumpUploadStream, &client)) {
        CFRunLoopRef runLoop = CFRunLoopGetCurrent();
        CFWriteStreamScheduleWithRunLoop(stream, runLoop, kCFRunLoopDefaultMode);
        if (CFWriteStreamOpen(stream)) {
            while (!pump.done && !atomic_load_explicit(&res->uploadStop, memory_order_relaxed)) {
                if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true) == kCFRunLoopRunFinished) {
                    break;
                }
            }
        }
        CFWriteStreamUnscheduleFromRunLoop(stream, runLoop, kCFRunLoopDefaultMode);
        CFWriteStreamSetClient(stream, 0, NULL, NULL);
    }
    CFWriteStreamClose(stream);
    return NULL;
}

static void stopUploadStream(InternalResponse* res) {
    if (res->uploadWorkerActive) {
        atomic_store_explicit(&res->uploadStop, 1, memory_order_relaxed);
        changeFeed(res->feed, FeedInterrupt);
        pthread_join(res->uploadWorker, NULL);
        res->uploadWorkerActive = 0;
        CFRelease(res->uploadStream);
        res->uploadStream = NULL;
    }
}

static void needNewBodyStream(id self, SEL _sel, id session, id task, void (^reply)(id)) {
    InternalResponse* res = NULL;
    object_getInstanceVariable(self, "response", (void**)&res);
    if (!res || !res->request->upload) {
        reply(nil);
        return;
    }

    stopUploadStream(res);
    if (res->uploadReadError || !resetUpload(res)) {
        res->uploadReadError = 1;
        reply(nil);
        return;
    }
    atomic_store_explicit(&res->uploaded, 0, memory_order_relaxed);

    CFReadStreamRef input = NULL;
    CFWriteStreamRef output = NULL;
    CFStreamCreateBoundPair(NULL, &input, &output, 16384);
    if (input && output) {
        res->uploadStream = output;
        atomic_store_explicit(&res->uploadStop, 0, memory_order_relaxed);
        if (pthread_create(&res->uploadWorker, NULL, writeUploadStream, res) == 0) {
            res->uploadWorkerActive = 1;
            reply((id)input);
            CFRelease(input);
            return;
        }
        res->uploadStream = NULL;
    }
    if (input) {
        CFRelease(input);
    }
    if (output) {
        CFRelease(output);
    }
    res->code = naettGenericError;
    reply(nil);
}

static void didSendBodyData(id self, SEL _sel, id session, id task, int64_t bytesSent,
    int64_t totalBytesSent, int64_t totalBytesExpected) {
    InternalResponse* res = NULL;
    object_getInstanceVariable(self, "response", (void**)&res);
    if (res && res->request->upload && totalBytesSent >= 0) {
        uint64_t sent = (uint64_t)totalBytesSent;
        UploadBody* upload = res->request->upload;
        atomic_store_explicit(&res->uploaded, upload->unknownSize || sent < upload->size ? sent : upload->size,
            memory_order_relaxed);
        if (res->cancellable) {
            signalActivity();
        }
    }
}

static void didComplete(id self, SEL _sel, id session, id dataTask, id error) {
    InternalResponse* res = NULL;
    object_getInstanceVariable(self, "response", (void**)&res);
    if (res != NULL) {
        stopUploadStream(res);
        if (res->sink && sinkStopped(res->sink)) {
            res->code = naettWriteError;
        } else if (atomic_load_explicit(&res->cancelRequested, memory_order_acquire)) {
            res->code = naettCancelled;
        } else if (res->uploadReadError) {
            res->code = naettReadError;
        } else if (error != nil && res->code >= 0) {
            res->code = naettConnectionError;
        } else if (res->code == naettProcessing) {
            id p = pool();
            readResponseHeaders(res, objc_msgSend_id(dataTask, sel("response")));
            release(p);
        }
        finishResponse(res);
    }
}

static id createDelegate(void) {
    static Class TaskDelegateClass = nil;

    if (!TaskDelegateClass) {
        TaskDelegateClass = objc_allocateClassPair((Class)objc_getClass("NSObject"), "naettTaskDelegate", 0);
        class_addProtocol(TaskDelegateClass, (Protocol* _Nonnull)objc_getProtocol("NSURLSessionDataDelegate"));

        addMethod(TaskDelegateClass, "URLSession:dataTask:didReceiveResponse:completionHandler:",
            didReceiveResponse, "v@:@@@@?");
        addMethod(TaskDelegateClass, "URLSession:dataTask:didReceiveData:", didReceiveData, "v@:@@@");
        addMethod(TaskDelegateClass, "URLSession:task:didCompleteWithError:", didComplete, "v@:@@@");
        addMethod(TaskDelegateClass, "URLSession:task:needNewBodyStream:", needNewBodyStream, "v@:@@@?");
        addMethod(TaskDelegateClass, "URLSession:task:didSendBodyData:totalBytesSent:totalBytesExpectedToSend:",
            didSendBodyData, "v@:@@qqq");
        addIvar(TaskDelegateClass, "response", sizeof(void*), "^v");
    }

    id delegate = objc_msgSend_id((id)TaskDelegateClass, sel("alloc"));
    delegate = objc_msgSend_id(delegate, sel("init"));

    return delegate;
}

void naettPlatformMakeRequest(InternalResponse* res) {
    InternalRequest* req = res->request;
    id p = pool();

    id delegate = createDelegate();
    // delegate will be retained by session below
    autorelease(delegate);
    object_setInstanceVariable(delegate, "response", (void*)res);

    id session = objc_msgSend_t(id, id, id, id)(class("NSURLSession"),
        sel("sessionWithConfiguration:delegate:delegateQueue:"),
        sessionConfiguration,
        delegate,
        nil);

    retain(session);
    res->session = session;

    id task;
    if (req->uploadFileURL) {
        task = objc_msgSend_t(id, id, id)(session, sel("uploadTaskWithRequest:fromFile:"),
            req->urlRequest, req->uploadFileURL);
    } else {
        SEL taskMethod = req->upload ? sel("uploadTaskWithStreamedRequest:") : sel("dataTaskWithRequest:");
        task = objc_msgSend_t(id, id)(session, taskMethod, req->urlRequest);
    }
    if (task == nil) {
        res->code = naettGenericError;
        release(p);
        finishResponse(res);
        return;
    }
    if (res->cancellable) {
        retain(task);
        res->task = task;
    }
    objc_msgSend_void(task, sel("resume"));

    release(p);
}

void naettPlatformFreeRequest(InternalRequest* req) {
    release(req->urlRequest);
    req->urlRequest = nil;
    release(req->uploadFileURL);
    req->uploadFileURL = nil;
}

void naettPlatformCloseResponse(InternalResponse* res) {
    objc_msgSend_void(res->session, sel("invalidateAndCancel"));
    release(res->task);
    release(res->session);
    res->session = nil;
}

void naettPlatformCancel(InternalResponse* res) {
    atomic_store_explicit(&res->cancelRequested, 1, memory_order_release);
    objc_msgSend_void(res->task, sel("cancel"));
}

#endif  // __APPLE__
// End of inlined naett_osx.c //

// Inlined naett_linux.c: //
//#include "naett_internal.h"

#if __linux__ && !__ANDROID__

#include <curl/curl.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

static pthread_t workerThread;
static int handleReadFD = 0;
static int handleWriteFD = 0;

static void panic(const char* message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static void* curlWorker(void* data) {
    CURLM* mc = (CURLM*)data;
    int activeHandles = 0;
    int messagesLeft = 1;

    struct curl_waitfd readFd = { handleReadFD, CURL_WAIT_POLLIN };

    union {
        CURL* handle;
        char buf[sizeof(CURL*)];
    } newHandle;

    int newHandlePos = 0;

    while (1) {
        int status = curl_multi_perform(mc, &activeHandles);
        if (status != CURLM_OK) {
            panic("CURL processing failure");
        }

        struct CURLMsg* message = curl_multi_info_read(mc, &messagesLeft);
        if (message && message->msg == CURLMSG_DONE) {
            CURL* handle = message->easy_handle;
            InternalResponse* res = NULL;
            curl_easy_getinfo(handle, CURLINFO_PRIVATE, (char**)&res);
            curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &res->code);
            res->complete = 1;
            curl_easy_cleanup(handle);
        }


        int readyFDs = 0;
        curl_multi_wait(mc, &readFd, 1, 1, &readyFDs);

        if (readyFDs == 0 && activeHandles == 0 && messagesLeft == 0) {
            usleep(100 * 1000);
        }

        int bytesRead = read(handleReadFD, newHandle.buf, sizeof(newHandle.buf) - newHandlePos);
        if (bytesRead > 0) {
            newHandlePos += bytesRead;
        }
        if (newHandlePos == sizeof(newHandle.buf)) {
            curl_multi_add_handle(mc, newHandle.handle);
            newHandlePos = 0;
        }
    }

    return NULL;
}

void naettPlatformInit(naettInitData initData) {
    curl_global_init(CURL_GLOBAL_ALL);
    CURLM* mc = curl_multi_init();
    int fds[2];
    if (pipe(fds) != 0) {
        panic("Failed to open pipe");
    }
    handleReadFD = fds[0];
    handleWriteFD = fds[1];

    int flags = fcntl(handleReadFD, F_GETFL, 0);
    fcntl(handleReadFD, F_SETFL, flags | O_NONBLOCK);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&workerThread, &attr, curlWorker, mc);
}

int naettPlatformInitRequest(InternalRequest* req) {
    return 1;
}

static size_t readCallback(char* buffer, size_t size, size_t numItems, void* userData) {
    InternalResponse* res = (InternalResponse*)userData;
    InternalRequest* req = res->request;
    return req->options.bodyReader(buffer, size * numItems, req->options.bodyReaderData);
}

static size_t writeCallback(char* ptr, size_t size, size_t numItems, void* userData) {
    InternalResponse* res = (InternalResponse*)userData;
    InternalRequest* req = res->request;
    size_t bytesWritten = req->options.bodyWriter(ptr, size * numItems, req->options.bodyWriterData);
    res->totalBytesRead += bytesWritten;
    return bytesWritten;
}

#define METHOD(A, B, C) (((A) << 16) | ((B) << 8) | (C))

static void setupMethod(CURL* curl, const char* method) {
    if (strlen(method) < 3) {
        return;
    }

    int methodCode = (method[0] << 16) | (method[1] << 8) | method[2];

    switch (methodCode) {
        case METHOD('G', 'E', 'T'):
        case METHOD('C', 'O', 'N'):
        case METHOD('O', 'P', 'T'):
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
            break;
        case METHOD('P', 'O', 'S'):
        case METHOD('P', 'A', 'T'):
        case METHOD('D', 'E', 'L'):
            curl_easy_setopt(curl, CURLOPT_POST, 1);
            break;
        case METHOD('P', 'U', 'T'):
            curl_easy_setopt(curl, CURLOPT_UPLOAD, 1);
            break;
        case METHOD('H', 'E', 'A'):
        case METHOD('T', 'R', 'A'):
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
            break;
    }

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
}

static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userData) {
    InternalResponse* res = (InternalResponse*) userData;
    size_t headerSize = size * nitems;

    char* headerName = strndup(buffer, headerSize);
    char* split = strchr(headerName, ':');
    if (split) {
        *split = 0;
        split++;
        while (*split == ' ') {
            split++;
        }
        char* headerValue = strdup(split);

        char* cr = strchr(headerValue, 13);
        if (cr) {
            *cr = 0;
        }

        char* lf = strchr(headerValue, 10);
        if (lf) {
            *lf = 0;
        }

        naettAlloc(KVLink, node);
        node->next = res->headers;
        node->key = headerName;
        node->value = headerValue;
        res->headers = node;
    }

    return headerSize;
}

void naettPlatformMakeRequest(InternalResponse* res) {
    InternalRequest* req = res->request;

    CURL* c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL, req->url);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, req->options.timeoutMS);

    curl_easy_setopt(c, CURLOPT_READFUNCTION, readCallback);
    curl_easy_setopt(c, CURLOPT_READDATA, res);

    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, res);

    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, res);

    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1);

    int bodySize = res->request->options.bodyReader(NULL, 0, res->request->options.bodyReaderData);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, bodySize);

    setupMethod(c, req->options.method);

    struct curl_slist* headerList = NULL;
    char uaBuf[512];
    snprintf(uaBuf, sizeof(uaBuf), "User-Agent: %s", req->options.userAgent ? req->options.userAgent : NAETT_UA);
    headerList = curl_slist_append(headerList, uaBuf);

    KVLink* header = req->options.headers;
    size_t bufferSize = 0;
    char* buffer = NULL;
    while (header) {
        size_t headerLength = strlen(header->key) + strlen(header->value) + 1 + 1;  // colon + null
        if (headerLength > bufferSize) {
            bufferSize = headerLength;
            buffer = (char*)realloc(buffer, bufferSize);
        }
        snprintf(buffer, bufferSize, "%s:%s", header->key, header->value);
        headerList = curl_slist_append(headerList, buffer);
        header = header->next;
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headerList);
    free(buffer);
    res->headerList = headerList;

    curl_easy_setopt(c, CURLOPT_PRIVATE, res);

    write(handleWriteFD, &c, sizeof(c));
}

void naettPlatformFreeRequest(InternalRequest* req) {
}

void naettPlatformCloseResponse(InternalResponse* res) {
    curl_slist_free_all(res->headerList);
}

#endif
// End of inlined naett_linux.c //

// Inlined naett_win.c: //
//#include "naett_internal.h"

#ifdef __WINDOWS__

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <winhttp.h>
#include <assert.h>
#include <tchar.h>
#include <process.h>

void naettPlatformInit(naettInitData initData) {
}

static char* winToUTF8(LPWSTR source) {
    int length = WideCharToMultiByte(CP_UTF8, 0, source, -1, NULL, 0, NULL, NULL);
    char* chars = (char*)malloc(length);
    int result = WideCharToMultiByte(CP_UTF8, 0, source, -1, chars, length, NULL, NULL);
    if (!result) {
        free(chars);
        return NULL;
    }
    return chars;
}

static LPWSTR winFromUTF8(const char* source) {
    int length = MultiByteToWideChar(CP_UTF8, 0, source, -1, NULL, 0);
    LPWSTR chars = (LPWSTR)malloc(length * sizeof(WCHAR));
    int result = MultiByteToWideChar(CP_UTF8, 0, source, -1, chars, length);
    if (!result) {
        free(chars);
        return NULL;
    }
    return chars;
}

#define ASPRINTF(result, fmt, ...)                        \
    {                                                     \
        size_t len = snprintf(NULL, 0, fmt, __VA_ARGS__); \
        *(result) = (char*)malloc(len + 1);               \
        snprintf(*(result), len + 1, fmt, __VA_ARGS__);   \
    }

static LPWSTR wcsndup(LPCWSTR str, size_t len) {
    LPWSTR result = calloc(1, sizeof(WCHAR) * (len + 1));
    wcsncpy(result, str, len);
    return result;
}

static LPCWSTR packHeaders(InternalRequest* req) {
    char* packed = strdup("");

    KVLink* node = req->options.headers;
    while (node != NULL) {
        char* update;
        ASPRINTF(&update, "%s%s:%s%s", packed, node->key, node->value, node->next ? "\r\n" : "");
        free(packed);
        packed = update;
        node = node->next;
    }

    LPCWSTR winHeaders = winFromUTF8(packed);
    free(packed);
    return winHeaders;
}

static void unpackHeaders(InternalResponse* res, LPWSTR packed) {
    size_t len = 0;
    KVLink* firstHeader = NULL;
    while ((len = wcslen(packed)) != 0) {
        char* header = winToUTF8(packed);
        char* split = strchr(header, ':');
        if (split) {
            *split = 0;
            split++;
            while (*split == ' ') {
                split++;
            }
            naettAlloc(KVLink, node);
            node->key = strdup(header);
            node->value = strdup(split);
            node->next = firstHeader;
            firstHeader = node;
        }
        free(header);
        packed += len + 1;
    }
    res->headers = firstHeader;
}

static void winFinishResponse(InternalResponse* res) {
    if (!res->cancellable) {
        finishResponse(res);
    } else if (InterlockedExchange(&res->streamClosing, 1) == 0) {
        HINTERNET request = res->request->request;
        res->request->request = NULL;
        changeFeed(res->feed, FeedInterrupt);
        // Wait for HANDLE_CLOSING before publishing completion.
        WinHttpCloseHandle(request);
    }
}

static void winSendRequest(InternalResponse* res, HINTERNET request);

static int retryUpload(InternalResponse* res, HINTERNET request, DWORD error) {
    if (error != ERROR_WINHTTP_RESEND_REQUEST || !res->request->upload || res->uploadRetries == 3) {
        return 0;
    }
    res->uploadRetries++;
    winSendRequest(res, request);
    return 1;
}

static int fillWinUpload(InternalResponse* res) {
    res->uploadOffset = 0;
    res->uploadStart = 0;
    res->uploadPayloadStart = 0;
    res->uploadPayloadEnd = 0;
    if (res->uploadEnded) {
        res->uploadSize = 0;
        return 1;
    }

    InternalRequest* req = res->request;
    int chunked = req->upload && req->upload->unknownSize;
    enum { prefixSpace = 16 };
    int prefix = chunked ? prefixSpace : 0;
    int capacity = (int)sizeof(res->buffer) - prefix - (chunked ? 2 : 0);
    int bytes = req->upload ? readUpload(res, res->buffer + prefix, capacity)
        : req->options.bodyReader(res->buffer, capacity, req->options.bodyReaderData);
    if (bytes < 0 || bytes > capacity) {
        return 0;
    }
    res->uploadPayloadEnd = (DWORD)bytes;
    res->uploadSize = (DWORD)bytes;
    if (chunked && bytes > 0) {
        char header[prefixSpace];
        DWORD size = (DWORD)snprintf(header, sizeof(header), "%x\r\n", (unsigned int)bytes);
        res->uploadStart = prefixSpace - size;
        memcpy(res->buffer + res->uploadStart, header, size);
        memcpy(res->buffer + prefixSpace + bytes, "\r\n", 2);
        res->uploadPayloadStart = size;
        res->uploadPayloadEnd += size;
        res->uploadSize += size + 2;
    } else if (chunked) {
        memcpy(res->buffer, "0\r\n\r\n", 5);
        res->uploadSize = 5;
        res->uploadEnded = 1;
    }
    return 1;
}

static void CALLBACK
callback(HINTERNET request, DWORD_PTR context, DWORD status, LPVOID statusInformation, DWORD statusInfoLength) {
    InternalResponse* res = (InternalResponse*)context;
    if (!res) {
        return;
    }
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        if (res->cancellable) {
            if (res->sink && sinkStopped(res->sink)) {
                res->code = naettWriteError;
            } else if (InterlockedCompareExchange(&res->cancelRequested, 0, 0)) {
                res->code = naettCancelled;
            }
            finishResponse(res);
        }
        return;
    }
    if (res->cancellable && InterlockedCompareExchange(&res->streamClosing, 0, 0)) {
        return;
    }

    switch (status) {
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: {
            DWORD bufSize = 0;
            WinHttpQueryHeaders(request,
                WINHTTP_QUERY_RAW_HEADERS,
                WINHTTP_HEADER_NAME_BY_INDEX,
                NULL,
                &bufSize,
                WINHTTP_NO_HEADER_INDEX);
            LPWSTR buffer = bufSize ? malloc(bufSize) : NULL;
            if (!buffer || !WinHttpQueryHeaders(request,
                    WINHTTP_QUERY_RAW_HEADERS, WINHTTP_HEADER_NAME_BY_INDEX, buffer, &bufSize,
                    WINHTTP_NO_HEADER_INDEX)) {
                free(buffer);
                res->code = naettProtocolError;
                winFinishResponse(res);
                break;
            }
            unpackHeaders(res, buffer);
            free(buffer);

            DWORD statusCode = 0;
            DWORD statusCodeSize = sizeof(statusCode);

            WinHttpQueryHeaders(request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &statusCodeSize,
                WINHTTP_NO_HEADER_INDEX);
            res->code = statusCode;
            readResponseLength(res);

            if (!WinHttpQueryDataAvailable(request, NULL)) {
                res->code = naettProtocolError;
                winFinishResponse(res);
            }
        } break;

        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE: {
            DWORD* available = (DWORD*)statusInformation;
            res->bytesLeft = *available;
            if (res->bytesLeft == 0) {
                winFinishResponse(res);
                break;
            }

            size_t bytesToRead = min(res->bytesLeft, sizeof(res->buffer));
            if (!WinHttpReadData(request, res->buffer, (DWORD)bytesToRead, NULL)) {
                res->code = naettReadError;
                winFinishResponse(res);
            }
        } break;

        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: {
            if (statusInfoLength == 0 || statusInfoLength > sizeof(res->buffer) || statusInfoLength > res->bytesLeft) {
                res->code = naettReadError;
                winFinishResponse(res);
                break;
            }
            int bytesRead = (int)statusInfoLength;
            if (writeResponseBody(res, res->buffer, bytesRead) != bytesRead) {
                res->code = res->sink || res->downloadFile ? naettWriteError : naettReadError;
                winFinishResponse(res);
                break;
            }
            recordResponseBytes(res, bytesRead);
            res->bytesLeft -= bytesRead;
            if (res->bytesLeft > 0) {
                size_t bytesToRead = min(res->bytesLeft, sizeof(res->buffer));
                if (!WinHttpReadData(request, res->buffer, (DWORD)bytesToRead, NULL)) {
                    res->code = naettReadError;
                    winFinishResponse(res);
                }
            } else {
                if (!WinHttpQueryDataAvailable(request, NULL)) {
                    res->code = naettProtocolError;
                    winFinishResponse(res);
                }
            }
        } break;

        case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE: {
            if (status == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE) {
                DWORD written = *(DWORD*)statusInformation;
                if (written == 0 || written > res->uploadSize - res->uploadOffset) {
                    res->code = naettWriteError;
                    winFinishResponse(res);
                    break;
                }
                DWORD previous = res->uploadOffset;
                res->uploadOffset += written;
                if (res->request->upload) {
                    DWORD first = previous > res->uploadPayloadStart ? previous : res->uploadPayloadStart;
                    DWORD end = res->uploadOffset < res->uploadPayloadEnd ? res->uploadOffset : res->uploadPayloadEnd;
                    if (end > first) {
                        InterlockedExchangeAdd64(&res->uploaded, end - first);
                        if (res->cancellable) {
                            signalActivity();
                        }
                    }
                }
            }
            if (res->uploadOffset == res->uploadSize && !fillWinUpload(res)) {
                res->code = naettReadError;
                winFinishResponse(res);
                break;
            }
            if (res->uploadSize != 0) {
                if (!WinHttpWriteData(request, res->buffer + res->uploadStart + res->uploadOffset,
                        res->uploadSize - res->uploadOffset, NULL)) {
                    res->code = naettWriteError;
                    winFinishResponse(res);
                }
            } else if (!WinHttpReceiveResponse(request, NULL) && !retryUpload(res, request, GetLastError())) {
                res->code = naettReadError;
                winFinishResponse(res);
            }
        } break;

        //
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
            WINHTTP_ASYNC_RESULT* result = (WINHTTP_ASYNC_RESULT*)statusInformation;
            if (retryUpload(res, request, result->dwError)) {
                break;
            }
            switch (result->dwResult) {
                case API_RECEIVE_RESPONSE:
                case API_QUERY_DATA_AVAILABLE:
                case API_READ_DATA:
                    res->code = naettReadError;
                    break;
                case API_WRITE_DATA:
                    res->code = naettWriteError;
                    break;
                case API_SEND_REQUEST:
                    res->code = naettConnectionError;
                    break;
                default:
                    res->code = naettGenericError;
            }

            winFinishResponse(res);
        } break;
    }
}

static int openWinRequest(InternalRequest* req) {
    LPWSTR verb = winFromUTF8(req->options.method);
    LPCWSTR version = req->upload && req->upload->unknownSize ? L"HTTP/1.1" : NULL;
    HINTERNET request = WinHttpOpenRequest(req->connection, verb, req->resource, version, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, req->secure ? WINHTTP_FLAG_SECURE : 0);
    free(verb);
    if (!request) {
        return 0;
    }
    LPCWSTR headers = packHeaders(req);
    int added = headers && (!headers[0] || WinHttpAddRequestHeaders(
        request, headers, -1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE));
    free((LPWSTR)headers);
    if (!added) {
        WinHttpCloseHandle(request);
        return 0;
    }
    req->request = request;
    return 1;
}

int naettPlatformInitRequest(InternalRequest* req) {
    LPWSTR url = winFromUTF8(req->url);

    URL_COMPONENTS components;
    ZeroMemory(&components, sizeof(components));
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = (DWORD)-1;
    components.dwHostNameLength = (DWORD)-1;
    components.dwUrlPathLength = (DWORD)-1;
    components.dwExtraInfoLength = (DWORD)-1;
    BOOL cracked = WinHttpCrackUrl(url, 0, 0, &components);

    if (!cracked) {
        free(url);
        return 0;
    }

    req->host = wcsndup(components.lpszHostName, components.dwHostNameLength);
    req->resource = wcsndup(components.lpszUrlPath, components.dwUrlPathLength + components.dwExtraInfoLength);
    req->secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    free(url);

    LPWSTR uaBuf = winFromUTF8(req->options.userAgent ? req->options.userAgent : NAETT_UA);
    req->session = WinHttpOpen(uaBuf,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        WINHTTP_FLAG_ASYNC);
    if (!req->session && GetLastError() == ERROR_INVALID_PARAMETER) {
        // Older WinHTTP versions use the default proxy configuration.
        req->session = WinHttpOpen(uaBuf,
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            WINHTTP_FLAG_ASYNC);
    }
    free(uaBuf);

    if (!req->session) {
        return 0;
    }

    WinHttpSetStatusCallback(req->session, callback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS, 0);

#ifdef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
    if (req->upload && req->upload->unknownSize) {
        DWORD protocols = 0;
        if (!WinHttpSetOption(req->session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols))) {
            DWORD error = GetLastError();
            if (error != ERROR_WINHTTP_INVALID_OPTION && error != ERROR_INVALID_PARAMETER) {
                return 0;
            }
        }
    }
#endif

    // Set the connect timeout. Leave the other three timeouts at their default values.
    WinHttpSetTimeouts(req->session, 0, req->options.timeoutMS, 30000, 30000);

    req->connection = WinHttpConnect(req->session, req->host, components.nPort, 0);
    if (!req->connection) {
        naettPlatformFreeRequest(req);
        return 0;
    }

    return openWinRequest(req);
}

static void winSendRequest(InternalResponse* res, HINTERNET request) {
    InternalRequest* req = res->request;
    if (res->cancellable && InterlockedCompareExchange(&res->streamClosing, 0, 0)) {
        return;
    }

    LPCWSTR extraHeaders = WINHTTP_NO_ADDITIONAL_HEADERS;
    WCHAR contentLengthHeader[64];
    DWORD totalLength = 0;
    if (req->upload) {
        if (!resetUpload(res)) {
            res->code = naettReadError;
            winFinishResponse(res);
            return;
        }
        res->uploadSize = 0;
        res->uploadOffset = 0;
        res->uploadEnded = 0;
        InterlockedExchange64(&res->uploaded, 0);
        if (req->upload->unknownSize) {
            if (!WinHttpAddRequestHeaders(request, L"Transfer-Encoding: chunked\r\n", (DWORD)-1,
                    WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
                res->code = naettProtocolError;
                winFinishResponse(res);
                return;
            }
            totalLength = WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH;
        } else if (req->upload->size <= MAXDWORD) {
            totalLength = (DWORD)req->upload->size;
        } else {
            swprintf(contentLengthHeader, 64, L"Content-Length: %llu", (unsigned long long)req->upload->size);
            extraHeaders = contentLengthHeader;
            totalLength = WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH;
        }
    } else {
        int contentLength = req->options.bodyReader(NULL, 0, req->options.bodyReaderData);
        if (contentLength > 0) {
            swprintf(contentLengthHeader, 64, L"Content-Length: %d", contentLength);
            extraHeaders = contentLengthHeader;
        }
    }

    if (!WinHttpSendRequest(request, extraHeaders, -1, NULL, 0, totalLength, (DWORD_PTR)res) &&
        !retryUpload(res, request, GetLastError())) {
        res->code = naettConnectionError;
        winFinishResponse(res);
    }
}

static unsigned int __stdcall startWinRequest(void* context) {
    InternalResponse* res = context;
    if (!InterlockedCompareExchange(&res->cancelRequested, 0, 0)) {
        winSendRequest(res, res->request->request);
    }
    InterlockedExchange(&res->starterDone, 1);
    if (InterlockedCompareExchange(&res->cancelRequested, 0, 0)) {
        winFinishResponse(res);
    }
    return 0;
}

void naettPlatformMakeRequest(InternalResponse* res) {
    InternalRequest* req = res->request;
    if (!req->request && !openWinRequest(req)) {
        res->code = naettConnectionError;
        finishResponse(res);
        return;
    }
    HINTERNET request = req->request;
    if (res->cancellable) {
        DWORD_PTR context = (DWORD_PTR)res;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
            WinHttpSetStatusCallback(request, callback,
                WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) ==
            WINHTTP_INVALID_STATUS_CALLBACK) {
            res->code = naettGenericError;
            finishResponse(res);
            return;
        }
        // WinHTTP may invoke its first callback before WinHttpSendRequest returns.
        res->requestStarter = (HANDLE)_beginthreadex(NULL, 0, startWinRequest, res, 0, NULL);
        if (!res->requestStarter) {
            res->code = naettGenericError;
            winFinishResponse(res);
        }
    } else {
        winSendRequest(res, request);
    }
}

void naettPlatformCancel(InternalResponse* res) {
    InterlockedExchange(&res->cancelRequested, 1);
    if (!res->requestStarter || InterlockedCompareExchange(&res->starterDone, 0, 0)) {
        winFinishResponse(res);
    }
}

void naettPlatformFreeRequest(InternalRequest* req) {
    assert(req != NULL);

    if (req->request != NULL) {
        WinHttpCloseHandle(req->request);
        req->request = NULL;
    }
    if (req->connection != NULL) {
        WinHttpCloseHandle(req->connection);
        req->connection = NULL;
    }
    if (req->session != NULL) {
        WinHttpCloseHandle(req->session);
        req->session = NULL;
    }
    if (req->host != NULL) {
        free(req->host);
        req->host = NULL;
    }
    if (req->resource != NULL) {
        free(req->resource);
        req->resource = NULL;
    }
}

void naettPlatformCloseResponse(InternalResponse* res) {
    if (res->requestStarter) {
        WaitForSingleObject(res->requestStarter, INFINITE);
        CloseHandle(res->requestStarter);
    }
}

#endif  // __WINDOWS__
// End of inlined naett_win.c //

// Inlined naett_android.c: //
//#include "naett_internal.h"

#ifdef __ANDROID__

#ifdef __cplusplus
#error "Cannot build in C++ mode"
#endif

#include <stdlib.h>
#include <string.h>
#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <stdarg.h>

#ifndef NDEBUG
#define LOGD(...) ((void)__android_log_print(ANDROID_LOG_DEBUG, "naett", __VA_ARGS__))
#else
#define LOGD(...) ((void)0)
#endif
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "naett", __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, "naett", __VA_ARGS__))

static JavaVM* globalVM = NULL;

static JavaVM* getVM() {
    if (globalVM == NULL) {
        LOGE("Panic: No VM configured, exiting.");
        exit(42);
    }
    return globalVM;
}

static JNIEnv* getEnv() {
    JavaVM* vm = getVM();
    JNIEnv* env;
    (*vm)->AttachCurrentThread(vm, &env, NULL);
    return env;
}

static int catch (JNIEnv* env) {
    int thrown = (*env)->ExceptionCheck(env);
    if (thrown) {
        (*env)->ExceptionDescribe(env);
    }
    return thrown;
}

static jmethodID getMethod(JNIEnv* env, jobject instance, const char* method, const char* sig) {
    jclass clazz = (*env)->GetObjectClass(env, instance);
    jmethodID id = (*env)->GetMethodID(env, clazz, method, sig);
    (*env)->DeleteLocalRef(env, clazz);
    return id;
}

static jobject call(JNIEnv* env, jobject instance, const char* method, const char* sig, ...) {
    jmethodID methodID = getMethod(env, instance, method, sig);
    va_list args;
    va_start(args, sig);
    jobject result = (*env)->CallObjectMethodV(env, instance, methodID, args);
    va_end(args);
    return result;
}

static void voidCall(JNIEnv* env, jobject instance, const char* method, const char* sig, ...) {
    jmethodID methodID = getMethod(env, instance, method, sig);
    va_list args;
    va_start(args, sig);
    (*env)->CallVoidMethodV(env, instance, methodID, args);
    va_end(args);
}

static jint intCall(JNIEnv* env, jobject instance, const char* method, const char* sig, ...) {
    jmethodID methodID = getMethod(env, instance, method, sig);
    va_list args;
    va_start(args, sig);
    jint result = (*env)->CallIntMethodV(env, instance, methodID, args);
    va_end(args);
    return result;
}

void naettPlatformInit(naettInitData initData) {
    globalVM = initData;
}

int naettPlatformInitRequest(InternalRequest* req) {
    JNIEnv* env = getEnv();
    (*env)->PushLocalFrame(env, 10);
    jclass URL = (*env)->FindClass(env, "java/net/URL");
    jmethodID newURL = (*env)->GetMethodID(env, URL, "<init>", "(Ljava/lang/String;)V");
    jstring urlString = (*env)->NewStringUTF(env, req->url);
    jobject url = (*env)->NewObject(env, URL, newURL, urlString);
    if (catch (env)) {
        (*env)->PopLocalFrame(env, NULL);
        return 0;
    }
    req->urlObject = (*env)->NewGlobalRef(env, url);
    (*env)->PopLocalFrame(env, NULL);
    return 1;
}

static void* processRequest(void* data) {
    const int bufSize = 10240;
    char byteBuffer[bufSize];
    InternalResponse* res = (InternalResponse*)data;
    InternalRequest* req = res->request;

    JNIEnv* env = getEnv();
    (*env)->PushLocalFrame(env, 100);

    jobject connection = call(env, req->urlObject, "openConnection", "()Ljava/net/URLConnection;");
    if (catch (env)) {
        res->code = naettConnectionError;
        goto finally;
    }

    {
        jstring name = (*env)->NewStringUTF(env, "User-Agent");
        jstring value = (*env)->NewStringUTF(env, req->options.userAgent ? req->options.userAgent : NAETT_UA);
        voidCall(env, connection, "addRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V", name, value);
        (*env)->DeleteLocalRef(env, name);
        (*env)->DeleteLocalRef(env, value);
    }

    KVLink* header = req->options.headers;
    while (header != NULL) {
        jstring name = (*env)->NewStringUTF(env, header->key);
        jstring value = (*env)->NewStringUTF(env, header->value);
        voidCall(env, connection, "addRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V", name, value);
        (*env)->DeleteLocalRef(env, name);
        (*env)->DeleteLocalRef(env, value);
        header = header->next;
    }

    jobject outputStream = NULL;
    if (strcmp(req->options.method, "POST") == 0 || strcmp(req->options.method, "PUT") == 0 ||
        strcmp(req->options.method, "PATCH") == 0 || strcmp(req->options.method, "DELETE") == 0) {
        voidCall(env, connection, "setDoOutput", "(Z)V", 1);
        outputStream = call(env, connection, "getOutputStream", "()Ljava/io/OutputStream;");
    }
    jobject methodString = (*env)->NewStringUTF(env, req->options.method);
    voidCall(env, connection, "setRequestMethod", "(Ljava/lang/String;)V", methodString);
    voidCall(env, connection, "setConnectTimeout", "(I)V", req->options.timeoutMS);
    voidCall(env, connection, "setInstanceFollowRedirects", "(Z)V", 1);

    voidCall(env, connection, "connect", "()V");
    if (catch (env)) {
        res->code = naettConnectionError;
        goto finally;
    }

    jbyteArray buffer = (*env)->NewByteArray(env, bufSize);

    if (outputStream != NULL) {
        int bytesRead = 0;
        if (req->options.bodyReader != NULL)
            do {
                bytesRead = req->options.bodyReader(byteBuffer, bufSize, req->options.bodyReaderData);
                if (bytesRead > 0) {
                    (*env)->SetByteArrayRegion(env, buffer, 0, bytesRead, (const jbyte*) byteBuffer);
                    voidCall(env, outputStream, "write", "([BII)V", buffer, 0, bytesRead);
                } else {
                    break;
                }
            } while (!res->closeRequested);
        voidCall(env, outputStream, "close", "()V");
    }

    jobject headerMap = call(env, connection, "getHeaderFields", "()Ljava/util/Map;");
    if (catch (env)) {
        res->code = naettProtocolError;
        goto finally;
    }

    jobject headerSet = call(env, headerMap, "keySet", "()Ljava/util/Set;");
    jarray headers = call(env, headerSet, "toArray", "()[Ljava/lang/Object;");
    jsize headerCount = (*env)->GetArrayLength(env, headers);

    KVLink *firstHeader = NULL;
    for (int i = 0; i < headerCount; i++) {
        jstring name = (*env)->GetObjectArrayElement(env, headers, i);
        if (name == NULL) {
            continue;
        }
        const char* nameString = (*env)->GetStringUTFChars(env, name, NULL);

        jobject values = call(env, headerMap, "get", "(Ljava/lang/Object;)Ljava/lang/Object;", name);
        jstring value = call(env, values, "get", "(I)Ljava/lang/Object;", 0);
        const char* valueString = (*env)->GetStringUTFChars(env, value, NULL);

        naettAlloc(KVLink, node);
        node->key = strdup(nameString);
        node->value = strdup(valueString);
        node->next = firstHeader;
        firstHeader = node;

        (*env)->ReleaseStringUTFChars(env, name, nameString);
        (*env)->ReleaseStringUTFChars(env, value, valueString);

        (*env)->DeleteLocalRef(env, name);
        (*env)->DeleteLocalRef(env, value);
        (*env)->DeleteLocalRef(env, values);
    }
    res->headers = firstHeader;

    const char *contentLength = naettGetHeader((naettRes *)res, "Content-Length");
    if (!contentLength || sscanf(contentLength, "%d", &res->contentLength) != 1) {
        res->contentLength = -1;
    }

    int statusCode = intCall(env, connection, "getResponseCode", "()I");

    jobject inputStream = NULL;

    if (statusCode >= 400) {
        inputStream = call(env, connection, "getErrorStream", "()Ljava/io/InputStream;");
    } else {
        inputStream = call(env, connection, "getInputStream", "()Ljava/io/InputStream;");
    }
    if (catch (env)) {
        res->code = naettProtocolError;
        goto finally;
    }

    jint bytesRead = 0;
    do {
        bytesRead = intCall(env, inputStream, "read", "([B)I", buffer);
        if (catch (env)) {
            res->code = naettReadError;
            goto finally;
        }
        if (bytesRead < 0) {
            break;
        } else if (bytesRead > 0) {
            (*env)->GetByteArrayRegion(env, buffer, 0, bytesRead, (jbyte*) byteBuffer);
            req->options.bodyWriter(byteBuffer, bytesRead, req->options.bodyWriterData);
            res->totalBytesRead += bytesRead;
        }
    } while (!res->closeRequested);

    voidCall(env, inputStream, "close", "()V");

    res->code = statusCode;

finally:
    res->complete = 1;
    (*env)->PopLocalFrame(env, NULL);
    JavaVM* vm = getVM();
    (*env)->ExceptionClear(env);
    (*vm)->DetachCurrentThread(vm);
    return NULL;
}

static void startWorkerThread(InternalResponse* res) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&res->workerThread, &attr, processRequest, res);
    pthread_setname_np(res->workerThread, "naett worker thread");
}

void naettPlatformMakeRequest(InternalResponse* res) {
    startWorkerThread(res);
}

void naettPlatformFreeRequest(InternalRequest* req) {
    JNIEnv* env = getEnv();
    (*env)->DeleteGlobalRef(env, req->urlObject);
}

void naettPlatformCloseResponse(InternalResponse* res) {
    res->closeRequested = 1;
    if (res->workerThread != 0) {
        int joinResult = pthread_join(res->workerThread, NULL);
        if (joinResult != 0) {
            LOGE("Failed to join: %s", strerror(joinResult));
        }
    }
}

#endif  // __ANDROID__
// End of inlined naett_android.c //

// End of inlined amalgam.h //
