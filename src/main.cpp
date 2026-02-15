#include<iostream>
#include<memory>
#include<boost/asio.hpp>
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

class Session: public std::enable_shared_from_this<Session> {
public:
   explicit Session(tcp::socket socket)
       : socket_(std::move(socket)) {}
   
   void start() {
        do_write();
    }   
private:
   tcp::socket socket_;
   std::string message_ = "Hello from async C++ server\n";

   void do_write(){
       auto self(shared_from_this());
       boost::asio::async_write(
          socket_,
          boost::asio::buffer(message_),
          [this, self] (boost::system::error_code ec, std::size_t){
            if(!ec) socket_.close();
        });
   }
};


class Server{
public:
  Server(asio::io_context& io, short port)    
    : acceptor_(io, tcp::endpoint(tcp::v4(), port)){
        do_accept();
    }

private:
  void do_accept(){
      acceptor_.async_accept(
         [this](boost::system::error_code ec, tcp::socket socket){
            if(!ec){
                std::make_shared<Session>(std::move(socket))->start();
            }
            do_accept();
         }
      );
  }
  tcp::acceptor acceptor_;
};


int main(){
    try{
        asio::io_context io;
        Server server(io, 8080);
        std::cout<<"Async server running on port 8080\n";
        io.run();
    } catch(std::exception& e){
        std::cerr<<e.what()<<std::endl;
    }
}
