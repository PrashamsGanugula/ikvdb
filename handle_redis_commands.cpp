#include <unistd.h>
#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <unordered_map>
#include <thread>
#include <condition_variable>
#include <deque>
#include <sstream>
#include "handle_redis_commands.h"
#include "redis_parser.h"
#include "database.h"


using namespace std;

string handle_echo(const vector<RESPObject> & args){
    if(args.size() !=2){
        return "-ERR ECHO expects 1 argument\r\n";
    }
    const RESPObject& message_obj = args[1];
    if (message_obj.get_type() != RESPType::BulkString && message_obj.get_type() != RESPType::SimpleString) {
        return "-ERR invalid argument type\r\n";
    }
    string value = message_obj.get_string_value();
    string response = "$" + to_string(value.size()) + "\r\n" + value + "\r\n";
    return response;
}

string handle_multi(const vector<RESPObject>& args, int client_fd) {
    if (args.size() != 1) {
        return "-ERR wrong number of arguments for 'multi'\r\n";
    }
    
    lock_guard<mutex> lock(store_mutex);
    if (global_transaction_flags.find(client_fd) != global_transaction_flags.end()) {
        return "-ERR MULTI is already in progress for this client\r\n";
    }

    global_transaction_flags[client_fd] = true;
    global_transaction_queue[client_fd] = vector<vector<RESPObject>>();
    stream_updated = false; 
    stream_cv.notify_all(); 
    
    return "+OK\r\n";
}

string handle_exec(const vector<RESPObject>& args, int client_fd, const vector<pair<string, string>>& replica_info) {
    if(args.size() != 1){
        return "-ERR wrong number of arguments for 'exec'\r\n";
    }

    vector<vector<RESPObject>> commands;

    {
        lock_guard<mutex> lock(store_mutex);

        auto flag_it = global_transaction_flags.find(client_fd);
        if(flag_it == global_transaction_flags.end()){
            return "-ERR EXEC without MULTI\r\n";
        }

        global_transaction_flags.erase(flag_it);

        auto queue_it = global_transaction_queue.find(client_fd);
        if(queue_it == global_transaction_queue.end()){
            return "*0\r\n"; 
        }

        commands = queue_it->second;
        global_transaction_queue.erase(queue_it);
    }

    string response = "*" + to_string(commands.size()) + "\r\n";
    for (const auto& command : commands) {  
            RESPObject cmd_obj(RESPType::Array, "", 0, command);
            response += handle_command(cmd_obj, client_fd, replica_info); 
    }
    return response;
}

string handle_discard(const vector<RESPObject>& args, int client_fd) {
    if (args.size() != 1) {
        return "-ERR wrong number of arguments for 'discard'\r\n";
    }

    lock_guard<mutex> lock(store_mutex);
    
    auto flag_it = global_transaction_flags.find(client_fd);
    if (flag_it == global_transaction_flags.end()) {
        return "-ERR DISCARD without MULTI\r\n";
    }

    global_transaction_flags.erase(flag_it);
    global_transaction_queue.erase(client_fd);

    return "+OK\r\n";
}

string handle_set(const vector<RESPObject> & args, int client_fd){
    if(args.size() != 3 && args.size() !=5){
        return "-ERR wrong number of arguments for 'set'\r\n";
    }

    {
        lock_guard<mutex> lock(store_mutex);
        if (global_transaction_flags.find(client_fd) != global_transaction_flags.end()) {
            global_transaction_queue[client_fd].push_back(args);
            return "+QUEUED\r\n";
        }
    }

    string key= args[1].get_string_value();
    string value = args[2].get_string_value();

    steady_clock::time_point expiry_time;

    bool has_expiry = false;

    if(args.size() == 5){

        string option = args[3].get_string_value();
        for(int i=0; i<option.size();i++){
            if(option[i] >= 'a' && option[i] <= 'z') {
                option[i] = option[i] - ('a'-'A');
            }
        }

        if(option != "PX" && option != "px"){
            return "-ERR syntax error\r\n";
        } 

        try{
            int64_t px = stoll(args[4].get_string_value());
            expiry_time = steady_clock::now() + milliseconds(px);
            has_expiry = true;
        }
        catch(...){
            return "-ERR PX value is not a valid integer\r\n";
        }
    }

    {
        lock_guard<mutex> lock(store_mutex);
        global_store[key] = value;
        if(has_expiry){
            global_key_expirations[key] = expiry_time;
        }
        else{
            global_key_expirations.erase(key);
        }
    }

    return "+OK\r\n";
}

