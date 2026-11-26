# naett /nɛt:/

Tiny HTTP client library in C.

Wraps native HTTP client functionality on macOS, Windows, Linux, iOS and Android in a single, simple non-blocking API.

## Using `naett`

Get the `naett.c` and `naett.h` files and throw them into your project. Check out the [example](./example) for a basic `Makefile` - based setup.

The library needs to be initialized by a call to `naettInit()`. On Android, you need to provide a `JavaVM*` handle in the call to `naettInit()`.
On the other platforms, call with `NULL`.

See `naett.h` for reference docs.

## Building with CMake

The CMake build currently supports Windows and macOS (Linux, iOS and Android
are not wired up yet).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /path/to/prefix
```

`naett` is built as a static library by default; pass
`-DNAETT_BUILD_SHARED=ON` to build a shared library instead. After
installation, the package can be consumed with:

```cmake
find_package(naett CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE naett::naett)
```

The library can also be embedded directly via `add_subdirectory()` or
`FetchContent`; the target is exposed as `naett::naett`.

On Windows, MSVC 2019 16.8+ or MinGW is required.

On Windows 8.1 and later, naett follows the Windows system and per-user proxy
settings through WinHTTP. Earlier versions use the default WinHTTP proxy
configuration.

## LuaJIT

Build the shared library for the same architecture as LuaJIT.

```sh
cmake -S . -B build -DNAETT_BUILD_SHARED=ON
cmake --build build --config Release
```

Run LuaJIT from the repository root so `require("naett")` finds `naett.lua`.
Set `NAETT_LIBRARY` to the full path of `naett.dll` (typically in
`build/Release` with Visual Studio) or `libnaett.dylib` (typically in `build`
on macOS). If `NAETT_LIBRARY` is unset, LuaJIT looks up `naett` using its
normal shared-library search. `cmake --install build --config Release` also
installs the module under `<prefix>/share/lua/5.1` by default. For a custom prefix,
add that directory's `?.lua` pattern to `package.path` or `LUA_PATH`.

```lua
local request = require("naett").request

local response, err = request("POST", "https://example.com/api", {
    headers = { ["Content-Type"] = "application/json" },
    body = '{"hello":"world"}',
    timeout = 5000,
})
assert(response, err)
print(response.status, response.headers["content-type"], response.body)
```

`request(url, options)` defaults to `GET`. Set `options.method` to change it,
or use `request(method, url, options)`. The table-only form
`request { url = url, method = "POST", body = body }` is also supported.
Options include `headers` (a table of string names and values), `body` (a
binary-safe string), `body_file`, `request_source`, `multipart`, `user_agent` (a string), and
`timeout` (the native timeout in milliseconds, which controls connection time
on Windows). Pass at most one of `body`, `body_file`, `request_source`, and `multipart`.

The call waits for the response to finish. All HTTP statuses, including 4xx and
5xx, return a table with `status`, `headers`, and `body`. Response header keys
are lowercase. If the backend reports a header multiple times, its value is
an array in the order reported by the backend. An empty body is `""`.
Transport failures return `nil, message, native_status`. If the native request
could not be created, the third value is `nil`. Invalid Lua arguments raise an
error.

### Concurrent requests

`request_async()` takes the same arguments as `request()` and returns a task.
Start multiple tasks and drive them with one `poll(timeout_ms)` call per loop
iteration. Each pass processes at most one upload chunk and one download
chunk per task. The timeout controls how long `poll()` waits when no task can
make progress. `poll()` and `poll(0)` process available work immediately.
The return value is the number of tasks completed by that call.

```lua
local naett = require("naett")
local first = assert(naett.request_async("GET", first_url))
local second = assert(naett.request_async("GET", second_url, {
    response_file = "second.zip",
}))

while not (first:done() and second:done()) do
    naett.poll(50)
end

local response, err = first:result()
assert(response, err)
local downloaded, download_err = second:result()
assert(downloaded, download_err)
```

`task:done()` is true after result processing and cleanup. `task:result()`
uses the same success and error values as `request()` and returns
`nil, "request pending", 0` while a task is active. `task:cancel()` returns
whether it accepted a cancellation. Continue polling until it completes
with `nil, "request cancelled", -6`. Callback exceptions are raised by
`task:result()` after cleanup. The callbacks run on the thread calling
`poll()`. Synchronous `request()` starts a task and polls until it completes.

### Streaming uploads

For a raw upload, pass the local file path as `body_file` and set any headers
the server expects. File paths use UTF-8.

```lua
local request = require("naett").request

local raw, raw_err = request("PUT", upload_url, {
    body_file = "archive.zip",
    headers = { ["Content-Type"] = "application/zip" },
    on_upload_progress = function(sent, total)
        print(sent, total)
    end,
})
assert(raw, raw_err)

