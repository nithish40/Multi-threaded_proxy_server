#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <chrono>
#include <filesystem>
#include <ctime>
#include <memory>
#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>

// Constants
constexpr int BUFFER_SIZE = 8192;
constexpr int MAX_WORKERS = 50;
constexpr int DEFAULT_TIMEOUT = 30;
const std::string CACHE_DIR = "cache";
const std::string LOGS_DIR = "logs";

class ProxyLogger {
private:
    std::mutex log_mutex;
    std::ofstream access_log;
    std::ofstream error_log;
    std::ofstream security_log;
    
    struct Statistics {
        size_t total_requests{0};
        size_t blocked_requests{0};
        size_t bytes_transferred{0};
    } stats;

    std::string format_bytes(size_t bytes) {
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        int unit_index = 0;
        double size = static_cast<double>(bytes);
        
        while (size >= 1024 && unit_index < 4) {
            size /= 1024;
            unit_index++;
        }
        
        std::stringstream ss;
        ss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
        return ss.str();
    }

    void setup_log_files() {
        std::filesystem::create_directories(LOGS_DIR);
        access_log.open(LOGS_DIR + "/access.log", std::ios::app);
        error_log.open(LOGS_DIR + "/error.log", std::ios::app);
        security_log.open(LOGS_DIR + "/security.log", std::ios::app);
    }

public:
    ProxyLogger() {
        setup_log_files();
    }

    ~ProxyLogger() {
        access_log.close();
        error_log.close();
        security_log.close();
    }

    void log_access(const std::string& client_ip, const std::string& method,
                   const std::string& url, int status_code, size_t bytes_transferred) {
        std::lock_guard<std::mutex> lock(log_mutex);
        stats.total_requests++;
        stats.bytes_transferred += bytes_transferred;
        
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        
        access_log << std::ctime(&time) << client_ip << " | " 
                  << method << " | " << url << " | " 
                  << status_code << " | " << bytes_transferred << std::endl;
    }

    void log_error(const std::string& client_ip, const std::string& message,
                  const std::exception* e = nullptr) {
        std::lock_guard<std::mutex> lock(log_mutex);
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        
        error_log << std::ctime(&time) << client_ip << " | " << message;
        if (e) {
            error_log << " | Exception: " << e->what();
        }
        error_log << std::endl;
    }

    void log_security(const std::string& client_ip, const std::string& message) {
        std::lock_guard<std::mutex> lock(log_mutex);
        stats.blocked_requests++;
        
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        
        security_log << std::ctime(&time) << "SECURITY | " 
                    << client_ip << " | " << message << std::endl;
    }

    Statistics get_statistics() const {
        return stats;
    }
};

class RequestHandler;

class Proxy {
private:
    int port;
    int server_socket;
    bool running;
    std::map<std::string, std::string> cache_file;
    std::map<std::string, bool> block_sites;
    std::vector<std::thread> worker_threads;
    ProxyLogger logger;
    std::mutex cache_mutex;
    std::mutex sites_mutex;

    // void load_cache() {
    //     try {
    //         std::ifstream file("cached_sites.dat", std::ios::binary);
    //         if (file.is_open()) {
    //             // Implement cache loading logic
    //             file.close();
    //         }
    //     } catch (const std::exception& e) {
    //         logger.log_error("system", "Cache loading error", &e);
    //     }
    // }

    void load_blocked_sites() {
        try {
            std::ifstream file("blocked_sites.dat", std::ios::binary);
            if (file.is_open()) {
                // Implement blocked sites loading logic
                file.close();
            }
        } catch (const std::exception& e) {
            logger.log_error("system", "Blocked sites loading error", &e);
        }
    }

    // void save_cache() {
    //     std::lock_guard<std::mutex> lock(cache_mutex);
    //     std::ofstream file("cached_sites.dat", std::ios::binary);
    //     // Implement cache saving logic
    // }

    void save_blocked_sites() {
        std::lock_guard<std::mutex> lock(sites_mutex);
        std::ofstream file("blocked_sites.dat", std::ios::binary);
        // Implement blocked sites saving logic
    }

public:
    Proxy(int port_) : port(port_), running(true) {
        std::filesystem::create_directories(CACHE_DIR);
        load_cache();
        load_blocked_sites();
        
        // Handle SIGINT gracefully
        signal(SIGINT, [](int) {
            // Cleanup code here
            exit(0);
        });
    }

    ~Proxy() {
        stop();
    }

    void start() {
        server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket < 0) {
            throw std::runtime_error("Failed to create socket");
        }