string handle_incr(const vector<RESPObject> & args, int client_fd){
    if(args.size() != 2){
        return "-ERR wrong number of arguments for 'incr'\r\n";
    }

    {
        lock_guard<mutex> lock(store_mutex);
        if (global_transaction_flags.find(client_fd) != global_transaction_flags.end()) {
            global_transaction_queue[client_fd].push_back(args);
            return "+QUEUED\r\n";
        }
    }

    string key = args[1].get_string_value();

    lock_guard<mutex> lock(store_mutex);

    auto it = global_store.find(key);
    if(it == global_store.end()){
        global_store[key] = "1"; // Initialize to 1 if key does not exist
        return ":1\r\n";  
    }

    int64_t value;
    try {
        value = stoll(it->second);
    } catch (...) {
        return "-ERR value is not an integer or out of range\r\n";
    }

    value++;
    it->second = to_string(value);

    return ":" + to_string(value) + "\r\n";
}

string handle_get(const vector<RESPObject> & args, int client_fd){
    if(args.size() !=2){
        return "-ERR wrong number of arguments for 'get'\r\n";
    }

    {
        lock_guard<mutex> lock(store_mutex);
        if (global_transaction_flags.find(client_fd) != global_transaction_flags.end()) {
            global_transaction_queue[client_fd].push_back(args);
            return "+QUEUED\r\n";
        }
    }

    string key = args[1].get_string_value();

    lock_guard<mutex> lock(store_mutex);

    auto exp_it = global_key_expirations.find(key);
    if(exp_it != global_key_expirations.end()){
        if(steady_clock::now() >= exp_it-> second){
            global_store.erase(key);
            global_key_expirations.erase(key);
        }
    }


    auto it = global_store.find(key);
    if(it == global_store.end()){
        return "$-1\r\n";
    }

    string value = it->second;
    string value_length = to_string(value.length());
    string response = "$" + value_length + "\r\n" + value + "\r\n";
    return response;
    

}

string handle_rpush(const vector<RESPObject> & args){
    if(args.size() < 3){
        return "-ERR wrong number of arguments for 'rpush'\r\n";
    }

    string key = args[1].get_string_value();
    vector<string> values;

    for(size_t i = 2; i < args.size(); ++i) {
        if(args[i].get_type() != RESPType::BulkString && args[i].get_type() != RESPType::SimpleString) {
            return "-ERR invalid argument type\r\n";
        }
        values.push_back(args[i].get_string_value());
    }

    unique_lock<mutex> lock(list_store_mutex);
    for(auto &vals:values) {
        global_list_store[key].push_back(vals);
    }

    list_updated = true;
    list_cv.notify_all();

    string response = ":" + to_string(global_list_store[key].size()) + "\r\n";
    return response;
}

string handle_lpush(const vector<RESPObject> & args){
    if(args.size() < 3){
        return "-ERR wrong number of arguments for 'lpush'\r\n";
    }

    string key = args[1].get_string_value();
    vector<string> values;

    for(size_t i = 2; i < args.size(); ++i) {
        if(args[i].get_type() != RESPType::BulkString && args[i].get_type() != RESPType::SimpleString) {
            return "-ERR invalid argument type\r\n";
        }
        values.push_back(args[i].get_string_value());
    }

    unique_lock<mutex> lock(list_store_mutex);

    for(auto &vals:values) {
        global_list_store[key].push_front(vals);
    }

    list_updated = true;
    list_cv.notify_all();

    string response = ":" + to_string(global_list_store[key].size()) + "\r\n";
    return response;
}

