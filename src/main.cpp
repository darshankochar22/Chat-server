#include <iostream>
#include <boost/asio.hpp>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

int main() {
    try {
        asio::io_context io;

        tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 8080));

        std::cout <<"Server running on port 8080\n";

        while(true){
            tcp::socket client_socket(io);
            acceptor.accept(client_socket);

            std::string message = "Hello from C++ backend\n";
            asio::write(client_socket, asio::buffer(message));
        }

    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
}
