local ffi = require("ffi")
local jit = require("jit")

ffi.cdef[[
typedef struct naettReq naettReq;
typedef struct naettRes naettRes;
typedef struct naettOption naettOption;
typedef int (*naettHeaderLister)(const char* name, const char* value, void* userData);

void naettInit(void* initData);
naettOption* naettMethod(const char* method);
naettOption* naettHeader(const char* name, const char* value);
naettOption* naettBody(const char* body, int size);
naettOption* naettTimeout(int milliseconds);
naettOption* naettUserAgent(const char* userAgent);
naettReq* naettRequestWithOptions(const char* url, int count, const naettOption** options);
typedef struct {
    const char* name;
    const char* value;
    int valueSize;
    const char* path;
    const char* filename;
    const char* contentType;
} naettMultipartPart;
naettReq* naettRequestWithUpload(const char* url, int count, const naettOption** options,
    const char* bodyFile, int numParts, const naettMultipartPart* parts, int* error);
naettReq* naettRequestWithSources(const char* url, int count, const naettOption** options,
    int numParts, const naettMultipartPart* parts, const int64_t* sourceLengths, int* error);
naettRes* naettMakeCancellable(naettReq* request);
void naettCancel(naettRes* response);
uint64_t naettActivityVersion(void);
void naettWaitForActivity(uint64_t version, int timeoutMS);
naettRes* naettMakeWithSink(naettReq* request);
naettRes* naettMakeWithResponseFile(naettReq* request, const char* path, int* error);
int naettCommitResponseFile(naettRes* response);
int naettReadSink(naettRes* response, void* buffer, int capacity, int timeoutMS);
int naettGetUploadSourceRequest(naettRes* response, int* capacity, uint64_t* attempt, int timeoutMS);
int naettSupplyUploadSource(naettRes* response, uint64_t attempt, const void* data, int size);
int naettComplete(const naettRes* response);
int naettGetStatus(const naettRes* response);
uint64_t naettGetUploadProgress(const naettRes* response, uint64_t* totalSize);
uint64_t naettGetDownloadProgress(const naettRes* response, int64_t* totalSize);
const void* naettGetBody(naettRes* response, int* size);
void naettListHeaders(naettRes* response, naettHeaderLister lister, void* userData);
void naettClose(naettRes* response);
void naettFree(naettReq* request);
]]

local has_activity_wait = ffi.os == "Windows" or ffi.os == "OSX"
if not has_activity_wait then
    ffi.cdef[[int usleep(unsigned int microseconds);]]
end

local lib = ffi.load(os.getenv("NAETT_LIBRARY") or "naett")
lib.naettInit(nil)

local transport_errors = {
    [-1] = "connection error",
    [-2] = "protocol error",
    [-3] = "read error",
    [-4] = "write error",
    [-5] = "request failed",
    [-6] = "request cancelled",
    [0] = "missing HTTP status",
}

local upload_errors = {
    [-2] = "streaming upload is unavailable or incompatible with the request",
    [-3] = "could not open upload file for reading",
}

local unknown_source_length = -2

local function is_token(value)
    return type(value) == "string" and value:match("^[!#$%%&'*+.^_`|~a-zA-Z0-9-]+$") ~= nil
end

local function is_header_value(value)
    return type(value) == "string" and not value:find("[%z\r\n]")
end

local function is_path(value)
    return type(value) == "string" and value ~= "" and not value:find("%z")
end

local function is_part_header(value)
    return type(value) == "string" and value ~= "" and not value:find("%c")
end

local function normalize_source(source)
    assert(type(source) == "table", "upload source must be a table")
    local length, read, rewind = source.length, source.read, source.rewind
    assert(length == nil or (type(length) == "number" and length >= 0 and length <= 9007199254740991
        and length % 1 == 0),
        "upload source length must be an exact non-negative byte count")
    assert(type(read) == "function", "upload source read must be a function")
    assert(rewind == nil or type(rewind) == "function", "upload source rewind must be a function")
    return { length = length, read = read, rewind = rewind }
