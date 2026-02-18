#include "chat_server.h"
Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::log(Level level, const std::string& message) {
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

void Logger::debug(const std::string& msg) { log(DEBUG, msg); }
void Logger::info(const std::string& msg) { log(INFO, msg); }
void Logger::warn(const std::string& msg) { log(WARN, msg); }
void Logger::error(const std::string& msg) { log(ERROR, msg); }

Logger::Logger() {
    log_file_.open("chat_server.log", std::ios::app);
}

Logger::~Logger() {
    if (log_file_.is_open()) {
        log_file_.close();
    }
}

std::string Logger::level_to_string(Level level) {
    switch(level) {
        case DEBUG: return "DEBUG";
        case INFO:  return "INFO ";
        case WARN:  return "WARN ";
        case ERROR: return "ERROR";
        default:    return "?????";
    }
}


Metrics& Metrics::instance() {
    static Metrics metrics;
    return metrics;
}

void Metrics::increment_connections() {
    std::lock_guard<std::mutex> lock(mutex_);
    total_connections_++;
    current_connections_++;
    peak_connections_ = std::max(peak_connections_, current_connections_);
}

void Metrics::decrement_connections() {
    std::lock_guard<std::mutex> lock(mutex_);
    current_connections_--;
}

void Metrics::increment_messages() {
    std::lock_guard<std::mutex> lock(mutex_);
    total_messages_++;
}

void Metrics::increment_matches() {
    std::lock_guard<std::mutex> lock(mutex_);
    total_matches_++;
}

void Metrics::increment_skips() {
    std::lock_guard<std::mutex> lock(mutex_);
    total_skips_++;
}

void Metrics::print_stats() {
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

bool RateLimiter::check_rate_limit(const std::string& identifier) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto now = std::chrono::steady_clock::now();
    auto& entry = rate_map_[identifier];
    auto cutoff_1min = now - std::chrono::minutes(1);
    entry.erase(
        std::remove_if(entry.begin(), entry.end(),
            [cutoff_1min](const auto& time) { return time < cutoff_1min; }),
        entry.end()
    );
    
    if (entry.size() >= Config::MAX_MESSAGES_PER_MINUTE) {
        Logger::instance().warn("Rate limit exceeded (60 msg/min) for: " + identifier);
        return false;
    }
    
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

void RateLimiter::cleanup_old_entries() {
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

bool IPTracker::can_connect(const std::string& ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    int count = connection_count_[ip];
    if (count >= Config::MAX_CONNECTIONS_PER_IP) {
        Logger::instance().warn("Too many connections from IP: " + ip);
        return false;
    }
    
    connection_count_[ip]++;
    return true;
}

void IPTracker::disconnect(const std::string& ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (connection_count_[ip] > 0) {
        connection_count_[ip]--;
    }
    
    if (connection_count_[ip] == 0) {
        connection_count_.erase(ip);
    }
}

std::string make_json(const std::string& type, const std::string& message) {
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