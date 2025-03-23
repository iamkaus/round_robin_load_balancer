#include "ping_server.hpp"
#include <thread>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>
using std::min;

// Platform-specific includes
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
    #define close closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
#endif

// Constructor
PingServer::PingServer(
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds interval
) : _timeout(timeout),
    _interval(interval),
    _isRunning(false),
    _threadPoolSize(4),
    _dnsCacheTTL(300), // Default 5 minutes
    _pingImplementation(std::bind(&PingServer::defaultPingImplementation, 
                                 this, 
                                 std::placeholders::_1, 
                                 std::placeholders::_2))
{
    // Initialize socket library on Windows
    #ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    #endif
}

// Destructor
PingServer::~PingServer()
{
    stopBackgroundPing();
    
    // Cleanup socket library on Windows
    #ifdef _WIN32
        WSACleanup();
    #endif
}

// Parse server address to extract hostname/IP and port
bool PingServer::parseServerAddress(const std::string& serverAddress, std::string& host, int& port)
{
    size_t colonPos = serverAddress.find(':');
    if (colonPos == std::string::npos) {
        // No port specified, use default HTTP port
        host = serverAddress;
        port = 80; 
        return true;
    }
    
    host = serverAddress.substr(0, colonPos);
    try {
        port = std::stoi(serverAddress.substr(colonPos + 1));
        return true;
    } catch (...) {
        return false; // Invalid port
    }
}

// DNS resolution with caching
std::string PingServer::resolveHostname(const std::string& hostname)
{
    // Check if IP address (simple check)
    struct sockaddr_in sa;
    if (inet_pton(AF_INET, hostname.c_str(), &(sa.sin_addr)) == 1) {
        return hostname; // Already an IPv4 address
    }
    
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(_dnsCacheMutex);
        auto it = _dnsCache.find(hostname);
        
        if (it != _dnsCache.end() && 
            it->second.expiryTime > std::chrono::steady_clock::now()) {
            return it->second.resolvedIP;
        }
    }
    
    // Perform DNS resolution
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    if (getaddrinfo(hostname.c_str(), nullptr, &hints, &result) != 0) {
        return ""; // Resolution failed
    }
    
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, 
              &((struct sockaddr_in*)result->ai_addr)->sin_addr,
              ipStr, 
              INET_ADDRSTRLEN);
    
    std::string resolvedIP = ipStr;
    freeaddrinfo(result);
    
    // Update cache
    {
        std::lock_guard<std::mutex> lock(_dnsCacheMutex);
        _dnsCache[hostname] = {
            resolvedIP,
            std::chrono::steady_clock::now() + _dnsCacheTTL
        };
    }
    
    return resolvedIP;
}

// Default ping implementation (TCP connect)
bool PingServer::defaultPingImplementation(const std::string& serverAddress, std::chrono::milliseconds timeout)
{
    std::string host;
    int port;
    
    if (!parseServerAddress(serverAddress, host, port)) {
        return false; // Invalid address format
    }
    
    // Resolve hostname with caching
    std::string resolvedIP = resolveHostname(host);
    if (resolvedIP.empty()) {
        return false; // Resolution failed
    }
    
    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }
    
    // Set socket to non-blocking
    #ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
    #else
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    #endif
    
    // Set up address structure
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, resolvedIP.c_str(), &(serverAddr.sin_addr));
    
    // Try to connect
    int connectResult = connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
    
    // Check for immediate success or in-progress
    bool success = false;
    
    #ifdef _WIN32
        if (connectResult == 0 || WSAGetLastError() == WSAEWOULDBLOCK) {
    #else
        if (connectResult == 0 || errno == EINPROGRESS) {
    #endif
            // Wait for the connection to complete or timeout
            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);
            
            struct timeval tv;
            tv.tv_sec = static_cast<long>(timeout.count() / 1000);
            tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
            
            int selectResult = select(sock + 1, nullptr, &fdset, nullptr, &tv);
            
            if (selectResult > 0) {
                // Check if connection was successful
                int error = 0;
                socklen_t len = sizeof(error);
                
                #ifdef _WIN32
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&error, &len);
                #else
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);
                #endif
                
                success = (error == 0);
            }
        }
    
    // Close socket regardless of result
    close(sock);
    return success;
}

