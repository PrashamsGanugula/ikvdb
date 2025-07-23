#include <iostream>
#include <cstdlib>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <thread>
#include <algorithm> 
#include <sstream>
#include "handle_redis_commands.h"
#include "redis_parser.h"
#include "database.h"

using namespace std;
using std::thread;

void handle_client(int client_fd,const vector<pair<string, string>> &replica_info) {
  char buffer[1024];

  while(1){

    int bytes_read = read(client_fd, buffer, sizeof(buffer));
    if(bytes_read<=0){
      cerr<< "failed to read\n";
      close(client_fd);
      return;
    }
    
    string input(buffer);
    RESPParser parser;
    try{
      RESPObject obj = parser.parse(input);
      string response = handle_command(obj, client_fd, replica_info);
      write(client_fd, response.c_str(),response.size()); 
    }
    catch(const exception& ex){
      cout << "Parse error: "<< ex.what() << "\n";
    }

  }

  close(client_fd);
}


vector<pair<string, string>> parse_info(int port, const string& replica_host, int replica_port) {
  vector<pair<string, string>> result;
  string role;
  
  if(replica_port == 0) {
      role = "master";
  } 
  else {
      role = "slave";
  }
  string master_replid = "8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb";
  string master_repl_offset = "0";
  result.push_back(make_pair("role", role));
  result.push_back(make_pair("master_replid", master_replid));
  result.push_back(make_pair("master_repl_offset", master_repl_offset));
  
  return result;
}


void connect_to_master(const string& replica_host, int master_port, int replica_port, const vector<pair<string, string>>& replica_info) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
      cerr << "Failed to create socket for master connection\n";
      return;
  }

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(master_port);

  if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
      cerr << "Failed to connect to master: " << strerror(errno) << "\n";
      close(sockfd);
      return;
  }


  auto send_and_recv = [&](const string& message, const string& tag) -> string {
      if (send(sockfd, message.c_str(), message.size(), 0) < 0) {
          cerr << "Failed to send " << tag << "\n";
          return "";
      }

      char buffer[1028];
      int len = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
      if (len <= 0) {
          cerr << "Failed to receive " << tag << " response\n";
          return "";
      }
      buffer[len] = '\0';
      return string(buffer);
  };

  string response;

  // 1. Send PING
  response = send_and_recv("*1\r\n$4\r\nPING\r\n", "PING");
  

  // 2. Send REPLCONF listening-port
  string replconf_port = "*3\r\n$8\r\nREPLCONF\r\n$14\r\nlistening-port\r\n$" + to_string(to_string(replica_port).size()) + "\r\n" + to_string(replica_port) + "\r\n";
  response = send_and_recv(replconf_port, "REPLCONF listening-port");
  cout << "[Master Reply - REPLCONF port]: " << response;

  // 3. Send REPLCONF capa psync2
  string replconf_capa = "*3\r\n$8\r\nREPLCONF\r\n$4\r\ncapa\r\n$6\r\npsync2\r\n";
  response = send_and_recv(replconf_capa, "REPLCONF capa");
  cout << "[Master Reply - REPLCONF capa]: " << response;

  // 4. Send PSYNC ? -1
  string psync = "*3\r\n$5\r\nPSYNC\r\n$1\r\n?\r\n$2\r\n-1\r\n";
  response = send_and_recv(psync, "PSYNC");
  cout << "[Master Reply - PSYNC]: " << response;

  char buffer[1024];
  while(1){

    int bytes_received = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if(bytes_received<=0){
      cerr<< "failed to read\n";
      close(sockfd);
      return;
    }
    
    string input(buffer);
    RESPParser parser;
    try{
      RESPObject obj = parser.parse(input);
      string response = handle_command(obj, -1, vector<pair<string, string>>());
    }
    catch(const exception& ex){
      cout << "Parse error: "<< ex.what() << "\n";
    }

  }
    
  close(sockfd);
}

int main(int argc, char **argv){
  // Flush after every std::cout / std::cerr
  cout << unitbuf;
  cerr << unitbuf;

  int port = 6379;
  string replica_host="";
  int replica_port = 0;
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[i + 1]);
            i += 1;
        } else if (strcmp(argv[i], "--replicaof") == 0 && i + 1 < argc) {
            string host_port = argv[i + 1];
            size_t space_pos = host_port.find(' ');
            if (space_pos != string::npos) {
                replica_host = host_port.substr(0, space_pos);
                replica_port = stoi(host_port.substr(space_pos + 1));
            } else {
                cerr << "Error: --replicaof requires a quoted host and port, e.g., \"127.0.0.1 6379\"\n";
                exit(1);
            }
            i += 1;
        }
    }

    if (port <= 0 || port > 65535) {
        cerr << "Invalid port number. Using default port 6379.\n";
        port = 6379;
    }
  }

  vector<pair<string, string>> replica_info = parse_info(port, replica_host, replica_port);

  if(!replica_host.empty() && replica_port != 0){
    thread replica_thread(connect_to_master, replica_host, replica_port, port, ref(replica_info));
    replica_thread.detach();
  }
  
  // cout<<port<<endl;
  // cout<<replica_host<<endl;
  // cout<<replica_port<<endl;

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
   cerr << "Failed to create server socket\n";
   return 1;
  }
  
  // Since the tester restarts your program quite often, setting SO_REUSEADDR
  // ensures that we don't run into 'Address already in use' errors
  int reuse = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    cerr << "setsockopt failed\n";
    return 1;
  }
  
  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port);
  
  if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0) {
    cerr << "Failed to bind to port 6379\n";
    return 1;
  }
  
  int connection_backlog = 5;
  if (listen(server_fd, connection_backlog) != 0) {
    cerr << "listen failed\n";
    return 1;
  }
  
  struct sockaddr_in client_addr;
  int client_addr_len = sizeof(client_addr);


  while(1){
    cout << "Waiting for a client to connect...\n";

    // You can use print statements as follows for debugging, they'll be visible when running tests.
    cout << "Logs from your program will appear here!\n";

    int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, (socklen_t *) &client_addr_len);
    cout << "Client connected\n";
    thread client_thread(handle_client, client_fd, ref(replica_info));
    client_thread.detach();
  }
  

  return 0;
}