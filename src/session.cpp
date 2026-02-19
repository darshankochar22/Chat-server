#include "chat_server.h"
namespace ssl = boost::asio::ssl;

std::deque<std::shared_ptr<Session>> waiting_pool;
std::unordered_set<std::shared_ptr<Session>> sessions;
std::mutex global_mutex;

// ─── Constructor ─────────────────────────────────────────────────────────────
Session::Session(tcp::socket socket, ssl::context& ctx, const std::string& ip)
    : ws_(std::move(socket), ctx)
    , ip_address_(ip)
    , session_id_(generate_session_id())
    , browser_token_("")
    , last_activity_(std::chrono::steady_clock::now())
    , last_heartbeat_(std::chrono::steady_clock::now()) {
}

// ─── Start ───────────────────────────────────────────────────────────────────
void Session::start() {
    auto self(shared_from_this());
    ws_.next_layer().async_handshake(
        ssl::stream_base::server,
        [self](beast::error_code ec) {
            if (ec) {
                Logger::instance().error("SSL Handshake error: " + ec.message());
                return;
            }

            self->ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
            self->ws_.set_option(websocket::stream_base::decorator(
                [](websocket::response_type& res) {
                    res.set(beast::http::field::server, "AnonymousChat/2.0");
                }));

            self->ws_.async_accept([self](beast::error_code ec) {
                if (!ec) {
                    Logger::instance().info("New Secure connection: " + self->session_id_ + " from " + self->ip_address_);
                    Metrics::instance().increment_connections();
                    self->send(make_json("info", "Welcome to Anonymous Chat (Secure)"));
                    self->send(make_json("session_id", self->session_id_));
                    self->do_read();
                    self->start_heartbeat();
                    // NOTE: try_match() is now called after identify, not here
                } else {
                    Logger::instance().error("WebSocket accept error: " + ec.message());
                }
            });
        });
}

// ─── Send ────────────────────────────────────────────────────────────────────
void Session::send(const std::string& msg) {
    auto self(shared_from_this());
    asio::post(beast::get_lowest_layer(ws_).get_executor(), [this, self, msg]() {
        bool write_in_progress = !write_queue_.empty();
        write_queue_.push_back(msg);
        if (!write_in_progress) do_write();
    });
}

// ─── Partner management ──────────────────────────────────────────────────────
void Session::set_partner(std::shared_ptr<Session> partner) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    partner_ = partner;
}

void Session::clear_partner() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    partner_.reset();
}

// ─── State checks ────────────────────────────────────────────────────────────
bool Session::is_closed() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return closed_;
}

bool Session::is_timed_out() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(
        now - last_activity_).count() > Config::SESSION_TIMEOUT_SECONDS;
}

bool Session::is_heartbeat_timeout() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_heartbeat_).count() > Config::HEARTBEAT_TIMEOUT_MS;
}

void Session::update_activity() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    last_activity_ = std::chrono::steady_clock::now();
}

std::string Session::get_ip()         const { return ip_address_; }
std::string Session::get_session_id() const { return session_id_; }
std::string Session::get_browser_token() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return browser_token_;
}

// ─── Session ID generation ───────────────────────────────────────────────────
std::string Session::generate_session_id() {
    static std::atomic<int> counter{0};
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "session_" + std::to_string(now) + "_" + std::to_string(counter++);
}

// ─── Heartbeat ───────────────────────────────────────────────────────────────
void Session::start_heartbeat() { send_ping(); }

void Session::send_ping() {
    auto self(shared_from_this());
    if (is_closed()) return;
    send(make_json("ping"));
    auto timer = std::make_shared<asio::steady_timer>(
        beast::get_lowest_layer(ws_).get_executor(),
        std::chrono::milliseconds(Config::HEARTBEAT_INTERVAL_MS));
    timer->async_wait([this, self, timer](beast::error_code ec) {
        if (!ec && !is_closed()) send_ping();
    });
}

