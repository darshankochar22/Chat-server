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

// ============================================================================
// CONFIGURATION
// ============================================================================
struct Config {
    // Message limits - prevents crash attacks, not normal users
    static constexpr int MAX_MESSAGE_LENGTH = 500;  // ~3-4 sentences, prevents huge text bombs
    
    // Spam protection - only blocks EXTREME abuse (bots sending 100+ msg/sec)
    static constexpr int MAX_MESSAGES_PER_MINUTE = 60;  // 1 per second avg = OK for fast typers
    static constexpr int MAX_MESSAGES_PER_10_SECONDS = 20;  // Burst protection: max 20 in 10 sec
    
    // Session timeouts - very generous, won't disconnect normal users
    static constexpr int SESSION_TIMEOUT_SECONDS = 1800;  // 30 minutes idle = disconnect
    static constexpr int HEARTBEAT_INTERVAL_MS = 30000;   // 30 seconds ping
    static constexpr int HEARTBEAT_TIMEOUT_MS = 120000;   // 2 minutes no response = dead
    
    // Cleanup and connection limits
    static constexpr int CLEANUP_INTERVAL_SECONDS = 60;
    static constexpr int MAX_CONNECTIONS_PER_IP = 5;  // Allows family/office, blocks bot farms
};

// ============================================================================
// LOGGER
// ============================================================================
class Logger {
public:
    enum Level { DEBUG, INFO, WARN, ERROR };
    
    static Logger& instance() {
        static Logger logger;
        return logger;
    }
    
    void log(Level level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        oss << " [" << level_to_string(level) << "] " << message << std::endl;
        
        std::string log_line = oss.str();
        std::cout << log_line;
        
        if (log_file_.is_open()) {
            log_file_ << log_line;
            log_file_.flush();
        }
    }
    
    void debug(const std::string& msg) { log(DEBUG, msg); }
    void info(const std::string& msg) { log(INFO, msg); }
    void warn(const std::string& msg) { log(WARN, msg); }
    void error(const std::string& msg) { log(ERROR, msg); }
    
private:
    Logger() {
        log_file_.open("chat_server.log", std::ios::app);
    }
    
    ~Logger() {
        if (log_file_.is_open()) {
            log_file_.close();
        }
    }
    
    std::string level_to_string(Level level) {
        switch(level) {
            case DEBUG: return "DEBUG";
            case INFO:  return "INFO ";
            case WARN:  return "WARN ";
            case ERROR: return "ERROR";
            default:    return "?????";
        }
    }
    
    std::mutex mutex_;
    std::ofstream log_file_;
};

// ============================================================================
// METRICS & ANALYTICS
// ============================================================================
class Metrics {
public:
    static Metrics& instance() {
        static Metrics metrics;
        return metrics;
    }
    
    void increment_connections() {
        std::lock_guard<std::mutex> lock(mutex_);
        total_connections_++;
        current_connections_++;
        peak_connections_ = std::max(peak_connections_, current_connections_);
    }
    
    void decrement_connections() {
        std::lock_guard<std::mutex> lock(mutex_);
        current_connections_--;
    }
    
    void increment_messages() {
        std::lock_guard<std::mutex> lock(mutex_);
        total_messages_++;
    }
    
    void increment_matches() {
        std::lock_guard<std::mutex> lock(mutex_);
        total_matches_++;
    }
    
    void increment_skips() {
        std::lock_guard<std::mutex> lock(mutex_);
        total_skips_++;
    }
    
    void print_stats() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream oss;
        oss << "\n========== SERVER STATISTICS ==========\n"
            << "Current connections: " << current_connections_ << "\n"
            << "Peak connections: " << peak_connections_ << "\n"
            << "Total connections: " << total_connections_ << "\n"
            << "Total messages: " << total_messages_ << "\n"
            << "Total matches: " << total_matches_ << "\n"
            << "Total skips: " << total_skips_ << "\n"
            << "======================================\n";
        Logger::instance().info(oss.str());
    }
    
