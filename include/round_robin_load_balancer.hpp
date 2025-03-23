#ifndef ROUND_ROBIN_LOAD_BALANCER_HPP_
#define ROUND_ROBIN_LOAD_BALANCER_HPP_

#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <memory>
#include <map>
#include <random>
#include <functional>
#include <shared_mutex>
#include <future>
#include <optional>
#include "server.hpp"
#include "ping_server.hpp"

// Load balancing strategy enum
enum class LoadBalancingStrategy {
    ROUND_ROBIN,
    WEIGHTED_ROUND_ROBIN,
    LEAST_CONNECTIONS,
    IP_HASH
};

class LoadBalancer {
protected:
    std::vector<std::shared_ptr<Server>>                    _serversList;                                           // List of server objects
    mutable std::shared_mutex                               _serversMutex;                                          // Read-write lock for server list
    std::unique_ptr<PingServer>                             _pingServer;                                            // Ping server instance
    
    // Health check configuration
    mutable std::mutex                                      _configMutex;                                           // Mutex for configuration parameters
    uint32_t                                                _healthCheckInterval;                                   // Base interval between health checks (ms)
    uint32_t                                                _maxHealthCheckFailures;                                // Maximum number of health check failures allowed
    
    // Load balancing strategy
    LoadBalancingStrategy                                   _strategy;
    
    // Background health check task
    std::atomic<bool>                                       _healthCheckRunning{false};
    std::future<void>                                       _healthCheckTask;

public:
    // Constructor
    explicit LoadBalancer(
        const std::vector<std::shared_ptr<Server>>& servers,
        LoadBalancingStrategy strategy = LoadBalancingStrategy::ROUND_ROBIN,
        uint32_t healthCheckInterval = 5000,
        uint32_t maxHealthCheckFailures = 3
    );
    
    // Rule of five
    LoadBalancer(const LoadBalancer& other);
    LoadBalancer(LoadBalancer&& other) noexcept;
    LoadBalancer& operator=(const LoadBalancer& other);
    LoadBalancer& operator=(LoadBalancer&& other) noexcept;
    
    // Virtual destructor
    virtual ~LoadBalancer();
    
    // Core functionality
    virtual std::shared_ptr<Server> getNextServer() = 0;                                                            // Pure virtual method
    bool performHealthCheck();                                                                                      // Perform health check on all servers
    
    // Server management
    bool addServer(std::shared_ptr<Server> server);                                                                 // Add a new server
    bool removeServer(const std::string& serverAddress);                                                            // Remove a server
    std::vector<std::shared_ptr<Server>> getServers() const;                                                        // Get all servers
    
    // Configuration
    void setHealthCheckInterval(uint32_t milliseconds);
    uint32_t getHealthCheckInterval() const;
    void setMaxHealthCheckFailures(uint32_t failures);
    uint32_t getMaxHealthCheckFailures() const;
    void setStrategy(LoadBalancingStrategy strategy);
    LoadBalancingStrategy getStrategy() const;
    
    // Health check control
    void startHealthChecks();
    void stopHealthChecks();
    bool isHealthCheckRunning() const;
    
    // Statistics
    size_t getServerCount() const;
    size_t getHealthyServerCount() const;
    double getAverageLoad() const;
};

// Concrete implementation for Round Robin
class RoundRobinLoadBalancer : public LoadBalancer {
private:
    std::atomic<size_t>                                     _currentServerIndex{0};                             // Index of the current server

public:
    // Constructor with default values
    explicit RoundRobinLoadBalancer(
        const std::vector<std::shared_ptr<Server>>& servers,
        uint32_t healthCheckInterval = 5000,
        uint32_t maxHealthCheckFailures = 3
    );
    
    // Rule of five
    RoundRobinLoadBalancer(const RoundRobinLoadBalancer& other);
    RoundRobinLoadBalancer(RoundRobinLoadBalancer&& other) noexcept;
    RoundRobinLoadBalancer& operator=(const RoundRobinLoadBalancer& other);
    RoundRobinLoadBalancer& operator=(RoundRobinLoadBalancer&& other) noexcept;
    ~RoundRobinLoadBalancer() override;
    
    // Implementation of the virtual method
    std::shared_ptr<Server> getNextServer() override;
    
    // Helper method for updating current server index
    void updateCurrentServer();
};

// Weighted Round Robin Load Balancer
class WeightedRoundRobinLoadBalancer : public LoadBalancer {
private:
    std::atomic<size_t>                                     _currentServerIndex{0};
    std::vector<std::shared_ptr<Server>>                    _weightedServersList;                               // List with duplicates based on weight
    mutable std::mutex                                      _weightedListMutex;                                 // Mutex for weighted list
    
    // Update the weighted servers list
    void updateWeightedList();

public:
    // Constructor
    explicit WeightedRoundRobinLoadBalancer(
        const std::vector<std::shared_ptr<Server>>& servers,
        uint32_t healthCheckInterval = 5000,
        uint32_t maxHealthCheckFailures = 3
    );
    
    // Copy/move constructors and assignment operators
    WeightedRoundRobinLoadBalancer(const WeightedRoundRobinLoadBalancer& other);
    WeightedRoundRobinLoadBalancer(WeightedRoundRobinLoadBalancer&& other) noexcept;
    WeightedRoundRobinLoadBalancer& operator=(const WeightedRoundRobinLoadBalancer& other);
    WeightedRoundRobinLoadBalancer& operator=(WeightedRoundRobinLoadBalancer&& other) noexcept;
    ~WeightedRoundRobinLoadBalancer() override;
    
    // Implementation of the virtual method
    std::shared_ptr<Server> getNextServer() override;
};

#endif // ROUND_ROBIN_LOAD_BALANCER_HPP_