# MicroHTTP Design Document

## 1. Project Overview

MicroHTTP is a concurrent HTTP/1.1 static web server implemented in C11 for Linux.

The server accepts TCP connections on port 8080, parses HTTP requests, safely resolves requested files, and returns static content with appropriate HTTP status codes and headers.

## 2. Concurrency Model

MicroHTTP uses a fixed-size POSIX pthread thread pool.

- 4 worker threads are created at startup.
- The main thread owns the listening socket and accepts incoming TCP connections.
- Accepted client sockets are placed into a bounded queue.
- Worker threads wait on a condition variable for queued connections.
- A mutex protects queue state and shutdown state.
- Each worker removes a client socket from the queue and handles its HTTP connection.

A bounded queue provides basic backpressure when many clients connect simultaneously.

The project deliberately uses a thread-pool design instead of epoll because the course specification permits either model and the thread-pool approach provides a straightforward concurrency model for this educational implementation.

## 3. Request Processing

The processing flow is:

1. Create TCP listening socket.
2. Enable `SO_REUSEADDR`.
3. Bind to port 8080.
4. Listen for incoming connections.
5. Accept a client connection.
6. Place the client socket in the worker queue.
7. A worker reads the HTTP request.
8. The request line and relevant headers are parsed.
9. The requested path is validated.
10. The requested file is opened safely.
11. HTTP response headers are generated.
12. The file contents are sent to the client.
13. The connection is kept alive when requested.
14. Idle connections are closed after the timeout.
15. The client descriptor is closed exactly once.

## 4. HTTP/1.1 Support

The server supports GET requests and HTTP/1.1 connection management.

Supported behavior includes:

- HTTP/1.1 request parsing
- GET requests
- `Connection: keep-alive`
- `Connection: close`
- Multiple sequential requests on a persistent connection
- `Content-Length`
- Appropriate `Content-Type`
- 200 OK
- 400 Bad Request
- 404 Not Found

Malformed requests are rejected with HTTP 400.

## 5. Static File Resolution

The document root is the MicroHTTP project directory.

The root URL `/` maps to:

    index.html

Requested files are resolved relative to the document root.

Directory traversal is prevented by rejecting unsafe path components and encoded traversal attempts such as `..`.

Backslash-based traversal is also rejected.

The implementation uses safe file-opening behavior to avoid following unsafe symbolic links.

## 6. MIME Type Handling

The server determines the response MIME type from the file extension.

Supported mappings include:

| Extension | Content-Type |
|---|---|
| `.html` | `text/html; charset=utf-8` |
| `.htm` | `text/html; charset=utf-8` |
| `.css` | `text/css; charset=utf-8` |
| `.js` | `application/javascript` |
| `.txt` | `text/plain; charset=utf-8` |
| `.json` | `application/json` |
| `.png` | `image/png` |
| `.jpg` / `.jpeg` | `image/jpeg` |
| `.gif` | `image/gif` |
| `.svg` | `image/svg+xml` |
| unknown | `application/octet-stream` |

The response includes an accurate `Content-Length`.

## 7. Connection Lifecycle

Connections have a receive/send timeout of approximately 15 seconds.

This prevents abandoned clients from occupying file descriptors indefinitely.

For keep-alive connections, the worker can process multiple sequential HTTP requests on the same TCP connection.

When a connection must close, the responsible worker closes the client descriptor.

The design ensures that ownership of the client descriptor remains clear and that the same descriptor is not closed by multiple components.

## 8. Graceful Shutdown

The server handles SIGINT and SIGTERM.

During shutdown:

1. The server stops accepting new connections.
2. The listening socket is closed.
3. The shutdown state is signaled to worker threads.
4. Queued work is drained where practical.
5. Workers finish their current work.
6. Worker threads are joined.
7. Synchronization resources are cleaned up.
8. The process exits cleanly.

Example shutdown message:

    Stopping: draining queued clients and waiting for workers.

## 9. Error Handling

The server handles:

- malformed HTTP requests
- unsupported request formats
- missing files
- unsafe paths
- socket errors
- connection timeouts
- interrupted system calls where appropriate

HTTP errors return a small plain-text response with the corresponding status code.

## 10. Resource Management

The implementation uses:

- TCP sockets
- pthread worker threads
- mutexes
- condition variables
- bounded client queue
- file descriptors

Resources are cleaned up during normal operation and graceful shutdown.

The worker pool is joined before process termination.

## 11. Benchmark Methodology

The benchmark uses `benchmark.py`.

Test configuration:

- Total requests: 1,000
- Client concurrency: 20
- HTTP method: GET
- Target: `/`
- HTTP version: HTTP/1.1
- Connection: close
- Server workers: 4

The benchmark measures:

- total execution time
- successful responses
- failed responses
- requests per second
- average latency
- p50 latency
- p99 latency

Latest recorded benchmark:

| Metric | Result |
|---|---:|
| Requests | 1,000 |
| Concurrency | 20 |
| Successful | 1,000 |
| Failed | 0 |
| Time | 0.324 s |
| Requests/sec | 3,082.09 |
| Average latency | 6.22 ms |
| p50 latency | 6.01 ms |
| p99 latency | 13.04 ms |

All 1,000 requests returned HTTP 200 in this run.

## 12. Single-Worker Baseline

A separate baseline run was performed using one worker with the same benchmark workload.

Recorded baseline:

| Metric | 1 Worker |
|---|---:|
| Requests | 1,000 |
| Concurrency | 20 |
| Successful | 1,000 |
| Failed | 0 |
| Time | 0.381 s |
| Requests/sec | 2,624.72 |
| Average latency | 5.61 ms |
| p50 latency | 5.43 ms |
| p99 latency | 10.30 ms |

The four-worker and one-worker results demonstrate the measured behavior of the implementation under this local test. These results should not be interpreted as universal performance guarantees.

## 13. Correctness Tests Performed

The server was tested for:

- normal HTTP GET
- HTTP/1.1 keep-alive
- multiple requests on one TCP connection
- 15-second idle timeout
- HTML MIME type
- CSS MIME type
- JSON MIME type
- directory traversal attempt
- malformed HTTP request
- 20 concurrent clients
- graceful Ctrl+C shutdown

All tested cases behaved as expected.

## 14. Known Limitations

The current implementation does not include:

- TLS/HTTPS
- HTTP chunked request bodies
- Range requests
- `If-Modified-Since` conditional GET
- CGI handlers
- epoll-based event loop
- HTTP/2

These are outside the implemented core feature set or are optional/stretch features in the project specification.

## 15. Build and Run

Build:

    make clean
    make

Run:

    ./server

The server listens on:

    http://localhost:8080/

Basic test:

    curl.exe -i http://localhost:8080/

Benchmark:

    python3 benchmark.py
