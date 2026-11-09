#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <csignal>
#include <chrono>
#include <vector>
#include <fd.hpp>


enum class ConnState{ Reading, Working, Writing };

struct Connection {
    Fd fd;
    uint64_t id = 0;             // unique per accept(); fd numbers get reused, ids never do
    std::string in;
    std::string out;
    size_t sent = 0;
    ConnState state = ConnState::Reading;
    bool keep_alive = true;
    off_t file_off = 0;          // Phase 5: sendfile() offset for this connection
    bool sending_file = false;   // Phase 5: mid-sendfile (body not yet fully sent)
    std::chrono::steady_clock::time_point last_activity;   // Phase 7: for idle-timeout reaping
};

enum class ParseResult { NeedMore, Complete };

// Pragmatic HTTP/1.1 request parser, sufficient for the benchmark's request
// shape (simple GETs, no body). Examines c.in:
//   - if the header block isn't fully received yet (no "\r\n\r\n") -> NeedMore
//   - otherwise: set c.keep_alive from the Connection header (HTTP/1.1 defaults
//     to keep-alive; only "Connection: close" turns it off), consume the request
//     bytes from c.in so a pipelined request behind it survives, -> Complete.
static ParseResult parse_request(Connection& c)
{
    size_t end = c.in.find("\r\n\r\n");
    if (end == std::string::npos)
    {
        return ParseResult::NeedMore;
    }

    std::string headers = c.in.substr(0, end);
    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    c.keep_alive = (headers.find("connection: close") == std::string::npos);

    c.in.erase(0, end + 4);
    return ParseResult::Complete;
}


#define PORT 8080
std::string flag = "none";
volatile uint64_t sink;
std::queue <int> connection_queue;
std::mutex m;
std::condition_variable cv;
std::unordered_map <int, std::string> buffers;   // fd → bytes accumulated so far

// --- hybrid arch: epoll I/O thread <-> CPU worker pool ---
// Anything that outlives a connection (worker task, timer) carries fd + id, so a
// result for a closed connection can't land on a new one that reused the fd.
struct ConnRef
{
    int fd;
    uint64_t id;
};

struct Completion
{
    int fd;
    uint64_t id;
    std::string response;
};

std::queue<ConnRef>      task_queue;
std::mutex               task_m;
std::condition_variable  task_cv;

std::queue<Completion>   completion_queue;
std::mutex               completion_m;

int hybrid_eventfd = -1;

// --- Phase 5: static-file serving (read()+write() vs zero-copy sendfile()) ---
int         g_file_fd = -1;        // open read-only file; -1 = not in file mode
off_t       g_file_size = 0;
bool        g_sendfile = false;    // true: sendfile(); false: read()+write()
std::string g_file_headers;        // cached response headers (with Content-Length)
std::string g_readwrite_body;      // whole file in userspace (read+write mode only)

// --- Phase 7: hardening (both opt-in; 0 = disabled -> default benchmark behavior) ---
int    g_idle_timeout_sec = 0;     // --idle-timeout: reap connections idle this long (Slowloris fix)
size_t g_max_conns = 0;            // --max-conns: shed new connections beyond this cap (backpressure)

void write_response(int client)
{
    const char *response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 12\r\n\r\nHello World!";
    size_t sent = 0;
    while (sent < strlen(response))
    {
        ssize_t n = write(client, response + sent, strlen(response) - sent);
        if (n <= 0 && errno != EAGAIN)
        {
            break;
        }
        if (n > 0)
        {
            sent += n;
        }
    }
    // printf("Response sent successfully.\n");
}
void respond(int client)
{
    if (flag == "sleep")
    {
        usleep(100000);
    }
    else if (flag == "cpu")
    {
        uint64_t x = 88172645463325252ULL;
        int ITERATIONS = 1000000;
        for (long i = 0; i < ITERATIONS; i++)
        {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
        }
        sink = x;
    }

    write_response(client);   // write only; the caller owns the connection lifecycle
}

