#ifndef SERVER_HPP_
#define SERVER_HPP_

#include <string>
#include <chrono>
#include <mutex>
#include <atomic>
#include <memory>

class Server {
private:
    std::string                                             _serverAddress;                         // Address of the server    
    std::atomic<bool>                                       _isAlive{true};                         // Atomic bool for thread safety
    std::atomic<bool>                                       _isHealthy{false};                      // Health status
    std::chrono::time_point<std::chrono::steady_clock>      _lastHealthCheck;                       // Last health check timestamp
    mutable std::mutex                                      _timeMutex;                             // Mutex for timestamp access
    std::atomic<uint32_t>                                   _weight{1};                             // Server weight for weighted algorithms
    std::atomic<uint32_t>                                   _currentConnections{0};                 // Current connection count
    std::atomic<uint32_t>                                   _failureCount{0};                       // Consecutive failures


public:
// Constructors with explicit keyword to prevent implicit conversions
explicit Server(const std::string& serverAddress, uint32_t weight = 1);
explicit Server(std::string&& serverAddress, uint32_t weight = 1);

// Rule of five
Server(const Server& other);
Server(Server&& other) noexcept;
Server& operator=(const Server& other);
Server& operator=(Server&& other) noexcept;
virtual ~Server();

// Getters
const std::string& getServerAddress() const;
bool isAlive() const;
bool isHealthy() const;
std::chrono::time_point<std::chrono::steady_clock> getLastHealthCheck() const;
uint32_t getWeight() const;
uint32_t getCurrentConnections() const;
uint32_t getFailureCount() const;

    // Setters
    void setHealthy(bool isHealthy);
    void updateLastHealthCheck();
    void setWeight(uint32_t weight);
    void setAlive(bool isAlive);
    
    // Connection management
    void incrementConnections();
    void decrementConnections();
    
    // Failure management
    void incrementFailures();
    void resetFailures();
    
    // Calculate effective load (for least connections algorithm)
    double getEffectiveLoad() const;
    
    friend class RoundRobinLoadBalancer;
};

#endif // SERVER_HPP_