private:
    std::mutex mutex_;
    int current_connections_ = 0;
    int peak_connections_ = 0;
    int total_connections_ = 0;
    int total_messages_ = 0;
    int total_matches_ = 0;
    int total_skips_ = 0;
};

// ============================================================================
// RATE LIMITER
// ============================================================================
class RateLimiter {
public:
    bool check_rate_limit(const std::string& identifier) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto now = std::chrono::steady_clock::now();
        auto& entry = rate_map_[identifier];
        
        // Remove old timestamps (older than 1 minute)
        auto cutoff_1min = now - std::chrono::minutes(1);
        entry.erase(
            std::remove_if(entry.begin(), entry.end(),
                [cutoff_1min](const auto& time) { return time < cutoff_1min; }),
            entry.end()
        );
        
        // Check 1-minute limit (sustained spam)
        if (entry.size() >= Config::MAX_MESSAGES_PER_MINUTE) {
            Logger::instance().warn("Rate limit exceeded (60 msg/min) for: " + identifier);
            return false;
        }
        
        // Check 10-second burst limit (rapid fire spam)
        auto cutoff_10sec = now - std::chrono::seconds(10);
        int recent_count = std::count_if(entry.begin(), entry.end(),
            [cutoff_10sec](const auto& time) { return time >= cutoff_10sec; });
        
        if (recent_count >= Config::MAX_MESSAGES_PER_10_SECONDS) {
            Logger::instance().warn("Burst rate limit exceeded (20 msg/10sec) for: " + identifier);
            return false;
        }
        
        entry.push_back(now);
        return true;
    }
    
    void cleanup_old_entries() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto now = std::chrono::steady_clock::now();
        auto cutoff = now - std::chrono::minutes(2);
        
        for (auto it = rate_map_.begin(); it != rate_map_.end();) {
            if (it->second.empty() || it->second.back() < cutoff) {
                it = rate_map_.erase(it);
            } else {
                ++it;
            }
        }
    }
    
private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> rate_map_;
};

// ============================================================================
// IP CONNECTION TRACKER
// ============================================================================
class IPTracker {
public:
    bool can_connect(const std::string& ip) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        int count = connection_count_[ip];
        if (count >= Config::MAX_CONNECTIONS_PER_IP) {
            Logger::instance().warn("Too many connections from IP: " + ip);
            return false;
        }
        
        connection_count_[ip]++;
        return true;
    }
    
    void disconnect(const std::string& ip) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (connection_count_[ip] > 0) {
            connection_count_[ip]--;
        }
        
        if (connection_count_[ip] == 0) {
            connection_count_.erase(ip);
        }
    }
    
private:
    std::mutex mutex_;
    std::unordered_map<std::string, int> connection_count_;
};

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================
std::string make_json(const std::string& type, const std::string& message = "") {
    ptree tree;
    tree.put("type", type);
    if (!message.empty()) tree.put("message", message);
    
    std::ostringstream buf;
    write_json(buf, tree, false);
    std::string result = buf.str();
    
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    
    return result;
}