// ─── Matching ────────────────────────────────────────────────────────────────
void Session::try_match() {
    std::lock_guard<std::mutex> lock(global_mutex);
    {
        std::lock_guard<std::mutex> slock(session_mutex_);
        if (closed_) return;
    }

    waiting_pool.erase(
        std::remove_if(waiting_pool.begin(), waiting_pool.end(),
            [](const std::shared_ptr<Session>& s) { return s->is_closed(); }),
        waiting_pool.end());

    if (!waiting_pool.empty()) {
        auto partner = waiting_pool.front();
        waiting_pool.pop_front();

        if (partner->is_closed()) { try_match(); return; }

        {
            std::lock_guard<std::mutex> slock(session_mutex_);
            partner_ = partner;
        }
        partner->set_partner(shared_from_this());

        send(make_json("status", "Matched"));
        partner->send(make_json("status", "Matched"));

        Metrics::instance().increment_matches();
        Logger::instance().info("Matched: " + session_id_ + " with " + partner->get_session_id());

        // ── Create room in Supabase ──
        std::string t1 = browser_token_;
        std::string t2 = partner->get_browser_token();
        if (!t1.empty() && !t2.empty()) {
            std::string body = "{\"user1_browser_token\":\"" + t1 +
                               "\",\"user2_browser_token\":\"" + t2 +
                               "\",\"room_status\":\"active\"}";
            supabase_post("confabs_rooms", body);
            Logger::instance().info("Room created in Supabase: " + t1 + " <-> " + t2);
        }
    } else {
        waiting_pool.push_back(shared_from_this());
        send(make_json("status", "Waiting"));
        Logger::instance().debug("Waiting: " + session_id_);
    }
}

// ─── Write ───────────────────────────────────────────────────────────────────
void Session::do_write() {
    auto self(shared_from_this());
    if (write_queue_.empty()) return;
    ws_.async_write(
        asio::buffer(write_queue_.front()),
        [this, self](beast::error_code ec, std::size_t) {
            if (!ec) {
                write_queue_.pop_front();
                if (!write_queue_.empty()) do_write();
            } else {
                Logger::instance().error("Write error for " + session_id_ + ": " + ec.message());
                close();
            }
        });
}

// ─── Read ────────────────────────────────────────────────────────────────────
void Session::do_read() {
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
                if (ec != websocket::error::closed)
                    Logger::instance().error("Read error for " + session_id_ + ": " + ec.message());
                close();
            }
        });
}

// ─── Handle JSON ─────────────────────────────────────────────────────────────
void Session::handle_json(const std::string& data) {
    try {
        std::stringstream ss(data);
        ptree tree;
        read_json(ss, tree);
        std::string type = tree.get<std::string>("type");

        // ── pong ──
        if (type == "pong") {
            std::lock_guard<std::mutex> lock(session_mutex_);
            last_heartbeat_ = std::chrono::steady_clock::now();
            return;
        }

        // ── identify (browser token + reconnect check) ──
        if (type == "identify") {
            std::string token = tree.get<std::string>("browser_token", "");
            if (token.empty() || token.length() > 64) {
                send(make_json("error", "Invalid browser token"));
                return;
            }

            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                browser_token_ = token;
            }

            // Register user in Supabase (upsert)
            std::string user_body = "{\"browser_token\":\"" + token + "\","
                                    "\"last_seen\":\"now()\"}";
            supabase_post("confabs_users?on_conflict=browser_token", user_body);

            // Check for active room from last 5 minutes
            std::string query = "confabs_rooms"
                                "?or=(user1_browser_token.eq." + token +
                                ",user2_browser_token.eq." + token + ")"
                                "&room_status=eq.disconnected"
                                "&last_active=gte." +
                                []() {
                                    auto t = std::chrono::system_clock::now() - std::chrono::minutes(5);
                                    auto tt = std::chrono::system_clock::to_time_t(t);
                                    std::ostringstream oss;
                                    oss << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%SZ");
                                    return oss.str();
                                }() +
                                "&limit=1";

            std::string room_result = supabase_get(query);

            if (room_result != "[]" && !room_result.empty()) {
                // Active room found — tell frontend
                send(make_json("info", "Reconnecting to your last session..."));
                Logger::instance().info("Reconnect possible for token: " + token);
                // TODO: Full reconnect logic in next iteration
                // For now fall through to normal matching
            }

            Logger::instance().info("Identified: " + session_id_ + " -> token: " + token.substr(0, 8) + "...");
            try_match();
            return;
        }

        // ── set_name ──
        if (type == "set_name") {
            std::string name = tree.get<std::string>("name", "");
            if (name.empty() || name.length() > 20) {
                send(make_json("error", "Invalid username"));
                return;
            }
            // Sanitize — only alphanumeric and underscore
            name.erase(std::remove_if(name.begin(), name.end(),
                [](char c){ return !std::isalnum(c) && c != '_'; }), name.end());
            if (name.empty()) {
                send(make_json("error", "Invalid username characters"));
                return;
            }
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                username_ = name;
            }
            send(make_json("info", "Username set to " + name));
            Logger::instance().info("Username set: " + session_id_ + " -> " + name);
            return;
        }

        // ── msg ──
        if (type == "msg") {
            std::string text = tree.get<std::string>("text", "");
            if (text.empty()) {
                send(make_json("error", "Empty message"));
                return;
            }
            if (text.length() > Config::MAX_MESSAGE_LENGTH) {
                send(make_json("error", "Message too long (max " +
                    std::to_string(Config::MAX_MESSAGE_LENGTH) + " chars)"));
                return;
            }
            if (!RateLimiter::instance().check_rate_limit(session_id_)){    
                send(make_json("error", "Slow down! You're sending messages too fast."));
                return;
            }

            std::shared_ptr<Session> partner;
            std::string name;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                partner = partner_;
                name    = username_;
            }

            if (partner && !partner->is_closed()) {
                ptree response;
                response.put("type",     "chat");
                response.put("text",     text);
                response.put("username", name);
                std::ostringstream buf;
                write_json(buf, response, false);
                std::string result = buf.str();
                if (!result.empty() && result.back() == '\n') result.pop_back();
                partner->send(result);
                Metrics::instance().increment_messages();
                Logger::instance().debug("Message from " + session_id_ + " to partner");
            } else {
                send(make_json("error", "Not matched"));
            }
            return;
        }

        // ── skip ──
        if (type == "skip") {
            std::shared_ptr<Session> old_partner;
            std::string old_partner_token;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                if (partner_) {
                    old_partner = partner_;
                    old_partner_token = old_partner->get_browser_token();
                    partner_.reset();
                    old_partner->clear_partner();
                }
            }

            if (old_partner && !old_partner->is_closed()) {
                old_partner->send(make_json("info", "Partner skipped"));
                old_partner->try_match();
            }

            // Mark room as closed in Supabase
            if (!browser_token_.empty() && !old_partner_token.empty()) {
                std::string patch_body = "{\"room_status\":\"closed\"}";
                std::string endpoint   = "confabs_rooms"
                                         "?or=(user1_browser_token.eq." + browser_token_ +
                                         ",user2_browser_token.eq." + browser_token_ + ")"
                                         "&room_status=eq.active";
                supabase_patch(endpoint, patch_body);
            }

            Metrics::instance().increment_skips();
            Logger::instance().info("Skip from " + session_id_);
            try_match();
            return;
        }

        // ── typing ──
        if (type == "typing") {
            bool is_typing = tree.get<bool>("status", false);
            std::shared_ptr<Session> partner;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                partner = partner_;
            }
            if (partner && !partner->is_closed()) {
                ptree response;
                response.put("type",   "typing");
                response.put("status", is_typing);
                std::ostringstream buf;
                write_json(buf, response, false);
                std::string result = buf.str();
                if (!result.empty() && result.back() == '\n') result.pop_back();
                partner->send(result);
            }
            return;
        }

        send(make_json("error", "Unknown message type"));
        Logger::instance().warn("Unknown type from " + session_id_ + ": " + type);

    } catch (const std::exception& e) {
        send(make_json("error", "Invalid JSON"));
        Logger::instance().error("JSON parse error from " + session_id_ + ": " + e.what());
    }
}