        int opt = 1;
        setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port);

        if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            throw std::runtime_error("Failed to bind socket");
        }

        if (listen(server_socket, 100) < 0) {
            throw std::runtime_error("Failed to listen on socket");
        }

        std::cout << "Proxy server listening on port " << port << std::endl;

        // Start management interface in a separate thread
        std::thread mgmt_thread(&Proxy::dynamic_manager, this);
        mgmt_thread.detach();

        // Main accept loop
        while (running) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            
            int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
            if (client_socket < 0) {
                logger.log_error("system", "Failed to accept connection");
                continue;
            }

            std::string client_ip = inet_ntoa(client_addr.sin_addr);
            std::cout << "Connection from " << client_ip << std::endl;

            // Create new thread for client handling
            worker_threads.emplace_back(&Proxy::handle_client, this, client_socket, client_ip);
        }
    }

    void stop() {
        running = false;
        save_cache();
        save_blocked_sites();
        
        // Join all worker threads
        for (auto& thread : worker_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        
        if (server_socket >= 0) {
            close(server_socket);
        }
    }

    // void handle_client(int client_socket, const std::string& client_ip) {
    //     try {
    //         // Create request handler and process request
    //         RequestHandler handler(client_socket, this, client_ip, logger);
    //         handler.handle_request();
    //     } catch (const std::exception& e) {
    //         logger.log_error(client_ip, "Error handling client", &e);
    //     }
        
    //     close(client_socket);
    // }

    void dynamic_manager() {
        while (running) {
            std::cout << "\nProxy Menu:\n"
                     << "1. Block a site\n"
                     << "2. View blocked sites\n"
                     << "3. View cached sites\n"
                     << "4. View statistics\n"
                     << "5. Clear cache\n"
                     << "6. View recent logs\n"
                     << "7. Exit\n"
                     << "Enter choice (1-7): ";

            std::string choice;
            std::getline(std::cin, choice);

            try {
                if (choice == "1") {
                    std::cout << "Enter site to block: ";
                    std::string site;
                    std::getline(std::cin, site);
                    std::lock_guard<std::mutex> lock(sites_mutex);
                    block_sites[site] = true;
                    std::cout << "Blocked " << site << std::endl;
                    save_blocked_sites();
                }
                // Implement other menu options...
            } catch (const std::exception& e) {
                logger.log_error("system", "Menu error", &e);
            }
        }
    }

    bool is_site_blocked(const std::string& site) const {
        std::lock_guard<std::mutex> lock(sites_mutex);
        return block_sites.find(site) != block_sites.end();
    }
};

class RequestHandler {
private:
    int client_socket;
    Proxy* proxy;
    std::string client_ip;
    ProxyLogger& logger;
    std::vector<char> request_data;
    std::map<std::string, std::string> headers;
    size_t bytes_transferred;

    void parse_headers(const std::string& header_data) {
        std::istringstream stream(header_data);
        std::string line;
        
        while (std::getline(stream, line) && !line.empty()) {
            auto colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = line.substr(0, colon_pos);
                std::string value = line.substr(colon_pos + 1);
                
                // Trim whitespace
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);
                
                headers[key] = value;
            }
        }
    }

public:
    RequestHandler(int socket, Proxy* proxy_, const std::string& ip, ProxyLogger& logger_)
        : client_socket(socket), proxy(proxy_), client_ip(ip), logger(logger_),
          bytes_transferred(0) {}

    void handle_request() {
        try {
            char buffer[BUFFER_SIZE];
            std::string header_data;
            bool headers_complete = false;
            
            while (!headers_complete) {
                ssize_t bytes_read = recv(client_socket, buffer, BUFFER_SIZE, 0);
                if (bytes_read <= 0) {
                    return;
                }
                
                header_data.append(buffer, bytes_read);
                if (header_data.find("\r\n\r\n") != std::string::npos) {
                    headers_complete = true;
                }
            }

            // Parse request line
            std::istringstream request_stream(header_data);
            std::string request_line;
            std::getline(request_stream, request_line);
            
            std::istringstream request_parts(request_line);
            std::string method, url, version;
            request_parts >> method >> url >> version;

            // Parse headers
            parse_headers(header_data.substr(request_line.length() + 2));

            // Handle request based on method
            if (method == "CONNECT") {
                handle_https_request(url);
            } else {
                handle_http_request(method, url);
            }

        } catch (const std::exception& e) {
            logger.log_error(client_ip, "Request handling error", &e);
        }
    }

    void handle_http_request(const std::string& method, const std::string& url) {
        // Implementation of HTTP request handling
        // This would involve creating a connection to the target server,
        // forwarding the request, and handling the response
    }

    void handle_https_request(const std::string& url) {
        // Implementation of HTTPS tunneling
        // This would involve creating an SSL tunnel between the client
        // and the target server
    }

    void send_blocked_response() {
        const char* response =
            "HTTP/1.1 403 Forbidden\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html><body><h1>403 Forbidden</h1>"
            "<p>This site is blocked by the proxy.</p></body></html>";
            
        send(client_socket, response, strlen(response), 0);
    }

    void send_error_response(int status_code) {
        std::stringstream response;
        response << "HTTP/1.1 " << status_code << " Error\r\n"
                << "Content-Type: text/html\r\n"
                << "Connection: close\r\n"
                << "\r\n"
                << "<html><body><h1>" << status_code << " Error</h1></body></html>";
                
        std::string response_str = response.str();
        send(client_socket, response_str.c_str(), response_str.length(), 0);
    }
};

