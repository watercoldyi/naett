#ifndef LIBNAETT_H
#define LIBNAETT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if __ANDROID__
#include <jni.h>
typedef JavaVM* naettInitData;
#else
typedef void* naettInitData;
#endif

#define NAETT_UA "Naett/1.0"

/**
 * @brief Global init method.
 * Call to initialize the library.
 */
void naettInit(naettInitData initThing);

typedef struct naettReq naettReq;
typedef struct naettRes naettRes;
// If naettReadFunc is called with NULL dest, it must respond with the body size
typedef int (*naettReadFunc)(void* dest, int bufferSize, void* userData);
typedef int (*naettWriteFunc)(const void* source, int bytes, void* userData);
typedef int (*naettHeaderLister)(const char* name, const char* value, void* userData);

// Option to `naettRequest`
typedef struct naettOption naettOption;

// Sets request method. Defaults to "GET".
naettOption* naettMethod(const char* method);
// Adds a request header.
naettOption* naettHeader(const char* name, const char* value);
// Sets the request body. Ignored if a body reader is configured.
// The body is not copied, and the passed pointer must be valid for the
// lifetime of the request.
naettOption* naettBody(const char* body, int size);
// Sets a request body reader.
naettOption* naettBodyReader(naettReadFunc reader, void* userData);
// Sets a response body writer.
naettOption* naettBodyWriter(naettWriteFunc writer, void* userData);
// Sets connection timeout in milliseconds.
naettOption* naettTimeout(int milliSeconds);
// Sets the user agent.
naettOption* naettUserAgent(const char *userAgent);

/**
 * @brief Creates a new request to the specified url.
 * Use varargs options to configure the connection and following request.
 */
#define naettRequest(url, ...) naettRequest_va(url, countOptions(__VA_ARGS__), ##__VA_ARGS__)

/**
 * @brief Creates a new request to the specified url.
 * Uses an array of options rather than varargs.
 */
naettReq* naettRequestWithOptions(const char* url, int numOptions, const naettOption** options);

typedef struct {
    const char* name;
    const char* value;
    int valueSize;
    const char* path;
    const char* filename;
    const char* contentType;
} naettMultipartPart;

#define NAETT_UNKNOWN_SOURCE_LENGTH (-2)

/**
 * Creates a streaming upload on Windows or Apple platforms. Pass either a
 * UTF-8 bodyFile path or one or more ordered multipart parts. Each part uses
 * either value/valueSize or a UTF-8 path/filename and optional contentType.
 * Text and metadata are copied and files stay open until naettFree. The
 * options are consumed. Supply an optional error code output for setup failures.
 * The upload manages Content-Length, Transfer-Encoding and multipart Content-Type.
 * Wait for completion before closing the response.
 */
naettReq* naettRequestWithUpload(const char* url, int numOptions, const naettOption** options,
    const char* bodyFile, int numParts, const naettMultipartPart* parts, int* error);

/**
 * Creates an upload driven by naettGetUploadSourceRequest and naettSupplyUploadSource.
 * With zero parts, sourceLengths[0] is the raw body length. Otherwise each
 * length describes its multipart part. Use -1 for ordinary text or file parts
 * and NAETT_UNKNOWN_SOURCE_LENGTH for a source read to EOF.
 * A source part supplies a filename and no value or path. Options are consumed.
 */
naettReq* naettRequestWithSources(const char* url, int numOptions, const naettOption** options,
    int numParts, const naettMultipartPart* parts, const int64_t* sourceLengths, int* error);

/**
 * @brief Makes a request and returns a response object.
 * The actual request is processed asynchronously, use `naettComplete`
 * to check if the response is completed.
 *
 * A request object can be reused multiple times to make requests, but
 * there can be only one active request using the same request object.
 */
naettRes* naettMake(naettReq* request);

/** Starts a buffered response that can be cancelled on Windows and Apple platforms. */
naettRes* naettMakeCancellable(naettReq* request);