// ─── Close ───────────────────────────────────────────────────────────────────
void Session::close() {
    std::shared_ptr<Session> partner_copy;
    std::string my_token;

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (closed_) return;
        closed_   = true;
        my_token  = browser_token_;
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

    // Mark room as disconnected in Supabase (not closed — allows reconnect)
    if (!my_token.empty()) {
        std::string patch_body = "{\"room_status\":\"disconnected\","
                                  "\"last_active\":\"now()\"}";
        std::string endpoint   = "confabs_rooms"
                                  "?or=(user1_browser_token.eq." + my_token +
                                  ",user2_browser_token.eq." + my_token + ")"
                                  "&room_status=eq.active";
        supabase_patch(endpoint, patch_body);
        Logger::instance().info("Room marked disconnected for token: " + my_token.substr(0, 8) + "...");
    }

    beast::error_code ec;
    ws_.close(websocket::close_code::normal, ec);
    ws_.next_layer().shutdown(ec);
    beast::get_lowest_layer(ws_).close();

    IPTracker().disconnect(ip_address_);
    Metrics::instance().decrement_connections();
    Logger::instance().info("Connection closed: " + session_id_);
}

// ─── Cleanup thread ──────────────────────────────────────────────────────────
void cleanup_thread(asio::io_context& io) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(Config::CLEANUP_INTERVAL_SECONDS));

        std::vector<std::shared_ptr<Session>> to_remove;
        {
            std::lock_guard<std::mutex> lock(global_mutex);
            for (const auto& session : sessions) {
                if (session->is_timed_out() || session->is_heartbeat_timeout())
                    to_remove.push_back(session);
            }
            for (const auto& session : to_remove) {
                sessions.erase(session);
                Logger::instance().info("Removed timed out session: " + session->get_session_id());
            }
        }

        RateLimiter::instance().cleanup_old_entries();
        Metrics::instance().print_stats();
    }
}