string handle_lrange(const vector<RESPObject> & args){
    if(args.size() != 4){
        return "-ERR wrong number of arguments for 'lrange'\r\n";
    }

    string key = args[1].get_string_value();
    int64_t start, end;

    try {
        start = stoll(args[2].get_string_value());
        end = stoll(args[3].get_string_value());
    } catch (...) {
        return "-ERR invalid range values\r\n";
    }

    lock_guard<mutex> lock(list_store_mutex);
    
    auto it = global_list_store.find(key);
    if(it == global_list_store.end() || it->second.empty()){
        return "*0\r\n";
    }

    const deque<string>& list = it->second;
    
    if(start < 0) start += list.size();
    if(end < 0) end += list.size();
    if(start < 0) start = 0;
    if(end >= list.size()) end = list.size() - 1;
    if(start > end || start >= list.size()) {
        return "*0\r\n";
    }
    

    string response = "*" + to_string(end - start + 1) + "\r\n";
    
    for(int i = start; i <= end; ++i) {
        response += "$" + to_string(list[i].size()) + "\r\n" + list[i] + "\r\n";
    }
    
    return response;
}

string handle_lpop(const vector<RESPObject> & args){
    if(args.size() != 2 && args.size() != 3){
        return "-ERR wrong number of arguments for 'lpop'\r\n";
    }

    string key = args[1].get_string_value();

    lock_guard<mutex> lock(list_store_mutex);
    
    auto it = global_list_store.find(key);
    if(it == global_list_store.end() || it->second.empty()){
        return "$-1\r\n";
    }

    int64_t num_items_to_remove;
    if(args.size() == 3) {
        try{
            num_items_to_remove = stoll(args[2].get_string_value());
            if(num_items_to_remove < 0) {
                return "-ERR count must be a non-negative integer\r\n";
            }
        } 
        catch(...){
            return "-ERR invalid count value\r\n";
        }
    }
    else{
        num_items_to_remove = 1; 
    }

    vector<string> values;
    for(int64_t i = 0; i < num_items_to_remove && !it->second.empty(); ++i) {
        values.push_back(it->second.front());
        it->second.pop_front();
    }

    if(it->second.empty()){
        global_list_store.erase(it);
    }

    if(num_items_to_remove == 1) {
        if(values.empty()) {
            return "$-1\r\n";
        }
        string response = "$" + to_string(values[0].size()) + "\r\n" + values[0] + "\r\n";
        return response;
    }
    string response;
    response += "*" + to_string(values.size()) + "\r\n";
    for(auto &val : values) {
        response += "$" + to_string(val.size()) + "\r\n" + val + "\r\n";
    }
    return response;
}

string handle_llen(const vector<RESPObject> & args){
    if(args.size() != 2){
        return "-ERR wrong number of arguments for 'llen'\r\n";
    }

    string key = args[1].get_string_value();

    lock_guard<mutex> lock(list_store_mutex);
    
    auto it = global_list_store.find(key);
    if(it == global_list_store.end()){
        return ":0\r\n";
    }

    int length = it->second.size();
    return ":" + to_string(length) + "\r\n";
}

