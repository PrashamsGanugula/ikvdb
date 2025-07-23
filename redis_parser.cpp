#include "redis_parser.h"

using namespace std;

// RESPObject implementation
RESPObject::RESPObject(RESPType t, const string& value, int64_t integer, vector<RESPObject> arr) {
    type = t;
    string_value = value;
    integer_value = integer;
    array = arr;
}

const RESPType RESPObject::get_type() const {
    return type;
}

const string& RESPObject::get_string_value() const {
    return string_value;
}

int64_t RESPObject::get_int_value() {
    return integer_value;
}

const vector<RESPObject> RESPObject::get_array() const {
    return array;
}

// RESPParser implementation
string RESPParser::read_line(const string& input) {
    size_t end_pos = input.find("\r\n", position);
    if (end_pos == string::npos) {
        throw runtime_error("Missing CRLF");
    }
    string line = input.substr(position, end_pos - position);
    position = end_pos + 2;
    return line;
}

int64_t RESPParser::read_integer(const string& input) {
    return stoll(read_line(input));
}

RESPObject RESPParser::parse_simple_string(const string& input) {
    string string_value = read_line(input);
    return RESPObject(RESPType::SimpleString, string_value);
}

RESPObject RESPParser::parse_error(const string& input) {
    string string_value = read_line(input);
    return RESPObject(RESPType::Error, string_value);
}

RESPObject RESPParser::parse_integer(const string& input) {
    int64_t integer_value = read_integer(input);
    return RESPObject(RESPType::Integer, "", integer_value);
}

RESPObject RESPParser::parse_bulk_string(const string& input) {
    int64_t length = read_integer(input);
    if (length < 0) {
        return RESPObject(RESPType::BulkString, "");
    }
    string string_value = input.substr(position, length);
    position += length + 2; // skip \r\n
    return RESPObject(RESPType::BulkString, string_value);
}

RESPObject RESPParser::parse_array(const string& input) {
    int64_t length = read_integer(input);
    vector<RESPObject> array;
    for (int64_t i = 0; i < length; ++i) {
        RESPObject obj = parse(input);
        array.push_back(obj);
    }
    return RESPObject(RESPType::Array, "", 0, array);
}

RESPObject RESPParser::parse(const string& input) {
    if (position >= input.size()) {
        throw runtime_error("Empty input");
    }

    char type_char = input[position++];
    switch (type_char) {
        case '+': return parse_simple_string(input);
        case '-': return parse_error(input);
        case ':': return parse_integer(input);
        case '$': return parse_bulk_string(input);
        case '*': return parse_array(input);
        default:
            throw runtime_error("Unknown RESP type");
    }
}
