#include "round_robin_load_balancer.hpp"
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <iostream>

// LoadBalancer base class implementation

// Constructor
LoadBalancer::LoadBalancer(
    const std::vector<std::shared_ptr<Server>>& servers,
    LoadBalancingStrategy strategy,
    uint32_t healthCheckInterval,
    uint32_t maxHealthCheckFailures
) : _serversList(servers),
    _strategy(strategy),
    _healthCheckInterval(healthCheckInterval),
    _maxHealthCheckFailures(maxHealthCheckFailures),
    _healthCheckRunning(false)
{
    // Initialize ping server
    _pingServer = std::make_unique<PingServer>();
}

// Copy constructor
LoadBalancer::LoadBalancer(const LoadBalancer& other)
    : _strategy(other._strategy),
      _healthCheckRunning(false) // Always start with health checks off
{
    {
        std::shared_lock<std::shared_mutex> lock(other._serversMutex);
        _serversList = other._serversList;
    }
    
    {
        std::lock_guard<std::mutex> configLock(other._configMutex);
        _healthCheckInterval = other._healthCheckInterval;
        _maxHealthCheckFailures = other._maxHealthCheckFailures;
    }
    
    // Create a new ping server instance
    _pingServer = std::make_unique<PingServer>();
}

// Move constructor
LoadBalancer::LoadBalancer(LoadBalancer&& other) noexcept
    : _serversList(std::move(other._serversList)),
      _pingServer(std::move(other._pingServer)),
      _healthCheckRunning(false) // Always start with health checks off
{
    std::lock_guard<std::mutex> configLock(other._configMutex);
    _strategy = other._strategy;
    _healthCheckInterval = other._healthCheckInterval;
    _maxHealthCheckFailures = other._maxHealthCheckFailures;
}

// Copy assignment
LoadBalancer& LoadBalancer::operator=(const LoadBalancer& other)
{
    if (this != &other) {
        // Stop any running health checks
        stopHealthChecks();
        
        {
            // Copy servers with appropriate locks
            std::unique_lock<std::shared_mutex> lockThis(_serversMutex);
            std::shared_lock<std::shared_mutex> lockOther(other._serversMutex);
            _serversList = other._serversList;
        }
        
        {
            // Copy configuration with appropriate locks
            std::lock_guard<std::mutex> lockThis(_configMutex);
            std::lock_guard<std::mutex> lockOther(other._configMutex);
            _strategy = other._strategy;
            _healthCheckInterval = other._healthCheckInterval;
            _maxHealthCheckFailures = other._maxHealthCheckFailures;
        }
        
        // Create a new ping server
        _pingServer = std::make_unique<PingServer>();
    }
    return *this;
}

// Move assignment
LoadBalancer& LoadBalancer::operator=(LoadBalancer&& other) noexcept
{
    if (this != &other) {
        // Stop any running health checks
        stopHealthChecks();
        
        {
            // Move servers with appropriate locks
            std::unique_lock<std::shared_mutex> lock(_serversMutex);
            _serversList = std::move(other._serversList);
        }
        
        {
            // Move configuration with appropriate locks
            std::lock_guard<std::mutex> lockThis(_configMutex);
            std::lock_guard<std::mutex> lockOther(other._configMutex);
            _strategy = other._strategy;
            _healthCheckInterval = other._healthCheckInterval;
            _maxHealthCheckFailures = other._maxHealthCheckFailures;
        }
        
        // Move ping server
        _pingServer = std::move(other._pingServer);
    }
    return *this;
}

// Destructor
LoadBalancer::~LoadBalancer()
{
    stopHealthChecks();
}

// Perform health check on all servers
bool LoadBalancer::performHealthCheck()
{
    if (!_pingServer) {
        return false;
    }
    
    // Use shared lock for reading servers
    std::shared_lock<std::shared_mutex> lock(_serversMutex);
    return _pingServer->pingServers(_serversList);
}

// Server management
bool LoadBalancer::addServer(std::shared_ptr<Server> server)
{
    if (!server) {
        return false;
    }
    
    // Use exclusive lock for writing
    std::unique_lock<std::shared_mutex> lock(_serversMutex);
    
    // Check for duplicate
    auto it = std::find_if(_serversList.begin(), _serversList.end(), 
                          [&](const std::shared_ptr<Server>& s) {
                              return s->getServerAddress() == server->getServerAddress();
                          });
    
    if (it != _serversList.end()) {
        return false; // Server already exists
    }
    
    _serversList.push_back(server);
    return true;
}