// ============================================================================
// SESSION CLASS
// ============================================================================
class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket, const std::string& ip)
        : ws_(std::move(socket))
        , ip_address_(ip)
        , session_id_(generate_session_id())
        , last_activity_(std::chrono::steady_clock::now())
        , last_heartbeat_(std::chrono::steady_clock::now()) {
    }
    
    void start() {
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) {
                res.set(beast::http::field::server, "AnonymousChat/2.0");
            }));
        
        ws_.async_accept([self = shared_from_this()](beast::error_code ec) {
            if (!ec) {
                Logger::instance().info("New connection: " + self->session_id_ + " from " + self->ip_address_);
                Metrics::instance().increment_connections();
                
                self->send(make_json("info", "Welcome to Anonymous Chat"));
                self->send(make_json("session_id", self->session_id_));
                self->try_match();
                self->do_read();
                self->start_heartbeat();
            } else {
                Logger::instance().error("WebSocket accept error: " + ec.message());
            }
        });
    }
    
    void send(const std::string& msg) {
        auto self(shared_from_this());
        
        asio::post(ws_.get_executor(), [this, self, msg]() {
            bool write_in_progress = !write_queue_.empty();
            write_queue_.push_back(msg);
            
            if (!write_in_progress) {
                do_write();
            }
        });
    }
    
    void set_partner(std::shared_ptr<Session> partner) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        partner_ = partner;
    }
    
    void clear_partner() {
        std::lock_guard<std::mutex> lock(session_mutex_);
        partner_.reset();
    }
    
    bool is_closed() const {
        std::lock_guard<std::mutex> lock(session_mutex_);
        return closed_;
    }
    
    bool is_timed_out() const {
        std::lock_guard<std::mutex> lock(session_mutex_);
        auto now = std::chrono::steady_clock::now();
        auto inactive_duration = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_activity_).count();
        return inactive_duration > Config::SESSION_TIMEOUT_SECONDS;
    }
    
    bool is_heartbeat_timeout() const {
        std::lock_guard<std::mutex> lock(session_mutex_);
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_heartbeat_).count();
        return duration > Config::HEARTBEAT_TIMEOUT_MS;
    }
    
    void update_activity() {
        std::lock_guard<std::mutex> lock(session_mutex_);
        last_activity_ = std::chrono::steady_clock::now();
    }
    
    std::string get_ip() const { return ip_address_; }
    std::string get_session_id() const { return session_id_; }

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
    
    static std::string generate_session_id() {
        static std::atomic<int> counter{0};
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        return "session_" + std::to_string(now) + "_" + std::to_string(counter++);
    }
    
    void start_heartbeat() {
        send_ping();
    }
    
    void send_ping() {
        auto self(shared_from_this());
        
        if (is_closed()) return;
        
        send(make_json("ping"));
        
        // Schedule next ping
        auto timer = std::make_shared<asio::steady_timer>(
            ws_.get_executor(),
            std::chrono::milliseconds(Config::HEARTBEAT_INTERVAL_MS)
        );
        
        timer->async_wait([this, self, timer](beast::error_code ec) {
            if (!ec && !is_closed()) {
                send_ping();
            }
        });
    }
    
    void try_match();
    
    void do_write() {
        auto self(shared_from_this());
        
        if (write_queue_.empty()) return;
        
        ws_.async_write(
            asio::buffer(write_queue_.front()),
            [this, self](beast::error_code ec, std::size_t) {
                if (!ec) {
                    write_queue_.pop_front();
                    if (!write_queue_.empty()) {
                        do_write();
                    }
                } else {
                    Logger::instance().error("Write error for " + session_id_ + ": " + ec.message());
                    close();
                }
            });
    }
    
    void do_read() {
        auto self(shared_from_this());
        
        ws_.async_read(
            buffer_,
            [this, self](beast::error_code ec, std::size_t bytes_transferred) {
                if (!ec) {
                    std::string message = beast::buffers_to_string(buffer_.data());
                    buffer_.consume(bytes_transferred);
                    
                    update_activity();
                    handle_json(message);
                    do_read();
                } else {
                    if (ec != websocket::error::closed) {
                        Logger::instance().error("Read error for " + session_id_ + ": " + ec.message());
                    }
                    close();
                }
            });
    }
    
    void handle_json(const std::string& data) {
        try {
            std::stringstream ss(data);
            ptree tree;
            read_json(ss, tree);
            
            std::string type = tree.get<std::string>("type");
            
            if (type == "pong") {
                std::lock_guard<std::mutex> lock(session_mutex_);
                last_heartbeat_ = std::chrono::steady_clock::now();
                return;
            }
            
            if (type == "msg") {
                std::string text = tree.get<std::string>("text", "");
                
                // INPUT VALIDATION
                if (text.empty()) {
                    send(make_json("error", "Empty message"));
                    Logger::instance().warn("Empty message from " + session_id_);
                    return;
                }
                
                if (text.length() > Config::MAX_MESSAGE_LENGTH) {
                    send(make_json("error", "Message too long (max " + 
                          std::to_string(Config::MAX_MESSAGE_LENGTH) + " chars)"));
                    Logger::instance().warn("Message too long from " + session_id_);
                    return;
                }
                
                // RATE LIMITING
                if (!RateLimiter().check_rate_limit(session_id_)) {
                    send(make_json("error", "Slow down! You're sending messages too fast."));
                    return;
                }
                
                std::shared_ptr<Session> partner;
                {
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    partner = partner_;
                }
                
                if (partner && !partner->is_closed()) {
                    partner->send(make_json("chat", text));
                    Metrics::instance().increment_messages();
                    Logger::instance().debug("Message from " + session_id_ + " to partner");
                } else {
                    send(make_json("error", "Not matched"));
                }
            }
            else if (type == "skip") {
                std::shared_ptr<Session> old_partner;
                
                {
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    if (partner_) {
                        old_partner = partner_;
                        partner_.reset();
                        old_partner->clear_partner();
                    }
                }
                
                if (old_partner && !old_partner->is_closed()) {
                    old_partner->send(make_json("info", "Partner skipped"));
                    old_partner->try_match();
                }
                
                Metrics::instance().increment_skips();
                Logger::instance().info("Skip from " + session_id_);
                try_match();
            }
            else if (type == "typing") {
                bool is_typing = tree.get<bool>("status", false);
                
                std::shared_ptr<Session> partner;
                {
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    partner = partner_;
                }
                
                if (partner && !partner->is_closed()) {
                    ptree response;
                    response.put("type", "typing");
                    response.put("status", is_typing);
                    
                    std::ostringstream buf;
                    write_json(buf, response, false);
                    std::string result = buf.str();
                    if (!result.empty() && result.back() == '\n') result.pop_back();
                    
                    partner->send(result);
                }
            }
            else {
                send(make_json("error", "Unknown message type"));
                Logger::instance().warn("Unknown type from " + session_id_ + ": " + type);
            }
            
        } catch (const std::exception& e) {
            send(make_json("error", "Invalid JSON"));
            Logger::instance().error("JSON parse error from " + session_id_ + ": " + e.what());
        }
    }
    
    void close() {
        std::shared_ptr<Session> partner_copy;
        
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            
            if (closed_) return;
            closed_ = true;
            
            if (partner_) {
                partner_copy = partner_;
                partner_.reset();
            }
        }
        
        if (partner_copy) {
            partner_copy->clear_partner();
            
            if (!partner_copy->is_closed()) {
                partner_copy->send(make_json("info", "Partner disconnected"));
                partner_copy->try_match();
            }
        }
        
        beast::error_code ec;
        ws_.close(websocket::close_code::normal, ec);
        
        IPTracker().disconnect(ip_address_);
        Metrics::instance().decrement_connections();
        Logger::instance().info("Connection closed: " + session_id_);
    }
};