string handle_blpop(const vector<RESPObject> & args){     
    if(args.size() != 3){         
        return "-ERR wrong number of arguments for 'blpop'\r\n";     
    }          
    
    string key = args[1].get_string_value();     
    double timeout;     
    try {         
        timeout = stod(args[2].get_string_value());     
    } catch (...) {         
        return "-ERR invalid timeout value\r\n";     
    }          
    
    if(timeout < 0) {         
        return "-ERR timeout must be a non-negative number\r\n";     
    }      
    
    steady_clock::time_point end_time;     
    if(timeout == 0) {         
        end_time = steady_clock::time_point::max();      
    } else {         
        end_time = steady_clock::now() + milliseconds(static_cast<int64_t>(timeout * 1000));     
    }      
    
    unique_lock<mutex> lock(list_store_mutex);          
    
    while(true) {         
        auto it = global_list_store.find(key);         
        if(it != global_list_store.end() && !it->second.empty()) {             
            string value = it->second.front();             
            it->second.pop_front();             
            if(it->second.empty()) {                 
                global_list_store.erase(it);             
            }                          
            
            string response = "*2\r\n";             
            response += "$" + to_string(key.size()) + "\r\n" + key + "\r\n";             
            response += "$" + to_string(value.size()) + "\r\n" + value + "\r\n";             
            return response;         
        }                  
        
        if (steady_clock::now() >= end_time) {             
            return "*-1\r\n"; // Timeout - null array        
        }          
        
        if (timeout == 0) {             
            list_cv.wait(lock, [&] { return list_updated; });         
        } else {             
            auto now = steady_clock::now();             
            if (now >= end_time) {
                return "*-1\r\n"; // Timeout - null array
            }
            auto time_left = end_time - now;             
            bool signaled = list_cv.wait_for(lock, time_left, [&] { return list_updated; });             
            if (!signaled) {                 
                return "*-1\r\n"; // Timeout - null array            
            }         
        }                  
        
        list_updated = false;     
    } 
}

string handle_type(const vector<RESPObject>& args){
    if(args.size() != 2){
        return "-ERR wrong number of arguments for 'type'\r\n";
    }

    string key = args[1].get_string_value();

    lock_guard<mutex> lock(store_mutex);
    
    if(global_store.find(key) != global_store.end()){
        return "+string\r\n";
    }

    if(global_list_store.find(key) != global_list_store.end()){
        return "+list\r\n";
    }

    if(global_stream_store.find(key) != global_stream_store.end()){
        return "+stream\r\n";
    }

    return "+none\r\n"; 
}

string handle_xadd(const vector<RESPObject>& args) {
    if (args.size()<4 || (args.size()-3)%2!=0) {
        return "-ERR wrong number of arguments for 'xadd'\r\n";
    }

    string key = args[1].get_string_value();
    string id = args[2].get_string_value();

    string last_timestamp_str="0";
    string last_sequence_str="0";

    {
        lock_guard<mutex> lock(stream_store_mutex);
        auto stream_it = global_stream_store.find(key);
        if (stream_it != global_stream_store.end() && !stream_it->second.empty()) {
            const auto& last_entry = stream_it->second.back();
            string last_id = last_entry.id;
            size_t last_separator_pos = last_id.find('-');
            if (last_separator_pos != string::npos) {
                last_timestamp_str = last_id.substr(0, last_separator_pos);
                last_sequence_str = last_id.substr(last_separator_pos + 1);
            }
        }
    }

    if (id == "*") {
        auto now = system_clock::now();
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
        string timestamp_str = to_string(ms);

        if (timestamp_str == last_timestamp_str) {
            int64_t seq = stoll(last_sequence_str);
            id = timestamp_str + "-" + to_string(seq + 1);
        } else {
            id = timestamp_str + "-0";
        }
    }

    // validating id (each id has two parts: timestamp and sequence number)
    // the timestamp part should be greater than last entry's timestamp
    // If the millisecondsTime part of the ID is equal to the millisecondsTime of the last entry,
    // the sequenceNumber part of the ID should be greater than the sequenceNumber of the last entry.
    // If the stream is empty, the ID should be greater than 0-0

    size_t separator_pos = id.find('-');
    if (separator_pos == string::npos) {
        return "-ERR invalid ID format\r\n";
    }

    string timestamp_str = id.substr(0, separator_pos);
    string sequence_str = id.substr(separator_pos + 1);

    if (sequence_str == "*"){
        if (last_timestamp_str == timestamp_str) {
            int64_t last_sequence = stoll(last_sequence_str);
            sequence_str = to_string(last_sequence + 1);
        }
        else{
            sequence_str = "0";
        }

        id = timestamp_str + "-" + sequence_str;

    }

    int64_t timestamp = stoll(timestamp_str);
    int64_t sequence = stoll(sequence_str);

    int64_t last_timestamp = stoll(last_timestamp_str);
    int64_t last_sequence = stoll(last_sequence_str);
    
    if(timestamp <=0 and sequence <= 0) {
        return "-ERR The ID specified in XADD must be greater than 0-0\r\n";
    }

        
    if(timestamp < last_timestamp || (timestamp == last_timestamp && sequence <= last_sequence)){
        return "-ERR The ID specified in XADD is equal or smaller than the target stream top item\r\n";
    }
    

    unordered_map<string, string> fields;

    for(int64_t i=3;i<args.size();i+=2) {
        if(i+1 >= args.size()) {
            return "-ERR wrong number of arguments for 'xadd'\r\n";
        }
        if(args[i].get_type() != RESPType::BulkString && args[i].get_type() != RESPType::SimpleString) {
            return "-ERR invalid field name type\r\n";
        }
        if(args[i + 1].get_type() != RESPType::BulkString && args[i + 1].get_type() != RESPType::SimpleString) {
            return "-ERR invalid field value type\r\n";
        }
        fields[args[i].get_string_value()] = args[i + 1].get_string_value();
    }

    {
        lock_guard<mutex> lock(stream_store_mutex);
        global_stream_store[key].push_back({id, fields});
        stream_updated = true;
        stream_cv.notify_all();
    }
    

    string response = "$" + to_string(id.size()) + "\r\n" + id + "\r\n";
    return response;
}

