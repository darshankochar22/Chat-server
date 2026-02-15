#include <iostream>
#include <memory>
#include <deque>
#include <unordered_set>
#include <mutex>
#include <sstream>
#include <algorithm>

#include <boost/asio.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

namespace asio = boost::asio;
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
    return buf.str();
}

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket)
        : socket_(std::move(socket)) {}

    void start() {
        {
            std::lock_guard<std::mutex> lock(global_mutex);
            sessions.insert(shared_from_this());
        }

        send(make_json("info", "Welcome"));
        try_match();
        do_read();
    }

    void send(const std::string& msg) {
        auto self(shared_from_this());
        std::string data = msg + "\n";

        asio::async_write(socket_, asio::buffer(data),
            [this, self](boost::system::error_code ec, std::size_t) {
                if (ec) close();
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
    tcp::socket socket_;
    std::shared_ptr<Session> partner_;
    asio::streambuf buffer_;
    mutable std::mutex session_mutex_;
    bool closed_ = false;

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
                // Try again with cleaned pool
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

    void do_read() {
        auto self(shared_from_this());
        asio::async_read_until(socket_, buffer_, '\n',
            [this, self](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    std::istream is(&buffer_);
                    std::string line;
                    std::getline(is, line);

                    handle_json(line);
                    do_read();
                } else {
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
        bool was_closed = false;

        {
            std::lock_guard<std::mutex> glock(global_mutex);
            std::lock_guard<std::mutex> slock(session_mutex_);

            if (closed_) return;
            closed_ = true;
            was_closed = true;

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

        // Close socket outside of locks
        boost::system::error_code ec;
        socket_.close(ec);
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
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket))->start();
                }
                do_accept();
            });
    }
};

int main() {
    try {
        asio::io_context io;
        Server server(io, 8080);

        std::cout << "JSON matchmaking server running on port 8080\n";
        std::cout << "Protocol:\n";
        std::cout << "  - Send: {\"type\":\"msg\",\"text\":\"your message\"}\n";
        std::cout << "  - Send: {\"type\":\"skip\"} to find new partner\n";
        std::cout << "  - Receive: {\"type\":\"chat\",\"message\":\"partner's message\"}\n";
        std::cout << "  - Receive: {\"type\":\"status\",\"message\":\"Matched|Waiting\"}\n\n";

        io.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}