// Remove a server
bool LoadBalancer::removeServer(const std::string& serverAddress)
{
    // Use exclusive lock for writing
    std::unique_lock<std::shared_mutex> lock(_serversMutex);
    
    auto originalSize = _serversList.size();
    
    _serversList.erase(
        std::remove_if(_serversList.begin(), _serversList.end(),
                      [&](const std::shared_ptr<Server>& server) {
                          return server->getServerAddress() == serverAddress;
                      }),
       _serversList.end()
    );
    
    return _serversList.size() < originalSize;
}

// Get all servers
std::vector<std::shared_ptr<Server>> LoadBalancer::getServers() const
{
    // Use shared lock for reading
    std::shared_lock<std::shared_mutex> lock(_serversMutex);
    return _serversList;
}

// Setters and getters for configuration
void LoadBalancer::setHealthCheckInterval(uint32_t milliseconds)
{
    std::lock_guard<std::mutex> lock(_configMutex);
    _healthCheckInterval = milliseconds;
}

uint32_t LoadBalancer::getHealthCheckInterval() const
{
    std::lock_guard<std::mutex> lock(_configMutex);
    return _healthCheckInterval;
}

void LoadBalancer::setMaxHealthCheckFailures(uint32_t failures)
{
    std::lock_guard<std::mutex> lock(_configMutex);
    _maxHealthCheckFailures = failures;
}

uint32_t LoadBalancer::getMaxHealthCheckFailures() const
{
    std::lock_guard<std::mutex> lock(_configMutex);
    return _maxHealthCheckFailures;
}

void LoadBalancer::setStrategy(LoadBalancingStrategy strategy)
{
    std::lock_guard<std::mutex> lock(_configMutex);
    _strategy = strategy;
}

LoadBalancingStrategy LoadBalancer::getStrategy() const
{
    std::lock_guard<std::mutex> lock(_configMutex);
    return _strategy;
}

// Start health checks
void LoadBalancer::startHealthChecks()
{
    bool expected = false;
    if (!_healthCheckRunning.compare_exchange_strong(expected, true)) {
        return; // Already running
    }
    
    // Get a copy of the interval to avoid data race
    uint32_t interval;
    {
        std::lock_guard<std::mutex> lock(_configMutex);
        interval = _healthCheckInterval;
    }
    
    _healthCheckTask = std::async(std::launch::async, [this, interval]() {
        while (_healthCheckRunning.load(std::memory_order_acquire)) {
            this->performHealthCheck();
            
            // Check for updated interval
            uint32_t currentInterval;
            {
                std::lock_guard<std::mutex> lock(_configMutex);
                currentInterval = _healthCheckInterval;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(currentInterval));
        }
    });
}

// Stop health checks
void LoadBalancer::stopHealthChecks()
{
    _healthCheckRunning.store(false, std::memory_order_release);
    
    if (_healthCheckTask.valid()) {
        try {
            _healthCheckTask.wait();
        } catch (...) {
            // Ignore exceptions during shutdown
        }
    }
}

// Check if health check is running
bool LoadBalancer::isHealthCheckRunning() const
{
    return _healthCheckRunning.load(std::memory_order_acquire);
}

// Statistics methods
size_t LoadBalancer::getServerCount() const
{
    // Use shared lock for reading
    std::shared_lock<std::shared_mutex> lock(_serversMutex);
    return _serversList.size();
}

size_t LoadBalancer::getHealthyServerCount() const
{
    // Use shared lock for reading
    std::shared_lock<std::shared_mutex> lock(_serversMutex);
    return std::count_if(_serversList.begin(), _serversList.end(),
                        [](const std::shared_ptr<Server>& server) {
                            return server->isAlive() && server->isHealthy();
                        });
}

double LoadBalancer::getAverageLoad() const
{
    // Use shared lock for reading
    std::shared_lock<std::shared_mutex> lock(_serversMutex);
    
    if (_serversList.empty()) {
        return 0.0;
    }
    
    double totalLoad = 0.0;
    size_t activeServerCount = 0;
    
    for (const auto& server : _serversList) {
        if (server->isAlive() && server->isHealthy()) {
            totalLoad += server->getEffectiveLoad();
            activeServerCount++;
        }
    }
    
    return activeServerCount > 0 ? totalLoad / activeServerCount : 0.0;
}

// RoundRobinLoadBalancer implementation

