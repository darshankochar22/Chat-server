#include <iostream>
#include <memory>
#include <deque>
#include <unordered_set>
#include <mutex>
#include <sstream>

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
        partner_ = partner;
    }

    void clear_partner() {
        partner_.reset();
    }

private:
    tcp::socket socket_;
    std::shared_ptr<Session> partner_;
    asio::streambuf buffer_;
    bool closed_ = false;

    void try_match() {
        std::lock_guard<std::mutex> lock(global_mutex);

        if (closed_) return;

        if (!waiting_pool.empty()) {
            auto partner = waiting_pool.front();
            waiting_pool.pop_front();

            partner_ = partner;
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

                auto partner = partner_;
                if (partner) {
                    partner->send(make_json("chat", text));
                } else {
                    send(make_json("error", "Not matched"));
                }
            }
            else if (type == "skip") {
                std::shared_ptr<Session> old_partner;

                {
                    std::lock_guard<std::mutex> lock(global_mutex);
                    if (partner_) {
                        old_partner = partner_;
                        clear_partner();
                        old_partner->clear_partner();
                    }
                }

                if (old_partner) old_partner->try_match();
                try_match();
            }
            else {
                send(make_json("error", "Unknown type"));
            }

        } catch (...) {
            send(make_json("error", "Invalid JSON"));
        }
    }

    void close() {
        std::shared_ptr<Session> partner_copy;

        {
            std::lock_guard<std::mutex> lock(global_mutex);

            if (closed_) return;
            closed_ = true;

            if (partner_) {
                partner_copy = partner_;
                partner_->clear_partner();
                clear_partner();
            }

            sessions.erase(shared_from_this());
        }

        if (partner_copy) {
            partner_copy->send(make_json("info", "Partner disconnected"));
            partner_copy->try_match();
        }

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

        io.run();
    }
    catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
}