// ============================================================================
// GLOBAL STATE
// ============================================================================
std::deque<std::shared_ptr<Session>> waiting_pool;
std::unordered_set<std::shared_ptr<Session>> sessions;
std::mutex global_mutex;

void Session::try_match() {
    std::lock_guard<std::mutex> lock(global_mutex);
    
    {
        std::lock_guard<std::mutex> slock(session_mutex_);
        if (closed_) return;
    }
    
    // Clean up closed sessions from waiting pool
    waiting_pool.erase(
        std::remove_if(waiting_pool.begin(), waiting_pool.end(),
            [](const std::shared_ptr<Session>& s) { return s->is_closed(); }),
        waiting_pool.end()
    );
    
    if (!waiting_pool.empty()) {
        auto partner = waiting_pool.front();
        waiting_pool.pop_front();
        
        if (partner->is_closed()) {
            try_match();
            return;
        }
        
        {
            std::lock_guard<std::mutex> slock(session_mutex_);
            partner_ = partner;
        }
        partner->set_partner(shared_from_this());
        
        send(make_json("status", "Matched"));
        partner->send(make_json("status", "Matched"));
        
        Metrics::instance().increment_matches();
        Logger::instance().info("Matched: " + session_id_ + " with " + partner->get_session_id());
    } else {
        waiting_pool.push_back(shared_from_this());
        send(make_json("status", "Waiting"));
        Logger::instance().debug("Waiting: " + session_id_);
    }
}

