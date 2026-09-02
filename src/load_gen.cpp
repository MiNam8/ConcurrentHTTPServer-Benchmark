#include <iostream>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <cstring>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <chrono>
#include <algorithm>
#include <latch>
#include <string>
#include <cctype>

#define PORT 8080


std::latch gate(1);
int NUMBER_OF_THREADS = 50;
int ITERATIONS = 100000 / NUMBER_OF_THREADS;
bool KEEP_ALIVE = false;

static int connect_to_server()
{
    int client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0) { return -1; }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    address.sin_port = htons(PORT);

    if (connect(client, (const sockaddr *)&address, sizeof(address)) < 0)
    {
        close(client);
        return -1;
    }
    return client;
}

static bool write_all(int fd, const char* data, size_t len)
{
    size_t written = 0;
    while (written < len)
    {
        ssize_t w = write(fd, data + written, len - written);
        if (w <= 0) { return false; }
        written += static_cast<size_t>(w);
    }
    return true;
}

static bool read_one_response(int fd, std::string& buf)
{
    size_t header_end;
    while ((header_end = buf.find("\r\n\r\n")) == std::string::npos)
    {
        char tmp[4096];
        ssize_t k = read(fd, tmp, sizeof(tmp));
        if (k <= 0) { return false; }
        buf.append(tmp, static_cast<size_t>(k));
    }

    std::string headers = buf.substr(0, header_end);
    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    size_t content_length = 0;
    size_t pos = headers.find("content-length:");
    if (pos != std::string::npos)
    {
        content_length = std::strtoul(headers.c_str() + pos + 15, nullptr, 10);
    }

    size_t total = header_end + 4 + content_length;
    while (buf.size() < total)
    {
        char tmp[4096];
        ssize_t k = read(fd, tmp, sizeof(tmp));
        if (k <= 0) { return false; }
        buf.append(tmp, static_cast<size_t>(k));
    }

    buf.erase(0, total);
    return true;
}

// Keep-alive worker: connect once, then loop ITERATIONS requests on the same
// connection, timing each request/response round-trip individually.
void keep_alive_worker(std::vector<long long> &latencies)
{
    int client = connect_to_server();
    if (client < 0) { return; }

    gate.wait();   // fair start: all connections established, released together

    // HTTP/1.1 defaults to keep-alive, so no "Connection: close".
    const char* request = "GET / HTTP/1.1\r\nHost: localhost:8080\r\n\r\n";
    size_t request_len = strlen(request);
    std::string buf;   // persistent per-connection response accumulator

    for (int i = 0; i < ITERATIONS; i++)
    {
        auto start_time = std::chrono::steady_clock::now();

        if (!write_all(client, request, request_len)) { close(client); return; }
        if (!read_one_response(client, buf))          { close(client); return; }

        auto end_time = std::chrono::steady_clock::now();
        latencies.push_back(
            std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());
    }
    close(client);
}

void do_one_request(std::vector<long long> &latencies)
{
    int cnt = 0;
    while (cnt < ITERATIONS)
    {
        cnt++;
        int client;
        struct sockaddr_in address;

        client = socket(AF_INET, SOCK_STREAM, 0);
        if (client < 0)
        {
            perror("Client socket failed");
            // exit(EXIT_FAILURE);
            // return;
            continue;
        }
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        address.sin_port = htons(PORT);

        if (connect(client, (const sockaddr *)&address, sizeof(address)) < 0)
        {
            perror("Connect failed");
            close(client);
            // exit(EXIT_FAILURE);
            // return;
            continue;
        }
        gate.wait();
        auto start_time = std::chrono::steady_clock::now();
        const char* request = "GET / HTTP/1.1\r\nHOST: localhost:8080\r\nConnection: close\r\n\r\n";

        ssize_t bytesWritten = 0;
        bool is_return = false;
        do
        {
            ssize_t write_result = write(client, request + bytesWritten, strlen(request) - bytesWritten);
            if (write_result == -1)
            {
                perror("Write returned -1");
                close(client);
                // return;
                is_return = true;
                break;
            }
            bytesWritten += write_result;

        } while (bytesWritten < strlen(request));
        if (is_return)
        {
            continue;
        }
        char buffer[1024] = {0};
        ssize_t response = read(client, buffer, 1024);
        if (response <= 0)
        {
            close(client);
            // return;
            continue;
        }
        // printf("Received response: \n%s", buffer);

        auto end_time = std::chrono::steady_clock::now();
        latencies.push_back(std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());
        close(client);
    }
}

