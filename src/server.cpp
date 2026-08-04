#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <cstring>
#include <netinet/in.h>
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
#include <sys/timerfd.h>



#define PORT 8080
std::string flag = "none";
volatile uint64_t sink;
std::queue <int> connection_queue;
std::mutex m;
std::condition_variable cv;
std::unordered_map <int, std::string> buffers;   // fd → bytes accumulated so far
std::unordered_map <int, int> timer_to_client;

void write_and_close(int client)
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

    // Close the client file descriptor to flush the stream and terminate the connection
    close(client);
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
    
    write_and_close(client);
}

void handle_connection(int client)
{
    char buffer[1024] = {0};
    const char *response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 12\r\n\r\nHello World!";

    if (client < 0) {
        perror("Accept failed");
        // continue; 
        return;
    }

    // read(): Read incoming bytes from the client stream into our buffer
    size_t total = 0;
    bool complete = false;
    while (total < sizeof(buffer))
    {
        ssize_t bytes_read = read(client, buffer + total, sizeof(buffer) - total - 1);
        if (bytes_read <= 0)
        {
            break;
        }
        total += bytes_read;
        buffer[total] = '\0';
        if (strstr(buffer, "\r\n\r\n"))
        {
            complete = true;
            break;
        }
    }

    if (!complete)
    {
        close(client);
        // continue;
        return;
    }

    respond(client);
    
    // printf("bytes read:\n%d\n", bytes_read);
    // printf("Received Request:\n%s\n", buffer);

    // if (flag == "sleep")
    // {
    //     usleep(100000);
    // }
    // else if (flag == "cpu")
    // {
    //     uint64_t x = 88172645463325252ULL;
    //     int ITERATIONS = 1000000;
    //     for (long i = 0; i < ITERATIONS; i++)
    //     {
    //         x ^= x << 13;
    //         x ^= x >> 7;
    //         x ^= x << 17;
    //     }
    //     sink = x;
    // }

    // write(): Send our raw text HTTP response back through the file descriptor
    // size_t sent = 0;
    // while (sent < strlen(response))
    // {
    //     ssize_t n = write(client, response + sent, strlen(response) - sent);
    //     if (n <= 0)
    //     {
    //         break;
    //     }
    //     sent += n;
    // }
    // printf("Response sent successfully.\n");

    // Close the client file descriptor to flush the stream and terminate the connection
    // close(client);
    memset(buffer, 0, 1024); // Clear the buffer for the next request
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

int main(int argc, char* argv[])
{
    if (argc != 5)
    {
        std::cout << "usage: TCPServer --build=<none|sleep|cpu> --arch=<pool|thread-per-conn> --workers <N>\n";
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

    std::string argv3 = argv[3];
    if (argv3 != "--workers")
    {
        std::cout << "The 4th argument has to be --workers followed by the number";
    }
    std::string argv4 = argv[4];
    int NUMBER_OF_THREADS = stoi(argv4);
    std::vector <std::thread> vec;
    if (arch == "pool")
    {
        for (int i = 0; i < NUMBER_OF_THREADS; i++)
        {
            vec.push_back(std::thread(worker_pool));
        }
    }




    int server, client;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0)
    {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // Added to allow to connect despite waiting time after closing the sockets
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. bind(): Attach the socket to port 8080
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        close(server);
        exit(EXIT_FAILURE);
    }

    // 3. listen(): Wait for incoming connections (backlog queue size of 3)
    if (listen(server, SOMAXCONN) < 0) {
        perror("Listen failed");
        close(server);
        exit(EXIT_FAILURE);
    }

    // printf("Server is listening on port %d...\n", PORT);

    // 4. The Infinite Loop (Accept -> Read -> Write)
    if (arch == "epoll")
    {
        int ep = epoll_create1(0);
        struct epoll_event events[64];

        struct epoll_event ev;
        ev.data.fd = server;
        ev.events = EPOLLIN;
        epoll_ctl(ep, EPOLL_CTL_ADD, server, &ev);

        fcntl(server, F_SETFL, O_NONBLOCK);
        while (true)
        {
            int n = epoll_wait(ep, events, 64, -1);
            for (int i = 0; i < n; i++)
            {
                if (timer_to_client.count(events[i].data.fd))
                {
                    int timer_fd = events[i].data.fd;
                    uint64_t exp;
                    ssize_t response = read(timer_fd, &exp, sizeof(exp));

                    write_and_close(timer_to_client[timer_fd]);

                    close(timer_fd);
                    timer_to_client.erase(timer_fd);
                }
                else if (events[i].data.fd == server)
                {
                    while (true)
                    {
                        int client_fd = accept(server, NULL, NULL);
                        if (client_fd < 0)
                        {
                            break;
                        }

                        fcntl(client_fd, F_SETFL, O_NONBLOCK);
                        ev.data.fd = client_fd;
                        ev.events = EPOLLIN;
                        epoll_ctl(ep, EPOLL_CTL_ADD, client_fd, &ev);
                    }
                }
                else
                {
                    int fd = events[i].data.fd;
                    char tmp[1024];
                    ssize_t k = read(fd, tmp, sizeof(tmp));

                    if (k > 0)
                    {
                        buffers[fd].append(tmp, k);
                        if (buffers[fd].find("\r\n\r\n") != std::string::npos)
                        {
                            if (flag == "sleep")
                            {
                                int timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);

                                timer_to_client[timer_fd] = fd;

                                struct epoll_event timer_ev;
                                timer_ev.data.fd = timer_fd;
                                timer_ev.events = EPOLLIN;
                                epoll_ctl(ep, EPOLL_CTL_ADD, timer_fd, &timer_ev);

                                struct  itimerspec timer_object = {};
                                timer_object.it_value.tv_sec = 0;
                                timer_object.it_value.tv_nsec = 100 * 1000 * 1000;
                                timerfd_settime(timer_fd, 0, &timer_object, NULL);
                            }
                            else
                            {
                                respond(fd);
                            }
                            buffers.erase(fd);
                        }
                    }
                    else if (k == 0)
                    {
                        close(fd);
                        buffers.erase(fd);
                    }
                    else
                    {
                        if (errno == EAGAIN) { /* nothing left now — just return */ }
                        else
                        {
                            close(fd);
                            buffers.erase(fd);
                        }
                    }
                }
            }
        }
    }
    else
    {
        while(1) {
            // printf("\nWaiting for a new connection...\n");
            
            // accept(): Blocks until a client connects. Returns a NEW file descriptor for this client.
            client = accept(server, (struct sockaddr *)&address, (socklen_t*)&addrlen);

            if (arch == "pool")
            {
                std::unique_lock lock (m);
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

    // Close the master listening socket (unreachable in this infinite loop)
    close(server);

    return 0;
}