string handle_xrange(const vector<RESPObject>& args) {
    if (args.size() != 4) {
        return "-ERR wrong number of arguments for 'xrange'\r\n";
    }

    string key = args[1].get_string_value();
    string start_id = args[2].get_string_value();
    string end_id = args[3].get_string_value();

    if (start_id == "-") start_id = "0";
    if (end_id == "+") end_id = "9999999999999-9999999999999"; 

    lock_guard<mutex> lock(stream_store_mutex);
    
    auto it = global_stream_store.find(key);
    if (it == global_stream_store.end()) {
        return "*0\r\n"; 
    }

    const deque<StreamEntry>& stream = it->second;
    vector<StreamEntry> result;

    for (const auto& entry : stream) {

        // size_t separator_pos = entry.id.find('-');
        // string entry_timestamp = entry.id.substr(0, separator_pos);
        if (entry.id >= start_id && entry.id <= end_id) {
            result.push_back(entry);
        }

    }

    string response = "*" + to_string(result.size()) + "\r\n";

    for (const auto& entry : result) {
        response += "*2\r\n";
        response += "$" + to_string(entry.id.size()) + "\r\n" + entry.id + "\r\n";

        // Number of fields (each field-value is 2 items)
        response += "*" + to_string(entry.fields.size() * 2) + "\r\n";
        for (const auto& field : entry.fields) {
            response += "$" + to_string(field.first.size()) + "\r\n" + field.first + "\r\n";
            response += "$" + to_string(field.second.size()) + "\r\n" + field.second + "\r\n";
        }
    }
    return response;
}