end

local function normalize_parts(parts)
    assert(type(parts) == "table" and #parts > 0 and #parts <= 2147483647,
        "multipart must be a non-empty array of parts")
    local count = #parts
    for key in pairs(parts) do
        assert(type(key) == "number" and key >= 1 and key <= count and key % 1 == 0,
            "multipart must be a non-empty array of parts")
    end
    local normalized = {}
    for i = 1, count do
        local part = parts[i]
        assert(type(part) == "table" and is_part_header(part.name), "multipart part must have a valid name")
        local value, path, source = part.value, part.path, part.source
        if path ~= nil or source ~= nil then
            assert(value == nil and (path == nil or (source == nil and is_path(path))),
                "file parts must have a path or source and no value")
            local filename, content_type = part.filename, part.content_type
            if filename == nil and path ~= nil then
                filename = path:match("([^/\\]+)$")
            end
            assert(is_part_header(filename), "file part must have a valid filename")
            assert(content_type == nil or is_part_header(content_type), "invalid file content_type")
            normalized[i] = { name = part.name, path = path, filename = filename, content_type = content_type,
                source = source ~= nil and normalize_source(source) or nil }
        else
            assert(type(value) == "string" and #value <= 2147483647
                and part.filename == nil and part.content_type == nil, "text parts must have a string value")
            normalized[i] = { name = part.name, value = value }
        end
    end
    return normalized
end