int main(int argc, char* argv[])
{
    if (argc != 5 && argc != 6)
    {
        std::cout << "Usage: ./LoadGenerator --concurrency 50 --requests 100000 [--keep-alive]";
        return 0;
    }
    if (argc == 6)
    {
        std::string argv5 = argv[5];
        if (argv5 != "--keep-alive")
        {
            std::cout << "The 6th argument, if given, has to be --keep-alive";
            return 0;
        }
        KEEP_ALIVE = true;
    }

    std::string argv1 = argv[1];
    if (argv1 != "--concurrency")
    {
        std::cout << "The 4th argument has to be --concurrency";
        return 0;
    }

    try
    {
        std::string argv2 = argv[2];
        NUMBER_OF_THREADS = stoi(argv2);
    }
    catch(const std::exception& e)
    {
        std::cout << "Failed to parse the argument of --concurrency";
        std::cerr << e.what() << '\n';
        return 0;
    }
    
    std::string argv3 = argv[3];
    if (argv3 != "--requests")
    {
        std::cout << "The 4th argument has to be --requests";
        return 0;
    }

    try
    {
        std::string argv4 = argv[4];
        ITERATIONS = int(stoi(argv4) / NUMBER_OF_THREADS);
    }
    catch(const std::exception& e)
    {
        std::cout << "Failed to parse the argument of --requests";
        std::cerr << e.what() << '\n';
        return 0;
    }
    std::vector<std::vector<long long> > results(NUMBER_OF_THREADS);
    std::vector<std::thread> vec;
    auto worker = KEEP_ALIVE ? keep_alive_worker : do_one_request;
    for (int i = 0; i < NUMBER_OF_THREADS; i++)
    {
        vec.push_back(std::thread(worker, std::ref(results[i])));
    }
    auto t0 = std::chrono::steady_clock::now();
    gate.count_down();
    for (int i = 0; i < vec.size(); i++)
    {
        vec[i].join();
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = (std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1e6;

    std::vector<long long> all;
    for (int i = 0; i < NUMBER_OF_THREADS; i++)
    {
        all.insert(all.end(), results[i].begin(), results[i].end());
    }
    std::sort(all.begin(), all.end());
    size_t n = all.size();

    long long attempted = (long long)NUMBER_OF_THREADS * ITERATIONS;
    if (n == 0)
    {
        std::printf("no successful requests (%lld attempted, all failed)\n", attempted);
        return 1;
    }

    long long p50 = all[n * 50 / 100];
    long long p95 = all[n * 95 / 100];
    long long p99 = all[n * 99 / 100];
    long long min = all.front();
    long long max = all.back();

    long long succeeded = n;
    long long failed    = attempted - succeeded;
    double throughput   = succeeded / secs;   // secs from the wall-clock timer

    std::printf("Requests:   %lld attempted, %lld ok, %lld failed\n",
            attempted, succeeded, failed);
    std::printf("Throughput: %.0f req/s\n", throughput);
    std::printf("Latency (us): min %lld  p50 %lld  p95 %lld  p99 %lld  max %lld\n",
                min, p50, p95, p99, max);

    // Machine-readable line for the sweep script (prefix "CSV," so it can be grep'd).
    // Columns: concurrency,attempted,ok,failed,throughput,p50,p95,p99,max
    std::printf("CSV,%d,%lld,%lld,%lld,%.0f,%lld,%lld,%lld,%lld\n",
                NUMBER_OF_THREADS, attempted, succeeded, failed, throughput,
                p50, p95, p99, max);

    return 0;
}