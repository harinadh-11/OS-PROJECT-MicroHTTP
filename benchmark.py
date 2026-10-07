#!/usr/bin/env python3

import socket
import time
from concurrent.futures import ThreadPoolExecutor

HOST = "127.0.0.1"
PORT = 8080
TOTAL_REQUESTS = 1000
CONCURRENCY = 20

REQUEST = (
    b"GET / HTTP/1.1\r\n"
    b"Host: localhost\r\n"
    b"Connection: close\r\n"
    b"\r\n"
)


def request_once():
    start = time.perf_counter()

    try:
        with socket.create_connection((HOST, PORT), timeout=5) as sock:
            sock.sendall(REQUEST)

            data = b""
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                data += chunk

        latency = time.perf_counter() - start
        success = data.startswith(b"HTTP/1.1 200")
        return success, latency

    except Exception:
        latency = time.perf_counter() - start
        return False, latency


def percentile(values, p):
    values = sorted(values)
    if not values:
        return 0.0

    index = (len(values) - 1) * p / 100
    lower = int(index)
    upper = min(lower + 1, len(values) - 1)

    if lower == upper:
        return values[lower]

    fraction = index - lower
    return values[lower] + (values[upper] - values[lower]) * fraction


def main():
    print("MicroHTTP benchmark")
    print(f"Requests: {TOTAL_REQUESTS}")
    print(f"Concurrency: {CONCURRENCY}")
    print()

    overall_start = time.perf_counter()

    with ThreadPoolExecutor(max_workers=CONCURRENCY) as executor:
        results = list(
            executor.map(
                lambda _: request_once(),
                range(TOTAL_REQUESTS)
            )
        )

    total_time = time.perf_counter() - overall_start

    successful = sum(1 for success, _ in results if success)
    failed = TOTAL_REQUESTS - successful

    latencies = [latency for _, latency in results]

    avg = sum(latencies) / len(latencies)
    p50 = percentile(latencies, 50)
    p99 = percentile(latencies, 99)
    rps = TOTAL_REQUESTS / total_time

    print(f"Successful 200: {successful}")
    print(f"Failed: {failed}")
    print(f"Time: {total_time:.3f} s")
    print(f"Requests/sec: {rps:.2f}")
    print(f"Average latency: {avg * 1000:.2f} ms")
    print(f"p50 latency: {p50 * 1000:.2f} ms")
    print(f"p99 latency: {p99 * 1000:.2f} ms")

    if successful == TOTAL_REQUESTS:
        print()
        print("PASS: All benchmark requests returned HTTP 200.")
    else:
        print()
        print("FAIL: Some benchmark requests failed.")


if __name__ == "__main__":
    main()