// Serve one connection to completion. With keep-alive (HTTP/1.1 default) this
// loops, serving many sequential requests on the same socket until the client
// sends "Connection: close" or closes the connection (EOF). This mirrors the
// epoll arch so all architectures are comparable under the keep-alive load.
void handle_connection(int client)
{
    if (client < 0) {
        perror("Accept failed");
        return;
    }

    std::string in;   // bytes accumulated across reads (may span/batch requests)
    while (true)
    {
        // Read until a full request header block (\r\n\r\n) is buffered.
        size_t header_end;
        while ((header_end = in.find("\r\n\r\n")) == std::string::npos)
        {
            char buffer[1024];
            ssize_t bytes_read = read(client, buffer, sizeof(buffer));
            if (bytes_read <= 0) { close(client); return; }   // EOF or error
            in.append(buffer, static_cast<size_t>(bytes_read));
        }

        // Honor "Connection: close" (case-insensitive); HTTP/1.1 else keeps alive.
        std::string headers = in.substr(0, header_end);
        std::transform(headers.begin(), headers.end(), headers.begin(),
                       [](unsigned char ch) { return std::tolower(ch); });
        bool keep_alive = (headers.find("connection: close") == std::string::npos);

        // Consume this request (no body in this benchmark), leaving any pipelined
        // bytes behind for the next loop iteration.
        in.erase(0, header_end + 4);

        respond(client);

        if (!keep_alive) { close(client); return; }
    }
}

void worker_pool()
{
    while (true)
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, []{return !connection_queue.empty();});

        int client = connection_queue.front();
        connection_queue.pop();
        lock.unlock();

        handle_connection(client);
    }
}

void hybrid_worker()
{
    while (true)
    {
        ConnRef task;
        {
            std::unique_lock<std::mutex> lock(task_m);
            task_cv.wait(lock, []{ return !task_queue.empty(); });
            task = task_queue.front();
            task_queue.pop();
        }   // <-- lock destructor unlocks task_m here, before the CPU work runs

        // 1. burn the CPU (the xorshift loop from build_response)
        if (flag == "cpu")
        {
            uint64_t x = 88172645463325252ULL;
            for (long i = 0; i < 1000000; i++)
            {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
            }
            sink = x;
        }

        // 2. build the response string (same literal as build_response's c.out)
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 12\r\n\r\nHello World!";

        {
            std::lock_guard<std::mutex> lock(completion_m);
            completion_queue.push({task.fd, task.id, resp});
        }

        uint64_t one = 1;
        ssize_t w = write(hybrid_eventfd, &one, sizeof(one));
        (void) w;
    }
}



static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Disable Nagle's algorithm: send small segments immediately instead of holding
// them for an ACK. Without this, a response split across two syscalls (write
// headers + sendfile body) stalls ~40 ms on the Nagle/delayed-ACK interaction.
static void set_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static void build_response(Connection& c)
{
    if (g_file_fd >= 0)                        // Phase 5: static-file mode
    {
        c.out = g_file_headers;                // headers always go through the buffer
        c.sent = 0;
        if (g_sendfile)
        {
            c.sending_file = true;             // body streamed via sendfile()
            c.file_off = 0;
        }
        else
        {
            c.out += g_readwrite_body;         // body copied through userspace (read+write)
            c.sending_file = false;
        }
        return;
    }

    if (flag == "cpu")
    {
        uint64_t x = 88172645463325252ULL;
        for (long i = 0; i < 1000000; i++)
        {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
        }
        sink = x;
    }
    c.out = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 12\r\n\r\nHello World!";
    c.sent = 0;
}

// Write as much of c.out as the socket accepts, resuming from c.sent.
enum class FlushResult { Done, WouldBlock, Error };
static FlushResult flush_out(Connection& c)
{
    while (c.sent < c.out.size())
    {
        ssize_t n = write(c.fd.get(), c.out.data() + c.sent, c.out.size() - c.sent);
        if (n > 0)
        {
            c.sent += static_cast<size_t>(n);
        }
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return FlushResult::WouldBlock;   // socket send buffer full; wait for EPOLLOUT
        }
        else
        {
            return FlushResult::Error;        // n == 0 or a real error
        }
    }
    return FlushResult::Done;
}

