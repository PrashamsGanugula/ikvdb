#pragma once
#include "redis_parser.h"


using namespace std;

string handle_echo(const vector<RESPObject>& args);
string handle_multi(const vector<RESPObject>& args, int client_fd);
string handle_exec(const vector<RESPObject>& args, int client_fd, const vector<pair<string, string>>& replica_info);
string handle_discard(const vector<RESPObject>& args, int client_fd);
string handle_set(const vector<RESPObject>& args, int client_fd);
string handle_incr(const vector<RESPObject>& args, int client_fd);
string handle_get(const vector<RESPObject>& args, int client_fd);
string handle_rpush(const vector<RESPObject>& args);
string handle_lpush(const vector<RESPObject>& args);
string handle_lrange(const vector<RESPObject>& args);
string handle_llen(const vector<RESPObject>& args);
string handle_lpop(const vector<RESPObject>& args);
string handle_blpop(const vector<RESPObject>& args);
string handle_type(const vector<RESPObject>& args);
string handle_xadd(const vector<RESPObject>& args);
string handle_xrange(const vector<RESPObject>& args);
string handle_xread(const vector<RESPObject>& args);
string handle_info(const vector<RESPObject>& args, const vector<pair<string, string>>& replica_info);
string hex_to_escaped_string(const string& hex);
string handle_command(const RESPObject& obj, int client_fd, const vector<pair<string, string>>& replica_info);