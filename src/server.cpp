#include "server.hpp"
#include <chrono>

// Constructor
Server::Server(const std::string& serverAddress, uint32_t weight)
    : _serverAddress(serverAddress),
      _isAlive(true),
      _isHealthy(false),
      _lastHealthCheck(std::chrono::steady_clock::now()),
      _weight(weight),
      _currentConnections(0),
      _failureCount(0)
{
}

// Move constructor
Server::Server(std::string&& serverAddress, uint32_t weight)
    : _serverAddress(std::move(serverAddress)),
      _isAlive(true),
      _isHealthy(false),
      _lastHealthCheck(std::chrono::steady_clock::now()),
      _weight(weight),
      _currentConnections(0),
      _failureCount(0)
{
}

// Copy constructor
Server::Server(const Server& other)
    : _serverAddress(other._serverAddress),
      _isAlive(other._isAlive.load()),
      _isHealthy(other._isHealthy.load()),
      _weight(other._weight.load()),
      _currentConnections(0),  // Don't copy connection count
      _failureCount(other._failureCount.load())
{
    std::lock_guard<std::mutex> lock(other._timeMutex);
    _lastHealthCheck = other._lastHealthCheck;
}

// Move constructor
Server::Server(Server&& other) noexcept
    : _serverAddress(std::move(other._serverAddress)),
      _isAlive(other._isAlive.load()),
      _isHealthy(other._isHealthy.load()),
      _weight(other._weight.load()),
      _currentConnections(0),  // Don't move connection count
      _failureCount(other._failureCount.load())
{
    std::lock_guard<std::mutex> lock(other._timeMutex);
    _lastHealthCheck = other._lastHealthCheck;
}

// Copy assignment
Server& Server::operator=(const Server& other)
{
    if (this != &other) {
        _serverAddress = other._serverAddress;
        _isAlive.store(other._isAlive.load());
        _isHealthy.store(other._isHealthy.load());
        _weight.store(other._weight.load());
        _failureCount.store(other._failureCount.load());
        
        // Don't copy connection count
        
        std::lock_guard<std::mutex> lockOther(other._timeMutex);
        std::lock_guard<std::mutex> lockThis(_timeMutex);
        _lastHealthCheck = other._lastHealthCheck;
    }
    return *this;
}

// Move assignment
Server& Server::operator=(Server&& other) noexcept
{
    if (this != &other) {
        _serverAddress = std::move(other._serverAddress);
        _isAlive.store(other._isAlive.load());
        _isHealthy.store(other._isHealthy.load());
        _weight.store(other._weight.load());
        _failureCount.store(other._failureCount.load());
        
        // Don't move connection count
        
        std::lock_guard<std::mutex> lockOther(other._timeMutex);
        std::lock_guard<std::mutex> lockThis(_timeMutex);
        _lastHealthCheck = other._lastHealthCheck;
    }
    return *this;
}

// Destructor
Server::~Server() = default;

// Getters
const std::string& Server::getServerAddress() const
{
    return _serverAddress;
}

bool Server::isAlive() const
{
    return _isAlive.load();
}

bool Server::isHealthy() const
{
    return _isHealthy.load();
}

std::chrono::time_point<std::chrono::steady_clock> Server::getLastHealthCheck() const
{
    std::lock_guard<std::mutex> lock(_timeMutex);
    return _lastHealthCheck;
}

uint32_t Server::getWeight() const
{
    return _weight.load();
}

uint32_t Server::getCurrentConnections() const
{
    return _currentConnections.load();
}

uint32_t Server::getFailureCount() const
{
    return _failureCount.load();
}

// Setters
void Server::setAlive(bool isAlive)
{
    _isAlive.store(isAlive);
}

void Server::setHealthy(bool isHealthy)
{
    _isHealthy.store(isHealthy);
    
    // Reset failure count if healthy
    if (isHealthy) {
        resetFailures();
    }
}

void Server::updateLastHealthCheck()
{
    std::lock_guard<std::mutex> lock(_timeMutex);
    _lastHealthCheck = std::chrono::steady_clock::now();
}

void Server::setWeight(uint32_t weight)
{
    _weight.store(weight);
}

// Connection management
void Server::incrementConnections()
{
    _currentConnections++;
}

void Server::decrementConnections()
{
    // Prevent underflow
    uint32_t current = _currentConnections.load();
    if (current > 0) {
        _currentConnections--;
    }
}

// Failure management
void Server::incrementFailures()
{
    _failureCount++;
}

void Server::resetFailures()
{
    _failureCount.store(0);
}

// Calculate effective load
double Server::getEffectiveLoad() const
{
    uint32_t weight = _weight.load();
    if (weight == 0) weight = 1; // Prevent division by zero
    
    return static_cast<double>(_currentConnections.load()) / weight;
}