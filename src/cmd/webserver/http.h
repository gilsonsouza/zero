#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "command.h"
#include "kits_cmd.h"
#include <boost/asio.hpp>
#include <string>
#include <memory>

using namespace boost;
using namespace boost::system;
using namespace boost::asio;

class http_headers
{
   std::string method;
   std::string url;
   std::string version;
   KitsCommand kits;
  
   std::map<std::string, std::string> headers;

public:
   
   http_headers(); 
   std::string get_response();
   
   int content_length();
   void on_read_header(std::string line);
   void on_read_request_line(std::string line);
};


class session {
   asio::streambuf buff;
   http_headers headers;
   
   static void read_body(std::shared_ptr<session> pThis);

   static void read_next_line(std::shared_ptr<session> pThis);

   static void read_first_line(std::shared_ptr<session> pThis);
public:

   ip::tcp::socket socket;
   
   session(io_service& io_service)
      :socket(io_service)
   {
   }
   
   static void interact(std::shared_ptr<session> pThis);

};


class Webserver : public Command
{
public:
    void setupOptions();
    void run();

};

#endif