local form, form_err = request("POST", form_url, {
    multipart = {
        { name = "title", value = "Monthly report" },
        { name = "files", path = "report.pdf", content_type = "application/pdf" },
        { name = "files", path = "data.zip" },
    },
})
assert(form, form_err)
```

`multipart` is an ordered list. Field names can repeat. A part has a string
`value` or a file `path`. File parts accept an optional `filename` (the last
component of `path` by default) and `content_type` (defaults to
`application/octet-stream`). The native code streams regular files on Windows
and macOS. It generates the multipart `Content-Type` and boundary. Windows
sends a calculated `Content-Length`. On macOS, NSURLSession manages the
transfer framing and can use chunked transfer for HTTP/1.1 multipart streams.
Leave `Content-Length` and `Transfer-Encoding` to the backend, and leave
`Content-Type` to naett when using `multipart`.

Use `request_source` to stream bytes produced by Lua. The required
`read(max_bytes)` function returns a non-empty binary string of at most
`max_bytes` bytes. The library calls it on the thread running `request()`.
When `length` is supplied, naett reads exactly that many bytes, and an early
`nil` fails with `naettReadError`. When `length` is omitted, `nil` marks the
end of the source. `nil, err` reports a read failure in either mode. The
caller owns any resources used by the source.

```lua
local remaining = 1024 * 1024
local response, err = request("PUT", upload_url, {
    request_source = {
        read = function(max_bytes)
            if remaining == 0 then
                return nil
            end
            local count = math.min(max_bytes, remaining)
            remaining = remaining - count
            return string.rep("x", count)
        end,
    },
})
assert(response, err)
```

If a redirect or authentication challenge requires resending the body,
the optional `rewind()` function must reset the source to its beginning.
Return `false` or `nil, err` to report a rewind failure. A source without
`rewind` reports an error when a resend requires reading it again.
A multipart file part may provide `source` and `filename` in place of `path`.
On Windows, an upload with an unknown length uses HTTP/1.1 chunked transfer.

Set `on_upload_progress(sent, total)` on a streaming upload to receive
byte counts during `request()`. Counts include multipart framing and can
restart if the backend resends the request. `total` is `nil` for uploads of
unknown length. Use the returned HTTP status to check whether the server
accepted the body.

### Streaming downloads

On Windows and macOS, pass a UTF-8 `response_file` path to stream a download
to disk. naett writes to a temporary file in the destination directory and
moves it into place after a completed 2xx response. If the destination
exists, a successful download replaces it. Other HTTP statuses return their
status and headers. Network or file errors discard the temporary file.

```lua
local response, err = request("GET", download_url, {
    response_file = "archive.zip",
    on_download_progress = function(received, total)
        print(received, total)
    end,
})
assert(response, err)
assert(response.status >= 200 and response.status < 300, response.status)
```

Choose either `response_file` or `response_sink`. The upload option `body_file`
can be used in the same request as `response_file`.

On Windows and macOS, set `response_sink` to process the response body as
binary strings while `request()` waits. The callback runs on the Lua thread
that called `request()`. The native queue is bounded to 64 KiB.

```lua
local request = require("naett").request
local bytes = 0
local response, err = request("GET", download_url, {
    response_sink = function(chunk)
        bytes = bytes + #chunk
    end,
    on_download_progress = function(received, total)
        print(received, total)
    end,
})
assert(response, err)
print(response.status, bytes)
```

A callback can return `nil, err` or `false` to cancel the transfer. `request()`
then returns `nil, message, -4`. If the callback throws, the exception is
raised after cleanup. A successful callback can omit its return values.
HTTP error bodies also go to the callback. `status` and `headers` are available
when `request()` returns. Both streaming download options leave `body` as `nil`.

`on_download_progress(received, total)` also works with buffered responses.
It runs on the calling Lua thread when either byte count changes. `received`
counts bytes received by the backend. `total` comes from this response and
is `nil` before a usable length is known or throughout an indeterminate
transfer, such as a chunked response. Responses without a body, including
`HEAD` and HTTP 204, report a total of zero once the headers arrive.

## Platform implementations

`naett` uses the following HTTP client libraries on each platform:

| Platform | Library / component | Build with |
| --- | --- | --- |
| macOS, iOS | NSURLRequest | -fblocks -framework Foundation -framework CoreFoundation |
| Windows | WinHTTP Sessions | -lwinhttp |
| Android | java.net.URL | NDK |
| Linux | libcurl | -lcurl -lpthread |

### Example

```C
#include "naett.h"
#include <unistd.h>
#include <stdio.h>

int main(int argc, char** argv) {
    naettInit(NULL);

    naettReq* req =
        naettRequest("https://foo.site.net", naettMethod("GET"), naettHeader("accept", "application/json"));

    naettRes* res = naettMake(req);

    while (!naettComplete(res)) {
        usleep(100 * 1000);
    }

    if (naettGetStatus(res) < 0) {
        printf("Request failed\n");
        return 1;
    }

    int bodyLength = 0;
    const char* body = naettGetBody(res, &bodyLength);

    printf("Got %d bytes of type '%s':\n", bodyLength, naettGetHeader(res, "Content-Type"));
    printf("%.100s\n...\n", body);
}
```
