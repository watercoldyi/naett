#include "naett_internal.h"

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