// Parallel ping implementation
bool PingServer::parallelPingImplementation(std::vector<std::shared_ptr<Server>>& servers, 
                                          std::chrono::milliseconds timeout)
{
    if (servers.empty()) {
        return true;
    }
    
    size_t serverCount = servers.size();
    size_t poolSize = (std::min)(_threadPoolSize.load(), serverCount);
    
    // Create futures for parallel pinging
    std::vector<std::future<void>> futures;
    futures.reserve(poolSize);
    
    std::atomic<size_t> nextIndex(0);
    std::atomic<bool> allSuccessful(true);
    
    // Launch worker threads
    for (size_t i = 0; i < poolSize; ++i) {
        futures.push_back(std::async(std::launch::async, [&]() {
            while (true) {
                // Get next server to process
                size_t index = nextIndex.fetch_add(1);
                if (index >= serverCount) {
                    break;
                }
                
                auto& server = servers[index];
                bool result = _pingImplementation(server->getServerAddress(), timeout);
                
                // Update server status
                server->setHealthy(result);
                server->updateLastHealthCheck();
                
                // Track failures
                if (!result) {
                    server->incrementFailures();
                    
                    // If too many consecutive failures, mark as not alive
                    if (server->getFailureCount() >= 3) { // Configurable threshold
                        server->setAlive(false);
                    }
                    
                    allSuccessful.store(false);
                } else {
                    // Reset failures and ensure it's marked alive
                    server->resetFailures();
                    server->setAlive(true);
                }
            }
        }));
    }
    
    // Wait for all threads to complete
    for (auto& future : futures) {
        future.wait();
    }
    
    return allSuccessful.load();
}

// Ping a single server
bool PingServer::pingServer(const std::shared_ptr<Server>& server)
{
    if (!server) {
        return false;
    }
    
    bool result = _pingImplementation(server->getServerAddress(), _timeout);
    
    // Update server status
    server->setHealthy(result);
    server->updateLastHealthCheck();
    
    if (!result) {
        server->incrementFailures();
        if (server->getFailureCount() >= 3) { // Configurable threshold
            server->setAlive(false);
        }
    } else {
        server->resetFailures();
        server->setAlive(true);
    }
    
    return result;
}

// Ping multiple servers
bool PingServer::pingServers(const std::vector<std::shared_ptr<Server>>& servers)
{
    if (servers.empty()) {
        return true;
    }
    
    // Create a copy to allow modification
    auto serversCopy = servers;
    return parallelPingImplementation(serversCopy, _timeout);
}

// Start background ping
void PingServer::startBackgroundPing(const std::vector<std::shared_ptr<Server>>& servers)
{
    // If already running, stop first
    if (_isRunning.load()) {
        stopBackgroundPing();
    }
    
    _isRunning.store(true);
    
    // Create a copy of the servers for the background task
    auto serversCopy = servers;
    
    // Start the background task
    _pingTask = std::async(std::launch::async, [this, serversCopy]() {
        this->pingWorker(serversCopy);
    });
}

// Stop background ping
void PingServer::stopBackgroundPing()
{
    _isRunning.store(false);
    
    if (_pingTask.valid()) {
        try {
            _pingTask.wait();
        } catch (...) {
            // Ignore exceptions during shutdown
        }
    }
}

// Check if background ping is running
bool PingServer::isBackgroundPingRunning() const
{
    return _isRunning.load();
}

// Background ping worker
void PingServer::pingWorker(const std::vector<std::shared_ptr<Server>>& servers)
{
    while (_isRunning.load()) {
        // Ping all servers in parallel
        pingServers(servers);
        
        // Sleep for the interval
        std::this_thread::sleep_for(_interval);
    }
}

// Clear DNS cache
void PingServer::clearDNSCache()
{
    std::lock_guard<std::mutex> lock(_dnsCacheMutex);
    _dnsCache.clear();
}

// Configuration methods
void PingServer::setTimeout(std::chrono::milliseconds timeout)
{
    _timeout = timeout;
}

std::chrono::milliseconds PingServer::getTimeout() const
{
    return _timeout;
}

void PingServer::setInterval(std::chrono::milliseconds interval)
{
    _interval = interval;
}

std::chrono::milliseconds PingServer::getInterval() const
{
    return _interval;
}

void PingServer::setThreadPoolSize(size_t size)
{
    _threadPoolSize.store(size > 0 ? size : 1);
}

size_t PingServer::getThreadPoolSize() const
{
    return _threadPoolSize.load();
}

void PingServer::setDNSCacheTTL(std::chrono::seconds ttl)
{
    _dnsCacheTTL = ttl;
}

std::chrono::seconds PingServer::getDNSCacheTTL() const
{
    return _dnsCacheTTL;
}

// Set custom ping implementation
void PingServer::setPingImplementation(PingImplementation implementation)
{
    if (implementation) {
        _pingImplementation = implementation;
    }
}

// Reset to default ping implementation
void PingServer::resetPingImplementation()
{
    _pingImplementation = std::bind(&PingServer::defaultPingImplementation, 
                                   this, 
                                   std::placeholders::_1, 
                                   std::placeholders::_2);
}