local function make_request(url, method, options, body)
    local scheme = type(url) == "string" and url:match("^([a-zA-Z]+)://[^/?#%c ]+")
    assert(scheme and (scheme:lower() == "http" or scheme:lower() == "https")
        and not url:find("%c"), "expected an HTTP or HTTPS URL")
    assert(is_token(method), "method must be an HTTP token")
    assert(body == nil or (type(body) == "string" and #body <= 2147483647),
        "body must be a string of at most 2147483647 bytes")

    local body_file, request_source, multipart = options.body_file, options.request_source, options.multipart
    local stream_options = (body_file ~= nil and 1 or 0) + (request_source ~= nil and 1 or 0) +
        (multipart ~= nil and 1 or 0)
    local streaming = stream_options > 0
    assert(stream_options + (body ~= nil and 1 or 0) <= 1,
        "body, body_file, request_source and multipart are mutually exclusive")
    assert(body_file == nil or is_path(body_file), "body_file must be a non-empty path")
    local parts, native_parts, upload_error, sources, lengths
    if request_source ~= nil then
        sources = { normalize_source(request_source) }
        lengths = ffi.new("int64_t[1]", sources[1].length or unknown_source_length)
    end
    if multipart ~= nil then
        parts = normalize_parts(multipart)
        native_parts = ffi.new("naettMultipartPart[?]", #parts)
        for i, part in ipairs(parts) do
            local native = native_parts[i - 1]
            native.name, native.value, native.path = part.name, part.value, part.path
            native.filename, native.contentType = part.filename, part.content_type
            native.valueSize = part.value and #part.value or 0
            if part.source then
                sources = sources or {}
                sources[i] = part.source
            end
        end
        if sources then
            lengths = ffi.new("int64_t[?]", #parts)
            for i, part in ipairs(parts) do
                lengths[i - 1] = part.source and (part.source.length or unknown_source_length) or -1
            end
        end
    end
    if streaming then
        upload_error = ffi.new("int[1]")
    end

    local timeout, user_agent = options.timeout, options.user_agent
    assert(timeout == nil or (type(timeout) == "number" and timeout >= 1
        and timeout <= 2147483647 and timeout % 1 == 0), "timeout must be a positive integer in milliseconds")
    assert(user_agent == nil or is_header_value(user_agent), "user_agent must be a valid HTTP header value")

    local headers = options.headers
    assert(headers == nil or type(headers) == "table", "headers must be a table")
    local header_list = {}
    if headers then
        for name, value in pairs(headers) do
            assert(is_token(name), "header names must be HTTP tokens")
            assert(is_header_value(value), "header values must be strings without NUL, CR or LF")
            if streaming then
                local key = name:lower()
                assert(key ~= "content-length" and key ~= "transfer-encoding",
                    "streaming upload manages Content-Length and Transfer-Encoding")
                assert(multipart == nil or key ~= "content-type", "multipart manages Content-Type")
            end
            header_list[#header_list + 1] = { name, value }
        end
    end

    local native_options = ffi.new("const naettOption*[?]", #header_list + 4)
    native_options[0] = lib.naettMethod(method)
    local count = 1
    for _, header in ipairs(header_list) do
        native_options[count] = lib.naettHeader(header[1], header[2])
        count = count + 1
    end
    if timeout then
        native_options[count] = lib.naettTimeout(timeout)
        count = count + 1
    end
    if user_agent then
        native_options[count] = lib.naettUserAgent(user_agent)
        count = count + 1
    end
    if body then
        native_options[count] = lib.naettBody(body, #body)
        count = count + 1
    end
    if sources then
        local handle = lib.naettRequestWithSources(url, count, native_options, parts and #parts or 0,
            native_parts, lengths, upload_error)
        return handle, upload_error[0], sources
    elseif streaming then
        local handle = lib.naettRequestWithUpload(url, count, native_options, body_file, parts and #parts or 0,
            native_parts, upload_error)
        return handle, upload_error[0]
    end
    return lib.naettRequestWithOptions(url, count, native_options)
end

-- C 结构体暂时借用 parts 内的字符串，构造请求时保留这些 Lua 局部变量。
jit.off(make_request)

local function read_response(response, streamed)
    local status = lib.naettGetStatus(response)
    if status <= 0 then
        return nil, transport_errors[status] or "unknown transport error", status
    end

    local body
    if not streamed then
        local size = ffi.new("int[1]")
        local data = lib.naettGetBody(response, size)
        body = size[0] == 0 and "" or ffi.string(data, size[0])
    end
    local headers = {}
    local lister = ffi.cast("naettHeaderLister", function(name, value)
        local key = ffi.string(name):lower()
        local text = ffi.string(value)
        local previous = headers[key]
        if previous == nil then
            headers[key] = text
        elseif type(previous) == "string" then
            headers[key] = { previous, text }
        else
            previous[#previous + 1] = text
        end
        return 1
    end)
    local ok, err = pcall(lib.naettListHeaders, response, lister, nil)
    lister:free()
    if not ok then
        error(err, 0)
    end
    return { status = status, headers = headers, body = body }
end

-- naettListHeaders 会同步回调 Lua，这段代码必须在解释器中执行。
jit.off(read_response)

local function deliver_chunk(sink, buffer, size)
    return sink(ffi.string(buffer, size))
end

local function read_source(source, attempts, index, attempt, capacity)
    if attempts[index] ~= attempt then
        if attempts[index] ~= nil then
            if not source.rewind then
                error("upload source needs rewind to resend the request", 0)
            end
            local ok, err = source.rewind()
            if ok == false or (ok == nil and err ~= nil) then
                error(err or "could not rewind upload source", 0)
            end
        end
        attempts[index] = attempt
    end
    local chunk, err = source.read(capacity)
    if chunk == nil then
        if err ~= nil then
            error(err, 0)
        end
        if source.length ~= nil then
            error("upload source ended before its declared length", 0)
        end
        return nil
    end
    if type(chunk) ~= "string" or #chunk == 0 or #chunk > capacity then
        error("upload source read must return 1 to " .. capacity .. " bytes", 0)
    end
    return chunk
end

local active_tasks = {}
local poll_depth = 0
local Task = {}
Task.__index = Task

local function stop_task(state, reason, code, thrown)
    if state.failure then
        return false
    end
    state.failure = { reason = reason, code = code, thrown = thrown }
    lib.naettCancel(state.response)
    return true
end

function Task:done()
    return self._pending == nil
end

function Task:result()
    if self._pending then
        return nil, "request pending", 0
    end
    if self._thrown then
        error(self._error, 0)
    end
    return self._result, self._error, self._code
end

function Task:cancel()
    local state = self._pending
    return state ~= nil and stop_task(state, "request cancelled", -6)
end

local sink_capacity = 65536

local function request_async(url, options, method_options)
    local method
    if type(url) == "table" then
        options, url = url, url.url
    elseif type(options) == "string" then
        method, url, options = url, options, method_options
    end
    if options == nil then
        options = {}
    end
    assert(type(options) == "table", "options must be a table")
    local upload_progress = options.on_upload_progress
    assert(upload_progress == nil or (type(upload_progress) == "function"
        and (options.body_file ~= nil or options.request_source ~= nil or options.multipart ~= nil)),
        "on_upload_progress must be a function used with body_file, request_source or multipart")
    local download_progress = options.on_download_progress
    assert(download_progress == nil or type(download_progress) == "function",
        "on_download_progress must be a function")
    local sink = options.response_sink
    assert(sink == nil or type(sink) == "function", "response_sink must be a function")
    local response_file = options.response_file
    assert(response_file == nil or is_path(response_file), "response_file must be a non-empty path")
    assert(response_file == nil or sink == nil, "response_file and response_sink are mutually exclusive")
    if method == nil then
        method = options.method
    end
    if method == nil then
        method = "GET"
    end

    local may_use_source = options.request_source ~= nil or options.multipart ~= nil
    local state = {
        body = options.body, -- 请求释放前保留 naettBody 借用的字符串。
        upload_progress = upload_progress,
        upload_total = upload_progress and ffi.new("uint64_t[1]"),
        download_progress = download_progress,
        download_total = download_progress and ffi.new("int64_t[1]"),
        sink = sink,
        sink_buffer = sink and ffi.new("uint8_t[?]", sink_capacity),
        response_file = response_file,
        source_capacity = may_use_source and ffi.new("int[1]"),
        source_attempt = may_use_source and ffi.new("uint64_t[1]"),
        attempts = may_use_source and {},
    }
    local file_error = response_file and ffi.new("int[1]")
    local handle, upload_error, sources = make_request(url, method, options, state.body)
    if handle == nil then
        if upload_error then
            return nil, upload_errors[upload_error] or "could not create streaming upload request", upload_error
        end
        return nil, "could not create request"
    end

    local response
    if response_file then
        response = lib.naettMakeWithResponseFile(handle, response_file, file_error)
    elseif sink then
        response = lib.naettMakeWithSink(handle)
    else
        response = lib.naettMakeCancellable(handle)
    end
    if response == nil then
        lib.naettFree(handle)
        if response_file then
            return nil, "could not open response file for writing", file_error[0]
        end
        return nil, sink and "could not start streaming response" or "could not start request", -5
    end
    state.handle, state.response, state.sources = handle, response, sources
    local task = setmetatable({ _pending = state }, Task)
    active_tasks[#active_tasks + 1] = task
    return task
end

local function notify_progress(state, callback, count, total)
    local ok, err = pcall(callback, tonumber(count), total >= 0 and tonumber(total) or nil)
    if not ok then
        stop_task(state, err, nil, true)
    end
end

local function finish_task(task, state)
    local response = state.response
    local result, err, code, thrown
    if state.failure then
        local failure = state.failure
        err, code, thrown = failure.reason, failure.code, failure.thrown
    else
        local ok
        ok, result, err, code = pcall(read_response, response, state.sink ~= nil or state.response_file ~= nil)
        if not ok then
            err, result, thrown = result, nil, true
        elseif result and state.response_file and result.status >= 200 and result.status < 300 then
            local status = lib.naettCommitResponseFile(response)
            if status ~= 0 then
                result, err, code = nil, "could not finalize response file", status
            end
        end
    end
    lib.naettClose(response)
    lib.naettFree(state.handle)
    task._result, task._error, task._code, task._thrown = result, err, code, thrown
    task._pending = nil
end

local function step_task(task, state)
    local response = state.response
    local complete = lib.naettComplete(response) ~= 0
    local worked = false
    if state.sources and not complete and not state.failure then
        local index = lib.naettGetUploadSourceRequest(response, state.source_capacity, state.source_attempt, 0)
        if index > 0 then
            local attempt = state.source_attempt[0]
            local ok, chunk = pcall(read_source, state.sources[index], state.attempts, index,
                attempt, state.source_capacity[0])
            if not ok then
                stop_task(state, chunk or "upload source failed", -3)
            elseif not state.failure then
                lib.naettSupplyUploadSource(response, attempt, chunk, chunk and #chunk or 0)
            end
            worked = true
        end
    end

    local chunk_size = 0
    if state.sink and not state.failure then
        chunk_size = lib.naettReadSink(response, state.sink_buffer, sink_capacity, 0)
    end
    if state.upload_progress and not state.failure then
        local sent = lib.naettGetUploadProgress(response, state.upload_total)
        if sent ~= state.last_sent then
            state.last_sent = sent
            notify_progress(state, state.upload_progress, sent, ffi.cast("int64_t", state.upload_total[0]))
            worked = true
        end
    end
    if state.download_progress and not state.failure then
        local received = lib.naettGetDownloadProgress(response, state.download_total)
        local total = state.download_total[0]
        if received ~= state.last_received or total ~= state.last_download_total then
            state.last_received, state.last_download_total = received, total
            notify_progress(state, state.download_progress, received, total)
            worked = true
        end
    end
    if chunk_size > 0 and not state.failure then
        local ok, result, err = pcall(deliver_chunk, state.sink, state.sink_buffer, chunk_size)
        if not ok then
            stop_task(state, result, nil, true)
        elseif result == false or (result == nil and err ~= nil) then
            stop_task(state, err or "response sink rejected a chunk", -4)
        end
        worked = true
    end
    if complete and (chunk_size == 0 or state.failure) then
        finish_task(task, state)
        return true, true
    end
    return worked, false
end

local function drive_tasks()
    local worked, runnable, completed = false, false, 0
    for i = 1, #active_tasks do
        local task = active_tasks[i]
        local state = task._pending
        if state and not state.busy then
            runnable = true
            state.busy = true
            local advanced, done = step_task(task, state)
            state.busy = nil
            worked = worked or advanced
            if done then
                completed = completed + 1
            end
        end
    end
    return worked, runnable, completed
end

local function poll_once(timeout)
    local version = lib.naettActivityVersion()
    local worked, runnable, completed = drive_tasks()
    if not worked and runnable and timeout > 0 then
        if has_activity_wait then
            lib.naettWaitForActivity(version, timeout)
        else
            ffi.C.usleep(math.min(timeout, 10) * 1000)
        end
        local _, _, after_wait = drive_tasks()
        completed = after_wait
    end
    return completed
end

local function poll(timeout)
    if timeout == nil then
        timeout = 0
    end
    assert(type(timeout) == "number" and timeout >= 0 and timeout <= 2147483647 and timeout % 1 == 0,
        "poll timeout must be a non-negative integer in milliseconds")
    poll_depth = poll_depth + 1
    local ok, completed = pcall(poll_once, timeout)
    poll_depth = poll_depth - 1
    if poll_depth == 0 then
        -- 回调可以发起同步请求并重入 poll，最外层退出时再整理列表。
        local count = 0
        for i = 1, #active_tasks do
            if active_tasks[i]._pending then
                count = count + 1
                active_tasks[count] = active_tasks[i]
            end
        end
        for i = #active_tasks, count + 1, -1 do
            active_tasks[i] = nil
        end
    end
    if not ok then
        error(completed, 0)
    end
    return completed
end

local function request(...)
    local task, err, code = request_async(...)
    if not task then
        return nil, err, code
    end
    while not task:done() do
        poll(50)
    end
    return task:result()
end

return { request = request, request_async = request_async, poll = poll }