/** Cancels a cancellable response. Wait for naettComplete before closing it. */
void naettCancel(naettRes* response);

/** Snapshot before polling requests. Wait for activity after that snapshot. */
uint64_t naettActivityVersion(void);
void naettWaitForActivity(uint64_t version, int timeoutMS);

/** Starts a response with a bounded body queue on Windows and Apple platforms. */
naettRes* naettMakeWithSink(naettReq* request);

/** Starts a response that writes successful HTTP bodies to a temporary file next to path. */
naettRes* naettMakeWithResponseFile(naettReq* request, const char* path, int* error);

/** After a completed 2xx response, publishes the file at path. Returns zero on success. */
int naettCommitResponseFile(naettRes* response);

/** Copies queued bytes. Zero means no data is available within timeoutMS. */
int naettReadSink(naettRes* response, void* buffer, int capacity, int timeoutMS);

/** Cancels a streaming download. Wait for naettComplete before closing. */
void naettStopSink(naettRes* response);

/** Returns a one-based source index, 0 on timeout or -1 when no input is currently needed. */
int naettGetUploadSourceRequest(naettRes* response, int* capacity, uint64_t* attempt, int timeoutMS);

/** Supplies one requested chunk. Zero bytes signals EOF. A stale attempt is rejected. */
int naettSupplyUploadSource(naettRes* response, uint64_t attempt, const void* data, int size);

/** Cancels a request driven by an upload source. Wait for completion before closing. */
void naettStopUploadSource(naettRes* response);

/**
 * @brief Frees a previously allocated request object.
 * The request must not have any pending responses.
 */
void naettFree(naettReq* request);

/**
 * @brief Checks if a response is complete, with a result
 * or with an error.
 * Use `naettGetStatus` to get the status.
 */
int naettComplete(const naettRes* response);

enum naettStatus {
    naettConnectionError = -1,
    naettProtocolError = -2,
    naettReadError = -3,
    naettWriteError = -4,
    naettGenericError = -5,
    naettCancelled = -6,
    naettProcessing = 0,
};

/**
 * @brief Returns the status of a response.
 * Status codes > 0 are HTTP status codes as returned by the server.
 * Status codes < 0 are processing errors according to the `naettStatus`
 * enum values.
 */
int naettGetStatus(const naettRes* response);

/** Returns bytes sent so far. A totalSize of UINT64_MAX means unknown. */
uint64_t naettGetUploadProgress(const naettRes* response, uint64_t* totalSize);

/** Returns response body bytes received so far. A totalSize of -1 means unknown. */
uint64_t naettGetDownloadProgress(const naettRes* response, int64_t* totalSize);

/**
 * @brief Returns the response body.
 * The body returned by this method is always empty when a custom
 * body reader has been set up using the `naettBodyReader` option.
 */
const void* naettGetBody(naettRes* response, int* outSize);

/**
 * @brief Returns the HTTP header value for the specified header name.
 */
const char* naettGetHeader(naettRes* response, const char* name);

/**
 * @brief Returns how many bytes have been read from the response so far,
 * and the integer pointed to by totalSize gets the Content-Length if available,
 * or -1 if not (or 0 if headers have not been read yet).
 */
int naettGetTotalBytesRead(naettRes* response, int* totalSize);

/**
 * @brief Enumerates all response headers as long as the `lister`
 * returns true.
 */
void naettListHeaders(naettRes* response, naettHeaderLister lister, void* userData);

/**
 * @brief Returns the request that initiated this response
 */
naettReq* naettGetRequest(naettRes* response);

/**
 * @brief Closes a response object.
 */
void naettClose(naettRes* response);

// Varargs glue
naettReq* naettRequest_va(const char* url, int numOptions, ...);
#define countOptions(...) ((sizeof((void*[]){ __VA_ARGS__ }) / sizeof(void*)))

#ifdef __cplusplus
}
#endif

#endif  // LIBNAETT_H