string handle_xread(const vector<RESPObject>& args){

    if(args.size()<3){
        return "-ERR wrong number of arguments for 'xread'\r\n";
    }

    int64_t block_ms = -1;
    int64_t index = 1;

    if(args[1].get_string_value() == "block"){
        if(args.size()<5){
            return "-ERR wrong number of arguments for 'xread' with BLOCK\r\n";
        }
        try{
            block_ms = stoll(args[2].get_string_value());
            if(block_ms < 0){
                return "-ERR timeout is negative\r\n";
            }
        } 
        catch(...){
            return "-ERR timeout is not an integer or out of range\r\n";
        }
        if(args[3].get_string_value() != "streams"){
            return "-ERR syntax error near 'STREAMS'\r\n";
        }
        index = 4;
    } 
    else if(args[1].get_string_value() == "streams"){
        index = 2;
    }
    else{
        return "-ERR syntax error near '" + args[1].get_string_value() + "'\r\n";
    }

    int64_t num_keys = (args.size() - index) / 2;
    if((args.size() - index)%2 != 0 || num_keys<=0){
        return "-ERR wrong number of arguments for 'xread'\r\n";
    }
    vector<string> keys;
    vector<string> ids;
    vector<string> effective_ids; 

    for(int64_t i=0;i<num_keys;i++){
        const auto& key_arg = args[index + i];
        const auto& id_arg = args[index + num_keys + i];

        if((key_arg.get_type() != RESPType::BulkString && key_arg.get_type() != RESPType::SimpleString) ||
            (id_arg.get_type() != RESPType::BulkString && id_arg.get_type() != RESPType::SimpleString)){
            return "-ERR invalid key or ID type\r\n";
        }

        keys.push_back(key_arg.get_string_value());
        string id_str = id_arg.get_string_value();
        ids.push_back(id_str);
        
        if(id_str == "$"){
            auto it = global_stream_store.find(key_arg.get_string_value());
            if(it != global_stream_store.end() && !it->second.empty()){
                effective_ids.push_back(it->second.back().id);
            }
            else{
                effective_ids.push_back("0-0");
            }
        } 
        else{
            effective_ids.push_back(id_str);
        }
    }
    
    if(keys.size() != ids.size()){
        return "-ERR keys and IDs count mismatch\r\n";
    }

    
    unique_lock<mutex> lock(stream_store_mutex);

    auto check_for_matches = [&]() -> string {
        
        string response = "";
        int matched_streams = 0;

        for(size_t i = 0; i < keys.size(); i++){
            const string& key = keys[i];
            const string& min_id = effective_ids[i]; 

            auto it = global_stream_store.find(key);
            if(it == global_stream_store.end()){
                continue;
            }

            const deque<StreamEntry>& stream = it->second;
            vector<StreamEntry> matching_entries;
            for(const auto& entry : stream){
                
                if(entry.id > min_id){
                    matching_entries.push_back(entry);
                }
            }

            if(!matching_entries.empty()){
                matched_streams++;
                response += "*2\r\n";
                response += "$" + to_string(key.size()) + "\r\n" + key + "\r\n";
                response += "*" + to_string(matching_entries.size()) + "\r\n";

                for(const auto& entry : matching_entries){
                    response += "*2\r\n";
                    response += "$" + to_string(entry.id.size()) + "\r\n" + entry.id + "\r\n";
                    response += "*" + to_string(entry.fields.size() * 2) + "\r\n";

                    for(const auto& field : entry.fields){
                        response += "$" + to_string(field.first.size()) + "\r\n" + field.first + "\r\n";
                        response += "$" + to_string(field.second.size()) + "\r\n" + field.second + "\r\n";
                    }
                }

            }
        }

        if(matched_streams > 0){
            return "*" + to_string(matched_streams) + "\r\n" + response;
        }
        return "";
    };

    // For $ IDs with blocking, we need to update effective_ids when blocking starts
    // to ensure we only get entries added after the blocking begins
    auto update_dollar_ids = [&]() {
        for(size_t i = 0; i < keys.size(); i++){
            if(ids[i] == "$"){
                const string& key = keys[i];
                auto it = global_stream_store.find(key);
                if(it != global_stream_store.end() && !it->second.empty()){
                    // Update to the current last entry ID
                    effective_ids[i] = it->second.back().id;
                }
                else{
                    // Keep "0-0" if stream still doesn't exist or is empty
                    effective_ids[i] = "0-0";
                }
            }
        }
    };

    string initial_result = check_for_matches();
    if(!initial_result.empty()){
        return initial_result;
    }

    if(block_ms < 0){
        return "$-1\r\n"; 
    }

    // Update $ IDs to current state before starting to block
    update_dollar_ids();

    if(block_ms == 0){
        while(true){
            stream_cv.wait(lock, [&] {
                return stream_updated;
            });
            
            stream_updated = false;
            
            string result = check_for_matches();
            if (!result.empty()) {
                return result;
            }
        }
    } 
    else{
        auto timeout = milliseconds(block_ms);
        auto start = steady_clock::now();

        while(steady_clock::now() - start < timeout){
            auto remaining_time = timeout - duration_cast<milliseconds>(steady_clock::now() - start);
            
            bool success = stream_cv.wait_for(lock, remaining_time, [&] {
                return stream_updated;
            });

            if(!success){
                return "$-1\r\n";
            }

            stream_updated = false;

            string result = check_for_matches();
            if(!result.empty()){
                return result;
            }
        }
        
        return "$-1\r\n";
    }
    
    return "$-1\r\n";
}

