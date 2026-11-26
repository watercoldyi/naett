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
