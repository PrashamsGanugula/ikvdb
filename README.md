# ikvdb

**ikvdb** is a lightweight in-memory key-value database implemented in **C++**. Inspired by Redis, it supports fundamental command parsing and storage features. Ideal for learning, prototyping, or extending key-value storage concepts.

---

## Features

- **In-memory key-value storage**
- **Basic Redis-style command support**
- **Modular C++ design** for easy extension and maintenance

---

## Getting Started

### Prerequisites

- C++17 compiler (e.g., g++, clang++)
- Linux or compatible POSIX environment
- (Optional) `make` for simplified builds

### Building

Clone this repository and compile:

```
git clone https://github.com/PrashamsGanugula/ikvdb.git
cd ikvdb
g++ -std=c++17 -o ikvdb Server.cpp database.cpp handle_redis_commands.cpp redis_parser.cpp
```


### Running

Start the server:

```
./ikvdb
```


---

## Project Structure

ikvdb/
├── Server.cpp # TCP server logic
├── database.cpp / .h # Core key-value storage
├── handle_redis_commands.cpp / .h # Redis command handling
├── redis_parser.cpp / .h # Command parser for Redis protocol

---

## Usage Example

Interact with the server using a client (like `netcat`) and send commands in the Redis protocol format.

> Current supported commands and formats are documented in source code comments.

---

