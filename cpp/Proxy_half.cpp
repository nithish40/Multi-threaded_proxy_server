#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <fstream>
#include <sstream>
#include <chrono>
#include <filesystem>
#include <atomic>
#include <queue>
#include <condition_variable>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define CLOSE_SOCKET closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
    #include <fcntl.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define CLOSE_SOCKET close
#endif

namespace fs = std::filesystem;

// Constants
constexpr int BUFFER_SIZE = 8192;
constexpr int MAX_THREADS = 50;
constexpr int DEFAULT_PORT = 8080;

// Logger class for handling log files
class Logger {
private:
    std::mutex log_mutex;
    std::ofstream activity_log;
    std::ofstream error_log;
    std::string log_dir;

    std::atomic<int> total_requests{0};
    std::atomic<int> blocked_requests{0};
    std::atomic<int> cache_hits{0};
    std::atomic<int> errors{0};

public:
    Logger(const std::string& dir = "logs") : log_dir(dir) {
        fs::create_directories(log_dir);
        
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y%m%d");
        
        activity_log.open(log_dir + "/activity_" + ss.str() + ".log", std::ios::app);
        error_log.open(log_dir + "/error_" + ss.str() + ".log", std::ios::app);
    }

    void log_activity(const std::string& client_ip, const std::string& url, 
                     const std::string& method, const std::string& status) {
        std::lock_guard<std::mutex> lock(log_mutex);
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        
        activity_log << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S")
                    << " | Client: " << client_ip 
                    << " | URL: " << url 
                    << " | Method: " << method 
                    << " | Status: " << status << std::endl;
        
        total_requests++;
    }

    void log_error(const std::string& client_ip, const std::string& message) {
        std::lock_guard<std::mutex> lock(log_mutex);
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        
        error_log << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S")
                 << " | ERROR | Client: " << client_ip 
                 << " | Message: " << message << std::endl;
        
        errors++;
    }

    void log_blocked(const std::string& client_ip, const std::string& url) {
        std::lock_guard<std::mutex> lock(log_mutex);
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        
        activity_log << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S")
                    << " | BLOCKED | Client: " << client_ip 
                    << " | URL: " << url << std::endl;
        
        blocked_requests++;
    }

    void get_stats() const {
        std::cout << "\nProxy Statistics:\n"
                  << "Total Requests: " << total_requests << "\n"
                  << "Blocked Requests: " << blocked_requests << "\n"
                  << "Cache Hits: " << cache_hits << "\n"
                  << "Errors: " << errors << "\n";
    }
};

// Thread Pool implementation
class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;

public:
    ThreadPool(size_t threads) : stop(false) {
        for(size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while(true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        condition.wait(lock, [this] { 
                            return stop || !tasks.empty(); 
                        });
                        if(stop && tasks.empty()) {
                            return;
                        }
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for(std::thread &worker: workers) {
            worker.join();
        }
    }
};

// HTTP Request Parser
class HttpRequest {
public:
    std::string method;
    std::string url;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;

    static HttpRequest parse(const std::string& raw_request) {
        HttpRequest request;
        std::istringstream stream(raw_request);
        std::string line;

        // Parse request line
        std::getline(stream, line);
        std::istringstream request_line(line);
        request_line >> request.method >> request.url >> request.version;

        // Parse headers
        while (std::getline(stream, line) && line != "\r") {
            auto colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = line.substr(0, colon_pos);
                std::string value = line.substr(colon_pos + 1);
                // Trim whitespace
                value.erase(0, value.find_first_not_of(" "));
                value.erase(value.find_last_not_of("\r") + 1);
                request.headers[key] = value;
            }
        }

        // Read body if present
        std::stringstream body_stream;
        while (std::getline(stream, line)) {
            body_stream << line << "\n";
        }
        request.body = body_stream.str();

        return request;
    }
};

// Proxy Server class
class ProxyServer {
private:
    SOCKET server_socket;
    int port;
    Logger logger;
    ThreadPool thread_pool;
    std::atomic<bool> running;
    std::map<std::string, bool> blocked_sites;
    std::mutex blocked_sites_mutex;

    void handle_client(SOCKET client_socket, const std::string& client_ip) {
        char buffer[BUFFER_SIZE];
        std::string request_data;
        int bytes_received;

        // Receive request
        while ((bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0)) > 0) {
            buffer[bytes_received] = '\0';
            request_data += buffer;
            if (request_data.find("\r\n\r\n") != std::string::npos) {
                break;
            }
        }

        if (bytes_received <= 0) {
            logger.log_error(client_ip, "Connection closed by client");
            CLOSE_SOCKET(client_socket);
            return;
        }

