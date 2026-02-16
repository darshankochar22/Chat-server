#ifndef CHAT_SERVER_H
#define CHAT_SERVER_H

#include <iostream>
#include <memory>
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <string>
#include <chrono>
#include <thread>
#include <fstream>
#include <iomanip>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using boost::property_tree::ptree;

// Configuration constants
struct Config {
    static constexpr int MAX_MESSAGE_LENGTH = 500;  
    static constexpr int MAX_MESSAGES_PER_MINUTE = 60;  
    static constexpr int MAX_MESSAGES_PER_10_SECONDS = 20; 
    static constexpr int SESSION_TIMEOUT_SECONDS = 1800; 
    static constexpr int HEARTBEAT_INTERVAL_MS = 30000;   
    static constexpr int HEARTBEAT_TIMEOUT_MS = 120000;  
    static constexpr int CLEANUP_INTERVAL_SECONDS = 60;
    static constexpr int MAX_CONNECTIONS_PER_IP = 5;  
};

// Logger class for centralized logging
class Logger {
public:
    enum Level { DEBUG, INFO, WARN, ERROR };
    
    static Logger& instance();
    void log(Level level, const std::string& message);
    void debug(const std::string& msg);
    void info(const std::string& msg);
    void warn(const std::string& msg);
    void error(const std::string& msg);
    
private:
    Logger();
    ~Logger();
    std::string level_to_string(Level level);
    
    std::mutex mutex_;
    std::ofstream log_file_;
};

// Metrics tracking class
class Metrics {
public:
    static Metrics& instance();
    
    void increment_connections();
    void decrement_connections();
    void increment_messages();
    void increment_matches();
    void increment_skips();
    void print_stats();
    
private:
    std::mutex mutex_;
    int current_connections_ = 0;
    int peak_connections_ = 0;
    int total_connections_ = 0;
    int total_messages_ = 0;
    int total_matches_ = 0;
    int total_skips_ = 0;
};

// Rate limiting class
class RateLimiter {
public:
    bool check_rate_limit(const std::string& identifier);
    void cleanup_old_entries();
    
private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> rate_map_;
};

// IP connection tracking class
class IPTracker {
public:
    bool can_connect(const std::string& ip);
    void disconnect(const std::string& ip);
    
private:
    std::mutex mutex_;
    std::unordered_map<std::string, int> connection_count_;
};

// Utility function for JSON generation
std::string make_json(const std::string& type, const std::string& message = "");

// Session class forward declaration
class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket, const std::string& ip);
    
    void start();
    void send(const std::string& msg);
    void set_partner(std::shared_ptr<Session> partner);
    void clear_partner();
    bool is_closed() const;
    bool is_timed_out() const;
    bool is_heartbeat_timeout() const;
    void update_activity();
    std::string get_ip() const;
    std::string get_session_id() const;

private:
    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    std::shared_ptr<Session> partner_;
    mutable std::mutex session_mutex_;
    bool closed_ = false;
    std::deque<std::string> write_queue_;
    
    std::string ip_address_;
    std::string session_id_;
    std::chrono::steady_clock::time_point last_activity_;
    std::chrono::steady_clock::time_point last_heartbeat_;
    
    static std::string generate_session_id();
    void start_heartbeat();
    void send_ping();
    void try_match();
    void do_write();
    void do_read();
    void handle_json(const std::string& data);
    void close();
};

// Global state
extern std::deque<std::shared_ptr<Session>> waiting_pool;
extern std::unordered_set<std::shared_ptr<Session>> sessions;
extern std::mutex global_mutex;

// Cleanup thread function
void cleanup_thread(asio::io_context& io);

// Server class
class Server {
public:
    Server(asio::io_context& io, short port);
    ~Server();

private:
    tcp::acceptor acceptor_;
    asio::io_context& io_context_;
    std::thread cleanup_thread_;
    
    void do_accept();
};

#endif // CHAT_SERVER_H