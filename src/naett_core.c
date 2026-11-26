#include "naett_internal.h"
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