        try {
            auto request = HttpRequest::parse(request_data);
            
            if (request.method == "CONNECT") {
                handle_https_request(client_socket, request, client_ip);
            } else {
                handle_http_request(client_socket, request, client_ip);
            }
        } catch (const std::exception& e) {
            logger.log_error(client_ip, std::string("Error processing request: ") + e.what());
            send_error_response(client_socket, 400, "Bad Request");
        }

        CLOSE_SOCKET(client_socket);
    }

    void handle_http_request(SOCKET client_socket, const HttpRequest& request, 
                           const std::string& client_ip) {
        // Check if site is blocked
        {
            std::lock_guard<std::mutex> lock(blocked_sites_mutex);
            if (blocked_sites[request.url]) {
                logger.log_blocked(client_ip, request.url);
                send_blocked_response(client_socket);
                return;
            }
        }

        // Log the request
        logger.log_activity(client_ip, request.url, request.method, "REQUESTED");

        // Create socket for target server
        SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock == INVALID_SOCKET) {
            logger.log_error(client_ip, "Failed to create socket for target server");
            send_error_response(client_socket, 502, "Bad Gateway");
            return;
        }

        // Parse URL and connect to target server
        std::string host = request.headers["Host"];
        struct hostent* he = gethostbyname(host.c_str());
        if (!he) {
            logger.log_error(client_ip, "Failed to resolve host: " + host);
            send_error_response(client_socket, 502, "Bad Gateway");
            CLOSE_SOCKET(server_sock);
            return;
        }

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(80);
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);

        if (connect(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            logger.log_error(client_ip, "Failed to connect to target server");
            send_error_response(client_socket, 502, "Bad Gateway");
            CLOSE_SOCKET(server_sock);
            return;
        }

        // Forward request to target server
        std::stringstream forward_request;
        forward_request << request.method << " " << request.url << " " << request.version << "\r\n";
        for (const auto& header : request.headers) {
            forward_request << header.first << ": " << header.second << "\r\n";
        }
        forward_request << "\r\n" << request.body;

        std::string request_str = forward_request.str();
        if (send(server_sock, request_str.c_str(), request_str.length(), 0) < 0) {
            logger.log_error(client_ip, "Failed to send request to target server");
            send_error_response(client_socket, 502, "Bad Gateway");
            CLOSE_SOCKET(server_sock);
            return;
        }

        // Forward response to client
        char buffer[BUFFER_SIZE];
        int bytes_received;
        while ((bytes_received = recv(server_sock, buffer, BUFFER_SIZE, 0)) > 0) {
            if (send(client_socket, buffer, bytes_received, 0) < 0) {
                logger.log_error(client_ip, "Failed to send response to client");
                break;
            }
        }

        logger.log_activity(client_ip, request.url, request.method, "COMPLETED");
        CLOSE_SOCKET(server_sock);
    }

    void handle_https_request(SOCKET client_socket, const HttpRequest& request, 
                            const std::string& client_ip) {
        std::string host = request.url.substr(0, request.url.find(":"));
        int port = std::stoi(request.url.substr(request.url.find(":") + 1));

        logger.log_activity(client_ip, "https://" + request.url, "CONNECT", "REQUESTED");

        // Create socket for target server
        SOCKET server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock == INVALID_SOCKET) {
            logger.log_error(client_ip, "Failed to create socket for HTTPS connection");
            send_error_response(client_socket, 502, "Bad Gateway");
            return;
        }

        // Resolve host
        struct hostent* he = gethostbyname(host.c_str());
        if (!he) {
            logger.log_error(client_ip, "Failed to resolve host: " + host);
            send_error_response(client_socket, 502, "Bad Gateway");
            CLOSE_SOCKET(server_sock);
            return;
        }

        // Connect to target server
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);

        if (connect(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            logger.log_error(client_ip, "Failed to connect to HTTPS server");
            send_error_response(client_socket, 502, "Bad Gateway");
            CLOSE_SOCKET(server_sock);
            return;
        }

        // Send connection established response
        const char* response = "HTTP/1.1 200 Connection Established\r\n\r\n";
        if (send(client_socket, response, strlen(response), 0) < 0) {
            logger.log_error(client_ip, "Failed to send connection established response");
            CLOSE_SOCKET(server_sock);
            return;
        }

        logger.log_activity(client_ip, "https://" + request.url, "CONNECT", "ESTABLISHED");

        // Start tunneling in both directions
        std::thread forward_thread([this, client_socket, server_sock, client_ip]() {
            tunnel_data(client_socket, server_sock, client_ip, "client->server");
        });

        tunnel_data(server_sock, client_socket, client_ip, "server->client");
        forward_threa