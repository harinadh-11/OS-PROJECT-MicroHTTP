# MicroHTTP

MicroHTTP is a small educational HTTP/1.1 static file server written in C11 for Linux. It serves files relative to the current working directory and maps `/` to `index.html`. It supports GET requests and a deliberately small subset of HTTP.

## Architecture

The main thread creates a TCP socket, binds port 8080, listens, and accepts clients. Accepted client sockets enter a bounded 64-entry queue. Four long-lived POSIX pthread workers remove sockets from the queue and serve them. The queue mutex protects its indices and count; condition variables wake workers when jobs arrive and the accept thread when space becomes available.

Workers parse HTTP/1.1 request lines and headers, read GET requests, and send responses using `send()`. HTTP/1.1 connections stay open by default. `Connection: close` closes after the current response; `Connection: keep-alive` explicitly keeps the connection available for additional sequential requests. Each socket has a 15-second receive idle timeout, after which the worker closes it. Requests with a non-empty body or Transfer-Encoding are rejected because this server only supports bodyless GET requests.

Files are opened relative to the configured document root using `openat()` and `O_NOFOLLOW` for every path component, preventing traversal and symlink escape. URL escapes are decoded before path validation; `.` and `..` components and backslashes are rejected. Regular files receive accurate Content-Length and a MIME type based on extension: HTML, CSS, JavaScript, text, JSON, PNG, JPEG, GIF, and SVG are recognized; other files use `application/octet-stream`.

SIGINT and SIGTERM stop the accept loop. The server stops queue submissions, drains queued sockets, allows in-flight workers to finish their current response, joins the workers, and destroys the queue synchronization objects. Idle workers exit after the idle timeout if they are waiting on a keep-alive connection.

## Compile

On Linux with GCC and pthread support:

```sh
gcc -Wall -Wextra -g -pthread -o server server.c
```

Or run `make`.

## Run

Run from the directory containing the files to serve:

```sh
./server
```

The server listens on TCP port 8080. Press Ctrl+C or send SIGTERM for graceful shutdown.

## Tests

Basic response and MIME type:

```sh
curl -i http://localhost:8080/
curl -i http://localhost:8080/style.css
curl -i http://localhost:8080/data.json
curl -i http://localhost:8080/not-found.html
```

Keep one connection open and make two sequential requests (requires curl with `--next`):

```sh
curl -i --http1.1 http://localhost:8080/ http://localhost:8080/index.html
```

To inspect persistent reuse interactively, use netcat and type both requests before waiting for responses:

```sh
printf 'GET / HTTP/1.1\r\nHost: localhost\r\n\r\nGET /index.html HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' | nc 127.0.0.1 8080
```

Idle timeout: connect and send no request; the server closes the connection after 15 seconds:

```sh
nc 127.0.0.1 8080
```

Traversal protection (curl's `--path-as-is` prevents client-side normalization):

```sh
curl --path-as-is -i http://localhost:8080/../etc/passwd
curl --path-as-is -i http://localhost:8080/%2e%2e/etc/passwd
```

Concurrent clients:

```sh
seq 1 20 | xargs -P20 -I{} curl -s -o /dev/null -w '%{http_code}\n' http://localhost:8080/
```

Graceful shutdown from another terminal:

```sh
kill -TERM "$(pgrep -n server)"
```

The server prints a shutdown message, drains queued clients, waits for workers, and exits. Each accepted socket has one owner: the queue/worker lifecycle closes it exactly once.

## Limitations

This is an educational server, not a production HTTP implementation. It only handles GET, requires HTTP/1.1, does not support request bodies, chunked transfer encoding, TLS, virtual hosts, range requests, caching, or configurable ports. It uses a single 15-second receive timeout and serves files from the process working directory. Shutdown waits for queued work and active clients; a worker blocked on an idle persistent connection may take up to the timeout to exit.