// Cache loading implementation
void Proxy::load_cache() {
    try {
        std::ifstream file("cached_sites.dat", std::ios::binary);
        if (file.is_open()) {
            std::lock_guard<std::mutex> lock(cache_mutex);
            size_t cache_size;
            file.read(reinterpret_cast<char*>(&cache_size), sizeof(size_t));
            
            for (size_t i = 0; i < cache_size; ++i) {
                size_t key_size, value_size;
                
                // Read key size and key
                file.read(reinterpret_cast<char*>(&key_size), sizeof(size_t));
                std::string key(key_size, '\0');
                file.read(&key[0], key_size);
                
                // Read value size and value
                file.read(reinterpret_cast<char*>(&value_size), sizeof(size_t));
                std::string value(value_size, '\0');
                file.read(&value[0], value_size);
                
                cache_file[key] = value;
            }
            file.close();
        }
    } catch (const std::exception& e) {
        logger.log_error("system", "Cache loading error", &e);
    }
}

// Save cache implementation
void Proxy::save_cache() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    std::ofstream file("cached_sites.dat", std::ios::binary);
    
    if (file.is_open()) {
        // Write cache size
        size_t cache_size = cache_file.size();
        file.write(reinterpret_cast<const char*>(&cache_size), sizeof(size_t));
        
        // Write each cache entry
        for (const auto& entry : cache_file) {
            // Write key
            size_t key_size = entry.first.size();
            file.write(reinterpret_cast<const char*>(&key_size), sizeof(size_t));
            file.write(entry.first.c_str(), key_size);
            
            // Write value
            size_t value_size = entry.second.size();
            file.write(reinterpret_cast<const char*>(&value_size), sizeof(size_t));
            file.write(entry.second.c_str(), value_size);
        }
        file.close();
    }
}

// Complete handle_client implementation
void Proxy::handle_client(int client_socket, const std::string& client_ip) {
    try {
        char buffer[BUFFER_SIZE];
        std::string request_data;
        ssize_t bytes_received;
        
        // Set socket timeout
        struct timeval timeout;
        timeout.tv_sec = DEFAULT_TIMEOUT;
        timeout.tv_usec = 0;
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        
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
            close(client_socket);
            return;
        }

        // Parse request
        std::istringstream request_stream(request_data);
        std::string request_line;
        std::getline(request_stream, request_line);
        
        std::istringstream request_parts(request_line);
        std::string method, url, version;
        request_parts >> method >> url >> version;

        // Parse headers
        std::map<std::string, std::string> headers;
        std::string header_line;
        while (std::getline(request_stream, header_line) && header_line != "\r") {
            size_t colon_pos = header_line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = header_line.substr(0, colon_pos);
                std::string value = header_line.substr(colon_pos + 2); // Skip ": "
                if (!value.empty() && value.back() == '\r') {
                    value.pop_back();
                }
                headers[key] = value;
            }
        }

        // Handle request based on method
        if (method == "CONNECT") {
            handle_https_request(client_socket, url, headers, client_ip);
        } else {
            handle_http_request(client_socket, method, url, headers, request_data, client_ip);
        }

    } catch (const std::exception& e) {
        logger.log_error(client_ip, "Error handling client", &e);
    }
    
    close(client_socket);
}

// Utility function for tunneling data
void tunnel_data(int source_socket, int dest_socket, const std::string& client_ip,
                const std::string& direction, ProxyLogger& logger) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    while ((bytes_read = recv(source_socket, buffer, BUFFER_SIZE, 0)) > 0) {
        ssize_t bytes_sent = send(dest_socket, buffer, bytes_read, 0);
        if (bytes_sent <= 0) {
            logger.log_error(client_ip, "Failed to forward data: " + direction);
            break;
        }
    }

    if (bytes_read < 0) {
        logger.log_error(client_ip, "Error reading data: " + direction);
    }
}

// Helper function for HTTP response handling
void forward_response(int client_socket, int server_socket, const std::string& client_ip,
                     ProxyLogger& logger) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;
    size_t total_bytes = 0;

    while ((bytes_received = recv(server_socket, buffer, BUFFER_SIZE, 0)) > 0) {
        ssize_t bytes_sent = send(client_socket, buffer, bytes_received, 0);
        if (bytes_sent <= 0) {
            logger.log_error(client_ip, "Failed to forward response to client");
            break;
        }
        total_bytes += bytes_sent;
    }

    logger.log_access(client_ip, "RESPONSE", "", 200, total_bytes);
}
int main() {
    try {
        Proxy proxy(8080);
        proxy.start();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

