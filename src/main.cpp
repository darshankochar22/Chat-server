#include "chat_server.h"
#include<boost/asio/ssl.hpp>

Server::Server(asio::io_context& io, short port,asio::ssl::context& ctx)
    : acceptor_(io, tcp::endpoint(tcp::v4(), port))
    , io_context_(io) 
    ,ssl_ctx_(ctx){
    
    cleanup_thread_ = std::thread([&io]() {
        cleanup_thread(io);
    });
    
    do_accept();
}

Server::~Server() {
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.detach();
    }
}

void Server::do_accept() {
    acceptor_.async_accept(
        [this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::string ip = socket.remote_endpoint().address().to_string();
                
                if (!IPTracker().can_connect(ip)) {
                    Logger::instance().warn("Rejected connection from " + ip + " (too many connections)");
                    socket.close();
                } else {
             
                    auto session = std::make_shared<Session>(std::move(socket), ssl_ctx_, ip);

                    
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

int main() {
    try {
        load_env();
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
        asio::ssl::context ctx{asio::ssl::context::tlsv12};
        ctx.use_certificate_chain_file("server.crt");
        ctx.use_private_key_file("server.key", asio::ssl::context::pem);
        Server server(io, 8080,ctx);
        
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