// ============================================================================
// CLEANUP THREAD
// ============================================================================
void cleanup_thread(asio::io_context& io) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(Config::CLEANUP_INTERVAL_SECONDS));
        
        std::vector<std::shared_ptr<Session>> to_remove;
        
        {
            std::lock_guard<std::mutex> lock(global_mutex);
            
            // Find timed out sessions
            for (const auto& session : sessions) {
                if (session->is_timed_out() || session->is_heartbeat_timeout()) {
                    to_remove.push_back(session);
                }
            }
            
            // Remove them
            for (const auto& session : to_remove) {
                sessions.erase(session);
                Logger::instance().info("Removed timed out session: " + session->get_session_id());
            }
        }
        
        // Cleanup rate limiter
        RateLimiter().cleanup_old_entries();
        
        // Print stats
        Metrics::instance().print_stats();
    }
}

// ============================================================================
// SERVER CLASS
// ============================================================================
class Server {
public:
    Server(asio::io_context& io, short port)
        : acceptor_(io, tcp::endpoint(tcp::v4(), port))
        , io_context_(io) {
        
        // Start cleanup thread
        cleanup_thread_ = std::thread([&io]() {
            cleanup_thread(io);
        });
        
        do_accept();
    }
    
    ~Server() {
        if (cleanup_thread_.joinable()) {
            cleanup_thread_.detach();
        }
    }

private:
    tcp::acceptor acceptor_;
    asio::io_context& io_context_;
    std::thread cleanup_thread_;
    
    void do_accept() {
        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::string ip = socket.remote_endpoint().address().to_string();
                    
                    // Check IP connection limit
                    if (!IPTracker().can_connect(ip)) {
                        Logger::instance().warn("Rejected connection from " + ip + " (too many connections)");
                        socket.close();
                    } else {
                        auto session = std::make_shared<Session>(std::move(socket), ip);
                        
                        {
                            std::lock_guard<std::mutex> lock(global_mutex);
                            sessions.insert(session);
                        }
                        
                        session->start();
                    }
                } else {
                    Logger::instance().error("Accept error: " + ec.message());
                }
                
                do_accept();
            });
    }
};

// ============================================================================
// MAIN
// ============================================================================
int main() {
    try {
        Logger::instance().info("===========================================");
        Logger::instance().info("Anonymous Chat Server v2.0 Starting...");
        Logger::instance().info("===========================================");
        
        Logger::instance().info("Configuration:");
        Logger::instance().info("  - Max message length: " + std::to_string(Config::MAX_MESSAGE_LENGTH) + " chars");
        Logger::instance().info("  - Rate limit: " + std::to_string(Config::MAX_MESSAGES_PER_MINUTE) + " msg/min (avg 1/sec)");
        Logger::instance().info("  - Burst limit: " + std::to_string(Config::MAX_MESSAGES_PER_10_SECONDS) + " msg/10sec");
        Logger::instance().info("  - Session timeout: " + std::to_string(Config::SESSION_TIMEOUT_SECONDS / 60) + " minutes idle");
        Logger::instance().info("  - Heartbeat interval: " + std::to_string(Config::HEARTBEAT_INTERVAL_MS / 1000) + " seconds");
        Logger::instance().info("  - Max connections per IP: " + std::to_string(Config::MAX_CONNECTIONS_PER_IP));
        
        asio::io_context io;
        Server server(io, 8080);
        
        Logger::instance().info("Server running on port 8080");
        Logger::instance().info("Logging to: chat_server.log");
        Logger::instance().info("===========================================");
        
        io.run();
        
    } catch (const std::exception& e) {
        Logger::instance().error("Fatal error: " + std::string(e.what()));
        return 1;
    }
    
    return 0;
}