string handle_info(const vector<RESPObject>& args, const vector<pair<string, string>>& replica_info) {
    string body = "# Replication\r\n";
    for(const auto& info : replica_info){
        body += info.first + ":" + info.second + "\r\n";
    }
    string response = "$" + to_string(body.size()) + "\r\n" + body + "\r\n";
    return response;
}

string hex_to_binary(const string& hex) {
    string binary;
    for (size_t i = 0; i < hex.length(); i += 2) {
        string byte_str = hex.substr(i, 2);
        char byte = (char) strtol(byte_str.c_str(), nullptr, 16);
        binary.push_back(byte);
    }
    return binary;
}


string handle_command(const RESPObject& obj, int client_fd, const vector<pair<string, string>>& replica_info) {
    if(obj.get_type() != RESPType::Array || obj.get_array().empty()){
        string response = "-ERR Invalid command format\r\n";
        return response;
    }

    string command = obj.get_array()[0].get_string_value();
    for(int i=0; i<command.size();i++){
        if(command[i] >= 'a' && command[i] <= 'z') {
            command[i] = command[i] - ('a'-'A');
        }
    }

    string response;

    if(command == "PING") response = "+PONG\r\n";
    else if(command == "REPLCONF") response = "+OK\r\n";  
    else if(command == "PSYNC"){
        string repl_id = replica_info[1].second;
        response = "+FULLRESYNC " + repl_id + " 0\r\n";
        string fake_rdb_in_hex = "524544495330303131fa0972656469732d76657205372e322e30fa0a72656469732d62697473c040fa056374696d65c26d08bc65fa08757365642d6d656dc2b0c41000fa08616f662d62617365c000fff06e3bfec0ff5aa2";
        string fake_rdb_in_binary = hex_to_binary(fake_rdb_in_hex);
        response += "$" + to_string(fake_rdb_in_binary.length()) + "\r\n" + fake_rdb_in_binary;
    }
    else if(command == "ECHO") response = handle_echo(obj.get_array());
    else if(command == "MULTI") response = handle_multi(obj.get_array(), client_fd); 
    else if(command == "EXEC") response = handle_exec(obj.get_array(), client_fd, replica_info);  
    else if(command == "DISCARD") response = handle_discard(obj.get_array(), client_fd);
    else if(command == "SET") response = handle_set(obj.get_array(), client_fd);
    else if(command == "INCR") response = handle_incr(obj.get_array(), client_fd);
    else if(command == "GET") response = handle_get(obj.get_array(), client_fd);
    else if(command == "RPUSH") response = handle_rpush(obj.get_array());
    else if(command == "LPUSH") response = handle_lpush(obj.get_array());
    else if(command == "LRANGE") response = handle_lrange(obj.get_array());
    else if(command == "LLEN") response = handle_llen(obj.get_array());
    else if(command == "LPOP") response = handle_lpop(obj.get_array());
    else if(command == "BLPOP") response = handle_blpop(obj.get_array());
    else if(command == "TYPE") response = handle_type(obj.get_array());
    else if(command == "XADD") response = handle_xadd(obj.get_array());
    else if(command == "XRANGE") response = handle_xrange(obj.get_array());
    else if(command == "XREAD") response = handle_xread(obj.get_array());
    else if(command == "INFO") response = handle_info(obj.get_array(), replica_info);
    
    else response = "-ERR Unknown command: " + command + "\r\n";

    if (client_fd == -1) {
        return ""; // Empty response for replication
    }
    
    return response;

}
