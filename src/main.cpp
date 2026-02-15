#include <iostream>
#include <memory>
#include <deque>
#include <unordered_set>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <string>

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

class Session;

std::deque<std::shared_ptr<Session>> waiting_pool;
std::unordered_set<std::shared_ptr<Session>> sessions;
std::mutex global_mutex;

std::string make_json(const std::string& type, const std::string& message="") {
    ptree tree;
    tree.put("type", type);
    if (!message.empty()) tree.put("message", message);

    std::ostringstream buf;
    write_json(buf, tree, false);
    std::string result = buf.str();
    
    // Remove trailing newline from JSON
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    
    return result;
}

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket)
        : ws_(std::move(socket)) {}

    void start() {
        // Set WebSocket options
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) {
                res.set(beast::http::field::server, "AnonymousChat/1.0");
            }));

        // Accept the WebSocket handshake
        ws_.async_accept(
            [self = shared_from_this()](beast::error_code ec) {
                if (!ec) {
                    {
                        std::lock_guard<std::mutex> lock(global_mutex);
                        sessions.insert(self);
                    }
                    
                    self->send(make_json("info", "Welcome to Anonymous Chat"));
                    self->try_match();
                    self->do_read();
                } else {
                    std::cerr << "WebSocket accept error: " << ec.message() << std::endl;
                }
            });
    }

    void send(const std::string& msg) {
        auto self(shared_from_this());
        
        // Queue message to be sent
        asio::post(ws_.get_executor(),
            [this, self, msg]() {
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

private:
    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    std::shared_ptr<Session> partner_;
    mutable std::mutex session_mutex_;
    bool closed_ = false;
    std::deque<std::string> write_queue_;

    void try_match() {
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

            // Double-check partner is still valid
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
        } else {
            waiting_pool.push_back(shared_from_this());
            send(make_json("status", "Waiting"));
        }
    }

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
                    std::cerr << "Write error: " << ec.message() << std::endl;
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
                    
                    handle_json(message);
                    do_read();
                } else {
                    if (ec != websocket::error::closed) {
                        std::cerr << "Read error: " << ec.message() << std::endl;
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

            if (type == "msg") {
                std::string text = tree.get<std::string>("text", "");

                std::shared_ptr<Session> partner;
                {
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    partner = partner_;
                }

                if (partner && !partner->is_closed()) {
                    partner->send(make_json("chat", text));
                } else {
                    send(make_json("error", "Not matched"));
                }
            }
            else if (type == "skip") {
                std::shared_ptr<Session> old_partner;

                {
                    std::lock_guard<std::mutex> glock(global_mutex);
                    std::lock_guard<std::mutex> slock(session_mutex_);
                    
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
                try_match();
            }
            else {
                send(make_json("error", "Unknown type"));
            }

        } catch (const std::exception& e) {
            send(make_json("error", "Invalid JSON"));
        }
    }

    void close() {
        std::shared_ptr<Session> partner_copy;

        {
            std::lock_guard<std::mutex> glock(global_mutex);
            std::lock_guard<std::mutex> slock(session_mutex_);

            if (closed_) return;
            closed_ = true;

            if (partner_) {
                partner_copy = partner_;
                partner_.reset();
            }

            // Remove from waiting pool if present
            waiting_pool.erase(
                std::remove(waiting_pool.begin(), waiting_pool.end(), shared_from_this()),
                waiting_pool.end()
            );

            sessions.erase(shared_from_this());
        }

        // Clear partner's reference to this session
        if (partner_copy) {
            partner_copy->clear_partner();
            
            if (!partner_copy->is_closed()) {
                partner_copy->send(make_json("info", "Partner disconnected"));
                partner_copy->try_match();
            }
        }

        // Close WebSocket
        beast::error_code ec;
        ws_.close(websocket::close_code::normal, ec);
    }
};

class Server {
public:
    Server(asio::io_context& io, short port)
        : acceptor_(io, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }

private:
    tcp::acceptor acceptor_;

    void do_accept() {
        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket))->start();
                } else {
                    std::cerr << "Accept error: " << ec.message() << std::endl;
                }
                do_accept();
            });
    }
};

int main() {
    try {
        asio::io_context io;
        Server server(io, 8080);

        std::cout << "===========================================\n";
        std::cout << "WebSocket Chat Server Running on port 8080\n";
        std::cout << "===========================================\n\n";
        std::cout << "Connect from browser using:\n";
        std::cout << "  ws://localhost:8080\n\n";
        std::cout << "Protocol:\n";
        std::cout << "  Send: {\"type\":\"msg\",\"text\":\"your message\"}\n";
        std::cout << "  Send: {\"type\":\"skip\"}\n";
        std::cout << "  Receive: {\"type\":\"chat\",\"message\":\"...\"}\n";
        std::cout << "  Receive: {\"type\":\"status\",\"message\":\"Matched|Waiting\"}\n";
        std::cout << "  Receive: {\"type\":\"info\",\"message\":\"...\"}\n\n";

        io.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}