// Flush a response that may be a static file. First drains c.out (headers, or
// the whole read()+write() body). Then, if this connection streams a file via
// sendfile(), pushes file bytes straight from the page cache to the socket
// (zero-copy) resuming from c.file_off. Both stages yield WouldBlock on a full
// socket buffer, so the EPOLLOUT path resumes exactly where it stopped. Passing
// &c.file_off (non-NULL) means the file's own offset is untouched, so many
// connections can stream the same shared fd concurrently.
static FlushResult flush_response(Connection& c)
{
    FlushResult hr = flush_out(c);            // headers / buffered body first
    if (hr != FlushResult::Done) { return hr; }

    if (c.sending_file)
    {
        while (c.file_off < g_file_size)
        {
            ssize_t n = sendfile(c.fd.get(), g_file_fd, &c.file_off,
                                 static_cast<size_t>(g_file_size - c.file_off));
            if (n > 0) { continue; }          // kernel advanced c.file_off
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                return FlushResult::WouldBlock;
            }
            return FlushResult::Error;
        }
        c.sending_file = false;               // whole file sent
    }
    return FlushResult::Done;
}

// Create a listening socket bound to PORT. `reuseport` sets SO_REUSEPORT so that
// N sockets can bind the same port and the kernel load-balances connections
// across them (the epoll-mt design). Returned socket is still blocking; the
// event loop makes it non-blocking itself.
static Fd make_listener(bool reuseport)
{
    Fd listener{socket(AF_INET, SOCK_STREAM, 0)};
    if (!listener.valid())
    {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (reuseport)
    {
        // Must be set BEFORE bind; every socket sharing the port must set it too.
        setsockopt(listener.get(), SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }

    struct sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(listener.get(), (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }
    if (listen(listener.get(), SOMAXCONN) < 0)
    {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    return listener;
}

// One complete epoll event loop: owns `listener` and every connection accepted
// on it, running Phase-1's non-blocking HTTP/1.1 state machine. All state here
// is LOCAL, so N of these can run on N threads with zero shared connection
// state and no hot-path locking (Phase 3, --arch=epoll-mt).
//   `hybrid` (Phase 2, single loop): also drain the CPU worker pool via the
//   eventfd registered below and write results through the EPOLLOUT path.
static void run_epoll_loop(Fd listener, bool hybrid)
{
    set_nonblocking(listener.get());

    int ep = epoll_create1(0);
    struct epoll_event ev{};
    ev.data.fd = listener.get();
    ev.events = EPOLLIN;
    epoll_ctl(ep, EPOLL_CTL_ADD, listener.get(), &ev);

    if (hybrid)
    {
        hybrid_eventfd = eventfd(0, 0);
        set_nonblocking(hybrid_eventfd);
        struct epoll_event xev{};
        xev.data.fd = hybrid_eventfd;
        xev.events = EPOLLIN;
        epoll_ctl(ep, EPOLL_CTL_ADD, hybrid_eventfd, &xev);
    }

    std::unordered_map<int, Connection> conns;      // client fd -> connection state
    std::unordered_map<int, ConnRef> timer_to_client;   // per-loop: sleep-mode timers
    uint64_t next_conn_id = 0;                      // per-loop, only this thread touches it
    struct epoll_event events[64];

    // Close and forget a connection (Connection dtor -> Fd dtor -> close(fd)).
    auto close_conn = [&](int fd) {
        epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
        conns.erase(fd);
    };
    // Switch a connection's epoll interest (EPOLLIN vs EPOLLOUT).
    auto mod_events = [&](int fd, uint32_t mask) {
        struct epoll_event mev{};
        mev.data.fd = fd;
        mev.events = mask;
        epoll_ctl(ep, EPOLL_CTL_MOD, fd, &mev);
    };
    // After a full response: keep-alive -> read the next request; else close.
    auto finish_response = [&](int fd, Connection& c) {
        if (c.keep_alive)
        {
            c.out.clear();
            c.sent = 0;
            c.state = ConnState::Reading;
            mod_events(fd, EPOLLIN);
        }
        else
        {
            close_conn(fd);
        }
    };

    // Phase 7: with idle timeouts on, wake at least every second to sweep for
    // connections that have gone quiet (Slowloris). Otherwise block indefinitely.
    int wait_ms = (g_idle_timeout_sec > 0) ? 1000 : -1;
    auto last_sweep = std::chrono::steady_clock::now();

    while (true)
    {
        int n = epoll_wait(ep, events, 64, wait_ms);
        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;
            uint32_t evmask = events[i].events;

            // (a) a sleep-mode timer fired: build the deferred response and send it.
            if (timer_to_client.count(fd))
            {
                uint64_t exp;
                ssize_t r = read(fd, &exp, sizeof(exp));   // drain the timerfd
                (void)r;
                ConnRef ref = timer_to_client[fd];
                int cfd = ref.fd;
                close(fd);
                timer_to_client.erase(fd);

                auto it = conns.find(cfd);
                if (it == conns.end()) { continue; }       // client already gone
                if (it->second.id != ref.id) { continue; } // fd reused by a new client: stale timer
                Connection& c = it->second;
                build_response(c);                          // sleep already "waited"
                c.state = ConnState::Writing;
                FlushResult fr = flush_response(c);
                if (fr == FlushResult::Done)            { finish_response(cfd, c); }
                else if (fr == FlushResult::WouldBlock) { mod_events(cfd, EPOLLOUT); }
                else                                    { close_conn(cfd); }
                continue;
            }

            // (a2) hybrid: a worker signalled the eventfd. Drain every finished
            // response and write it out via the Phase-1 EPOLLOUT path.
            if (hybrid && fd == hybrid_eventfd)
            {
                uint64_t cnt;
                ssize_t r = read(hybrid_eventfd, &cnt, sizeof(cnt));   // clear the counter
                (void)r;

                // Take the whole queue under the lock, then process it unlocked
                // so workers keep flowing while we write responses.
                std::queue<Completion> done;
                {
                    std::lock_guard<std::mutex> lock(completion_m);
                    std::swap(done, completion_queue);
                }

                while (!done.empty())
                {
                    Completion comp = done.front();
                    done.pop();

                    auto it = conns.find(comp.fd);
                    if (it == conns.end()) { continue; }   // connection gone: drop the result
                    // fd was closed and reused by a new accept() while the worker computed:
                    // this result belongs to the old client, so drop it.
                    if (it->second.id != comp.id) { continue; }
                    Connection& c = it->second;

                    c.out = comp.response;
                    c.sent = 0;
                    c.state = ConnState::Writing;
                    mod_events(comp.fd, EPOLLIN);          // re-enable the interest parked at offload
                    FlushResult fr = flush_response(c);
                    if (fr == FlushResult::WouldBlock) { mod_events(comp.fd, EPOLLOUT); }
                    else if (fr == FlushResult::Error) { close_conn(comp.fd); }
                    else                               { finish_response(comp.fd, c); }
                }
                continue;
            }

            // (b) listener readable: accept every pending connection.
            if (fd == listener.get())
            {
                while (true)
                {
                    int cfd = accept(listener.get(), nullptr, nullptr);
                    if (cfd < 0) { break; }                 // EAGAIN: backlog drained
                    // Phase 7: backpressure -- shed the connection if at the cap.
                    if (g_max_conns > 0 && conns.size() >= g_max_conns)
                    {
                        close(cfd);
                        continue;
                    }
                    set_nonblocking(cfd);
                    set_nodelay(cfd);
                    struct epoll_event cev{};
                    cev.data.fd = cfd;
                    cev.events = EPOLLIN;
                    epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev);
                    auto it = conns.try_emplace(cfd).first; // default Connection
                    it->second.fd = Fd{cfd};                // take ownership of cfd
                    it->second.id = ++next_conn_id;
                    it->second.last_activity = std::chrono::steady_clock::now();
                }
                continue;
            }

            // (c) an existing client connection.
            auto it = conns.find(fd);
            if (it == conns.end()) { continue; }
            Connection& c = it->second;
            c.last_activity = std::chrono::steady_clock::now();   // Phase 7: mark active

            if (evmask & (EPOLLHUP | EPOLLERR))
            {
                close_conn(fd);
                continue;
            }

            // (c1) readable: accumulate bytes, then parse one request.
            if (c.state == ConnState::Reading && (evmask & EPOLLIN))
            {
                char tmp[4096];
                bool peer_closed = false;
                while (true)
                {
                    ssize_t k = read(fd, tmp, sizeof(tmp));
                    if (k > 0)        { c.in.append(tmp, static_cast<size_t>(k)); }
                    else if (k == 0)  { peer_closed = true; break; }   // clean EOF
                    else if (errno == EAGAIN || errno == EWOULDBLOCK) { break; }
                    else              { peer_closed = true; break; }   // read error
                }
                if (peer_closed) { close_conn(fd); continue; }

                // Drain every complete request already buffered. A single read() can
                // pull multiple pipelined requests out of the kernel socket buffer into
                // c.in at once; once drained, no further EPOLLIN fires for them, so they
                // must all be parsed here rather than waiting for another epoll wakeup.
                while (parse_request(c) == ParseResult::Complete)
                {
                    if (flag == "sleep")
                    {
                        // Defer 100 ms without blocking the loop: arm a timerfd.
                        c.state = ConnState::Working;
                        int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
                        timer_to_client[tfd] = {fd, c.id};
                        struct epoll_event tev{};
                        tev.data.fd = tfd;
                        tev.events = EPOLLIN;
                        epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &tev);
                        struct itimerspec ts{};
                        ts.it_value.tv_nsec = 100 * 1000 * 1000;
                        timerfd_settime(tfd, 0, &ts, nullptr);
                        break;                              // wait for the timer before the next request
                    }

                    if (hybrid && flag == "cpu")
                    {
                        // Offload the CPU work to a worker; the epoll thread
                        // returns to serving I/O immediately.
                        c.state = ConnState::Working;
                        mod_events(fd, 0);                  // park the fd: no epoll interest until the response is ready
                        {
                            std::lock_guard<std::mutex> lock(task_m);
                            task_queue.push({fd, c.id});
                        }
                        task_cv.notify_one();
                        break;                              // this connection is now in a worker's hands
                    }

                    build_response(c);                      // none: instant, cpu(non-hybrid): burn
                    c.state = ConnState::Writing;
                    FlushResult fr = flush_response(c);
                    if (fr == FlushResult::WouldBlock) { mod_events(fd, EPOLLOUT); break; }  // don't send atop a half-written response
                    if (fr == FlushResult::Error)      { close_conn(fd);          break; }  // c is dangling after close

                    bool alive = c.keep_alive;              // read before finish_response may erase c
                    finish_response(fd, c);                 // keep-alive: reset to Reading + EPOLLIN; else close
                    if (!alive) { break; }                  // c is dangling after close_conn
                }
                continue;
            }

            // (c2) writable: keep sending the response where we left off.
            if (c.state == ConnState::Writing && (evmask & EPOLLOUT))
            {
                FlushResult fr = flush_response(c);
                if (fr == FlushResult::Done)       { finish_response(fd, c); }
                else if (fr == FlushResult::Error) { close_conn(fd); }
                // WouldBlock: stay on EPOLLOUT, wait for the next event.
                continue;
            }
        }

        // Phase 7: reap connections idle beyond the timeout. Only those still
        // Reading (awaiting a request) are candidates -- a Slowloris client that
        // opened a socket and then sends its request bytes too slowly, or never.
        // Connections mid-work (Working/Writing) are never reaped out from under us.
        if (g_idle_timeout_sec > 0)
        {
            auto now = std::chrono::steady_clock::now();
            if (now - last_sweep >= std::chrono::seconds(1))
            {
                last_sweep = now;
                auto deadline = std::chrono::seconds(g_idle_timeout_sec);
                std::vector<int> idle;
                for (auto& [cfd, conn] : conns)
                {
                    if (conn.state == ConnState::Reading && now - conn.last_activity > deadline)
                    {
                        idle.push_back(cfd);
                    }
                }
                for (int cfd : idle) { close_conn(cfd); }   // erases from conns
            }
        }
    }
}

// On SIGTERM/SIGINT (the sweep kills the server this way), emit the process's
// resource usage and exit. RUSAGE_SELF aggregates ALL threads including ones
// that already exited -- essential for thread-per-conn, whose per-request
// threads come and go, so /proc sampling of live threads would miss them.
// ru_nvcsw / ru_nivcsw = voluntary / involuntary context switches: the number
// that exposes the scheduler thrashing when thread-per-conn oversubscribes.
static void on_term(int)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "RUSAGE,%ld,%ld,%ld.%06ld,%ld.%06ld\n",
        ru.ru_nvcsw, ru.ru_nivcsw,
        (long)ru.ru_utime.tv_sec, (long)ru.ru_utime.tv_usec,
        (long)ru.ru_stime.tv_sec, (long)ru.ru_stime.tv_usec);
    if (len > 0)
    {
        ssize_t w = write(STDERR_FILENO, buf, static_cast<size_t>(len));
        (void)w;
    }
    _exit(0);
}

