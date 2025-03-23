#ifndef PING_SERVER_HPP_
#define PING_SERVER_HPP_

#include <string>
#include <chrono>
#include <future>
#include <functional>
#include <atomic>
#include <memory>
#include <vector>
#include <unordered_map>
#include <thread>
#include <mutex>
#include "server.hpp"

// Forward declaration
struct ResolvedAddress;

class PingServer {
private:
    std::chrono::milliseconds                               _timeout;                 // Timeout for ping operations
    std::chrono::milliseconds                               _interval;                // Interval between pings
    std::atomic<bool>                                       _isRunning{false};        // Flag to control background ping thread
    std::future<void>                                       _pingTask;                // Future for the background ping task
    std::atomic<size_t>                                     _threadPoolSize{4};       // Thread pool size
    
    // DNS cache
    struct DNSCacheEntry {
        std::string resolvedIP;
        std::chrono::steady_clock::time_point expiryTime;
    };
    
    std::unordered_map<std::string, DNSCacheEntry>          _dnsCache;               // Cache for DNS resolution
    std::mutex                                              _dnsCacheMutex;          // Mutex for DNS cache
    std::chrono::seconds                                    _dnsCacheTTL{300};       // DNS cache TTL (5 minutes)
    
    // Function type for custom ping implementations
    using PingImplementation = std::function<bool(const std::string&, std::chrono::milliseconds)>;
    PingImplementation                                      _pingImplementation;      // Custom ping implementation

    // Default ping implementation
    bool defaultPingImplementation(const std::string& serverAddress, std::chrono::milliseconds timeout);
    
    // Background ping worker function
    void pingWorker(const std::vector<std::shared_ptr<Server>>& servers);
    
    // DNS resolution with caching
    std::string resolveHostname(const std::string& hostname);
    
    // Parse server address 
    bool parseServerAddress(const std::string& serverAddress, std::string& host, int& port);
    
    // Parallel ping implementation
    bool parallelPingImplementation(std::vector<std::shared_ptr<Server>>& servers, 
                                  std::chrono::milliseconds timeout);

public:
    // Constructor with default timeout and interval
    explicit PingServer(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(1000),
        std::chrono::milliseconds interval = std::chrono::milliseconds(5000)
    );
    
    // Destructor ensures background tasks are stopped
    ~PingServer();

    // No copy or move (because of the async task)
    PingServer(const PingServer&) = delete;
    PingServer& operator=(const PingServer&) = delete;
    PingServer(PingServer&&) = delete;
    PingServer& operator=(PingServer&&) = delete;
    
    // Core functionality
    bool pingServer(const std::shared_ptr<Server>& server);             // Ping a single server
    bool pingServers(const std::vector<std::shared_ptr<Server>>& servers); // Ping multiple servers
    
    // Asynchronous ping operations
    void startBackgroundPing(const std::vector<std::shared_ptr<Server>>& servers);
    void stopBackgroundPing();
    bool isBackgroundPingRunning() const;
    
    // Configuration
    void setTimeout(std::chrono::milliseconds timeout);
    std::chrono::milliseconds getTimeout() const;
    void setInterval(std::chrono::milliseconds interval);
    std::chrono::milliseconds getInterval() const;
    void setThreadPoolSize(size_t size);
    size_t getThreadPoolSize() const;
    void setDNSCacheTTL(std::chrono::seconds ttl);
    std::chrono::seconds getDNSCacheTTL() const;
    
    // Custom ping implementation
    void setPingImplementation(PingImplementation implementation);
    void resetPingImplementation(); // Reset to default implementation
    
    // DNS cache management
    void clearDNSCache();
};

#endif // PING_SERVER_HPP_