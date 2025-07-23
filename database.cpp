#include <unordered_map>
#include <string>
#include <mutex>
#include <chrono>
#include <deque>
#include <condition_variable>
#include "database.h"
#include "redis_parser.h"

using namespace std;
using namespace chrono;

condition_variable list_cv;
bool list_updated = false;
condition_variable stream_cv;
bool stream_updated = false;

unordered_map<string, string> global_store;
unordered_map<string, steady_clock::time_point> global_key_expirations;
unordered_map<string, deque<string>> global_list_store;
unordered_map<string, deque<StreamEntry>> global_stream_store;
unordered_map<int, bool> global_transaction_flags;
unordered_map<int, vector<vector<RESPObject>>> global_transaction_queue;

mutex store_mutex;
mutex list_store_mutex;
mutex stream_store_mutex;