// Constructor
RoundRobinLoadBalancer::RoundRobinLoadBalancer(
    const std::vector<std::shared_ptr<Server>>& servers,
    uint32_t healthCheckInterval,
    uint32_t maxHealthCheckFailures
) : LoadBalancer(servers, LoadBalancingStrategy::ROUND_ROBIN, healthCheckInterval, maxHealthCheckFailures),
    _currentServerIndex(0)
{
    if (servers.empty()) {
        throw std::invalid_argument("Server list cannot be empty");
    }
}

// Copy constructor
RoundRobinLoadBalancer::RoundRobinLoadBalancer(const RoundRobinLoadBalancer& other)
    : LoadBalancer(other)
{
    _currentServerIndex.store(other._currentServerIndex.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

// Move constructor
RoundRobinLoadBalancer::RoundRobinLoadBalancer(RoundRobinLoadBalancer&& other) noexcept
    : LoadBalancer(std::move(other))
{
    _currentServerIndex.store(other._currentServerIndex.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

// Copy assignment
RoundRobinLoadBalancer& RoundRobinLoadBalancer::operator=(const RoundRobinLoadBalancer& other)
{
    if (this != &other) {
        LoadBalancer::operator=(other);
        _currentServerIndex.store(other._currentServerIndex.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return *this;
}

// Move assignment
RoundRobinLoadBalancer& RoundRobinLoadBalancer::operator=(RoundRobinLoadBalancer&& other) noexcept
{
    if (this != &other) {
        LoadBalancer::operator=(std::move(other));
        _currentServerIndex.store(other._currentServerIndex.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return *this;
}

// Destructor
RoundRobinLoadBalancer::~RoundRobinLoadBalancer() = default;

// Get next server using round robin algorithm
std::shared_ptr<Server> RoundRobinLoadBalancer::getNextServer()
{
    std::shared_lock<std::shared_mutex> lock(_serversMutex);
    
    if (_serversList.empty()) {
        return nullptr;
    }
    
    // First pass: find a healthy server
    size_t startIndex = _currentServerIndex.load(std::memory_order_acquire);
    size_t serverCount = _serversList.size();
    std::shared_ptr<Server> fallbackServer = nullptr;
    
    for (size_t i = 0; i < serverCount; ++i) {
        size_t index = (startIndex + i) % serverCount;
        auto& server = _serversList[index];
        
        if (server->isAlive()) {
            if (server->isHealthy()) {
                // Found a healthy server, update index and return it
                _currentServerIndex.store((index + 1) % serverCount, std::memory_order_release);
                return server;
            } else if (!fallbackServer) {
                // Keep first alive but unhealthy server as fallback
                fallbackServer = server;
            }
        }
    }
    
    // If we found at least an alive but unhealthy server, use it
    if (fallbackServer) {
        // Find this server's index to update the counter
        for (size_t i = 0; i < serverCount; ++i) {
            if (_serversList[i] == fallbackServer) {
                _currentServerIndex.store((i + 1) % serverCount, std::memory_order_release);
                break;
            }
        }
        return fallbackServer;
    }
    
    // No available servers
    return nullptr;
}

// Update current server (internal implementation detail)
void RoundRobinLoadBalancer::updateCurrentServer()
{
    // Use shared lock for reading the server count
    std::shared_lock<std::shared_mutex> lock(_serversMutex);
    
    if (_serversList.empty()) {
        return;
    }
    
    size_t index = _currentServerIndex.load(std::memory_order_relaxed);
    _currentServerIndex.store((index + 1) % _serversList.size(), std::memory_order_relaxed);
}

// WeightedRoundRobinLoadBalancer implementation

// Constructor
WeightedRoundRobinLoadBalancer::WeightedRoundRobinLoadBalancer(
    const std::vector<std::shared_ptr<Server>>& servers,
    uint32_t healthCheckInterval,
    uint32_t maxHealthCheckFailures
) : LoadBalancer(servers, LoadBalancingStrategy::WEIGHTED_ROUND_ROBIN, healthCheckInterval, maxHealthCheckFailures),
    _currentServerIndex(0)
{
    if (servers.empty()) {
        throw std::invalid_argument("Server list cannot be empty");
    }
    
    updateWeightedList();
}

// Copy constructor
WeightedRoundRobinLoadBalancer::WeightedRoundRobinLoadBalancer(const WeightedRoundRobinLoadBalancer& other)
    : LoadBalancer(other),
      _currentServerIndex(other._currentServerIndex.load(std::memory_order_relaxed))
{
    std::lock_guard<std::mutex> lock(other._weightedListMutex);
    _weightedServersList = other._weightedServersList;
}

// Move constructor
WeightedRoundRobinLoadBalancer::WeightedRoundRobinLoadBalancer(WeightedRoundRobinLoadBalancer&& other) noexcept
    : LoadBalancer(std::move(other)),
      _currentServerIndex(other._currentServerIndex.load(std::memory_order_relaxed))
{
    std::lock_guard<std::mutex> lock(other._weightedListMutex);
    _weightedServersList = std::move(other._weightedServersList);
}

// Copy assignment operator
WeightedRoundRobinLoadBalancer& WeightedRoundRobinLoadBalancer::operator=(const WeightedRoundRobinLoadBalancer& other)
{
    if (this != &other) {
        LoadBalancer::operator=(other);
        _currentServerIndex.store(other._currentServerIndex.load(std::memory_order_relaxed), std::memory_order_relaxed);
        
        // Use std::scoped_lock for multiple mutex acquisition (C++17)
        std::scoped_lock lock(_weightedListMutex, other._weightedListMutex);
        _weightedServersList = other._weightedServersList;
    }
    return *this;
}

// Move assignment operator
WeightedRoundRobinLoadBalancer& WeightedRoundRobinLoadBalancer::operator=(WeightedRoundRobinLoadBalancer&& other) noexcept
{
    if (this != &other) {
        LoadBalancer::operator=(std::move(other));
        _currentServerIndex.store(other._currentServerIndex.load(std::memory_order_relaxed), std::memory_order_relaxed);
        
        // Use std::scoped_lock for multiple mutex acquisition (C++17)
        std::scoped_lock lock(_weightedListMutex, other._weightedListMutex);
        _weightedServersList = std::move(other._weightedServersList);
    }
    return *this;
}

// Destructor
WeightedRoundRobinLoadBalancer::~WeightedRoundRobinLoadBalancer() = default;

// Update the weighted servers list
void WeightedRoundRobinLoadBalancer::updateWeightedList()
{
    // First get a copy of the servers list with shared lock
    std::vector<std::shared_ptr<Server>> serversCopy;
    {
        std::shared_lock<std::shared_mutex> lock(_serversMutex);
        serversCopy = _serversList;
    }
    
    // Then update the weighted list with exclusive lock
    std::lock_guard<std::mutex> lock(_weightedListMutex);
    _weightedServersList.clear();
    
    for (const auto& server : serversCopy) {
        // Only include servers that are alive
        if (server->isAlive()) {
            uint32_t weight = server->getWeight();
            for (uint32_t i = 0; i < weight; ++i) {
                _weightedServersList.push_back(server);
            }
        }
    }
}

// Get next server using weighted round robin algorithm
std::shared_ptr<Server> WeightedRoundRobinLoadBalancer::getNextServer()
{
    // First check if we need to update the weighted list
    {
        std::shared_lock<std::shared_mutex> serversLock(_serversMutex);
        std::unique_lock<std::mutex> weightedLock(_weightedListMutex);
        
        // If the weighted list is empty or servers have changed, update it
        if (_weightedServersList.empty() || _weightedServersList.size() == 0) {
            // Release locks in correct order
            serversLock.unlock();
            weightedLock.unlock();
            
            // Update the weighted list
            updateWeightedList();
        }
    }
    
    // Now get a server from the weighted list
    std::lock_guard<std::mutex> lock(_weightedListMutex);
    
    if (_weightedServersList.empty()) {
        return nullptr; // No servers available
    }
    
    size_t startIndex = _currentServerIndex.load(std::memory_order_acquire);
    size_t serverCount = _weightedServersList.size();
    std::shared_ptr<Server> fallbackServer = nullptr;
    
    // First try to find a healthy server
    for (size_t i = 0; i < serverCount; ++i) {
        size_t index = (startIndex + i) % serverCount;
        auto& server = _weightedServersList[index];
        
        if (server->isAlive()) {
            if (server->isHealthy()) {
                // Found a healthy server, update index and return it
                _currentServerIndex.store((index + 1) % serverCount, std::memory_order_release);
                return server;
            } else if (!fallbackServer) {
                // Keep first alive but unhealthy server as fallback
                fallbackServer = server;
            }
        }
    }
    
    // If we found at least an alive but unhealthy server, use it
    if (fallbackServer) {
        // Find this server's index to update the counter
        for (size_t i = 0; i < serverCount; ++i) {
            if (_weightedServersList[i] == fallbackServer) {
                _currentServerIndex.store((i + 1) % serverCount, std::memory_order_release);
                break;
            }
        }
        return fallbackServer;
    }
    
    // No available servers, trigger an update of the weighted list
    return nullptr;
}