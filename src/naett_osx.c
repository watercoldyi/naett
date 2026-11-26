#include "naett_internal.h"

#ifdef __APPLE__

#include "naett_objc.h"
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
