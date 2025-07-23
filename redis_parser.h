#pragma once

#include <iostream>
#include <string>
#include <vector>

enum class RESPType {
    SimpleString,
    Error,
    Integer,
    BulkString,
    Array
};

class RESPObject {
private:
    RESPType type;
    std::string string_value;
    int64_t integer_value;
    std::vector<RESPObject> array;

public:
    RESPObject(RESPType t, const std::string& value = "", int64_t integer = 0, std::vector<RESPObject> arr = {});
    
    const RESPType get_type() const;
    const std::string& get_string_value() const;
    int64_t get_int_value();
    const std::vector<RESPObject> get_array() const;
};

class RESPParser {
private:
    size_t position=0;

    std::string read_line(const std::string& input);
    int64_t read_integer(const std::string& input);

    RESPObject parse_simple_string(const std::string& input);
    RESPObject parse_error(const std::string& input);
    RESPObject parse_integer(const std::string& input);
    RESPObject parse_bulk_string(const std::string& input);
    RESPObject parse_array(const std::string& input);

public:
    RESPObject parse(const std::string& input);
};
