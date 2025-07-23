#pragma once
#include <unordered_map>
#include <string>
#include <mutex>
#include <chrono>
#include <deque>
#include <condition_variable>
#include "redis_parser.h"

using namespace std;
using namespace chrono;

struct StreamEntry {
    string id;
    unordered_map<string, string> fields;
};

extern condition_variable list_cv;
extern bool list_updated;
extern condition_variable stream_cv;
extern bool stream_updated;

extern unordered_map<string, string> global_store;
extern unordered_map<string, steady_clock::time_point> global_key_expirations;
extern unordered_map<string, deque<string>> global_list_store;
extern unordered_map<string, deque<StreamEntry>> global_stream_store;
extern unordered_map<int, bool> global_transaction_flags;
extern unordered_map<int, vector<vector<RESPObject>>> global_transaction_queue;

extern mutex store_mutex;
extern mutex list_store_mutex;
extern mutex stream_store_mutex;