int main(int argc, char* argv[])
{
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    if (argc < 5)
    {
        std::cout << "usage: TCPServer --build=<none|sleep|cpu> --arch=<pool|thread-per-conn|epoll|hybrid|epoll-mt> --workers <N> [--file <path> --io=<readwrite|sendfile>]\n";
        return 1;
    }

    std::string argument = argv[1];
    if (argument == "--build=sleep")
    {
        flag = "sleep";
    }
    else if (argument == "--build=cpu")
    {
        flag = "cpu";
    }

    std::string arch;
    std::string argv2 = argv[2];
    if (argv2 == "--arch=thread-per-conn")
    {
        arch = "thread-per-conn";
    }
    else if (argv2 == "--arch=pool")
    {
        arch = "pool";
    }
    else if (argv2 == "--arch=epoll")
    {
        arch = "epoll";
    }
    else if (argv2 == "--arch=hybrid")
    {
        arch = "hybrid";
    }
    else if (argv2 == "--arch=epoll-mt")
    {
        arch = "epoll-mt";
    }

    std::string argv3 = argv[3];
    if (argv3 != "--workers")
    {
        std::cout << "The 4th argument has to be --workers followed by the number";
    }
    std::string argv4 = argv[4];
    int NUMBER_OF_THREADS = stoi(argv4);

    // Phase 5: optional static-file serving. --file <path> --io=<readwrite|sendfile>
    std::string file_path;
    for (int i = 5; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--file" && i + 1 < argc) { file_path = argv[++i]; }
        else if (a == "--io=sendfile")     { g_sendfile = true; }
        else if (a == "--io=readwrite")    { g_sendfile = false; }
        else if (a == "--idle-timeout" && i + 1 < argc) { g_idle_timeout_sec = std::stoi(argv[++i]); }
        else if (a == "--max-conns" && i + 1 < argc)    { g_max_conns = static_cast<size_t>(std::stoul(argv[++i])); }
    }
    if (!file_path.empty())
    {
        g_file_fd = open(file_path.c_str(), O_RDONLY);
        if (g_file_fd < 0) { perror("open file"); return 1; }
        struct stat st{};
        if (fstat(g_file_fd, &st) < 0) { perror("fstat"); return 1; }
        g_file_size = st.st_size;
        g_file_headers = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: "
                       + std::to_string(static_cast<long long>(g_file_size)) + "\r\n\r\n";
        if (!g_sendfile)   // read+write: cache the whole file in userspace once
        {
            g_readwrite_body.resize(static_cast<size_t>(g_file_size));
            off_t got = 0;
            while (got < g_file_size)
            {
                ssize_t r = pread(g_file_fd, &g_readwrite_body[got], g_file_size - got, got);
                if (r <= 0) { break; }
                got += r;
            }
        }
    }

    std::vector<std::thread> vec;

    if (arch == "epoll-mt")
    {
        // N independent epoll loops, each with its own SO_REUSEPORT listener;
        // the kernel load-balances connections across them. No shared state.
        for (int i = 0; i < NUMBER_OF_THREADS; i++)
        {
            vec.push_back(std::thread([]{ run_epoll_loop(make_listener(true), false); }));
        }
        for (auto& t : vec) { t.join(); }
    }
    else if (arch == "epoll" || arch == "hybrid")
    {
        // One epoll I/O loop. In hybrid mode, spawn the CPU worker pool first.
        if (arch == "hybrid")
        {
            for (int i = 0; i < NUMBER_OF_THREADS; i++)
            {
                vec.push_back(std::thread(hybrid_worker));
            }
        }
        run_epoll_loop(make_listener(false), arch == "hybrid");
    }
    else
    {
        // Blocking archs: one listener, one blocking accept loop.
        if (arch == "pool")
        {
            for (int i = 0; i < NUMBER_OF_THREADS; i++)
            {
                vec.push_back(std::thread(worker_pool));
            }
        }

        Fd server = make_listener(false);
        while (true)
        {
            int client = accept(server.get(), nullptr, nullptr);
            if (client >= 0) { set_nodelay(client); }

            if (arch == "pool")
            {
                std::unique_lock lock(m);
                connection_queue.push(client);
                lock.unlock();
                cv.notify_one();
            }
            else if (arch == "thread-per-conn")
            {
                std::thread(handle_connection, client).detach();
            }
        }
    }

    return 0;
}
