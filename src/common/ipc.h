#pragma once

/* Line-delimited JSON over a named pipe between the background core (server)
 * and the UI process (client). One pipe per Windows session. */

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace hl::ipc {

using json = nlohmann::json;

std::wstring pipe_name();

class Connection;

class Server {
public:
	using Handler = std::function<void(const json &msg, Connection &from)>;

	Server() = default;
	~Server();
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	bool start(Handler handler);
	void stop();
	void broadcast(const json &msg);
	size_t client_count();

private:
	void accept_loop();

	Handler handler_;
	std::thread accept_thread_;
	void *stop_event_ = nullptr;
	std::mutex mutex_;
	std::vector<std::shared_ptr<Connection>> clients_;
	std::atomic<bool> running_{false};
};

class Connection {
public:
	explicit Connection(void *pipe);
	~Connection();

	bool send(const json &msg);
	/* Blocks until a full line arrives. Returns false when the pipe closes. */
	bool read_line(std::string &line, void *stop_event);
	void close();
	bool alive() const { return alive_; }

	std::thread thread;

private:
	void *pipe_;
	void *read_event_;
	void *write_event_;
	std::mutex write_mutex_;
	std::string buffer_;
	std::atomic<bool> alive_{true};
};

class Client {
public:
	~Client();

	/* Tries to connect for up to timeout_ms. */
	bool connect(int timeout_ms);
	bool connected() const { return conn_ && conn_->alive(); }
	bool send(const json &msg);
	/* Non-blocking: pops the next message received from the core. */
	bool poll(json &out);
	void close();

private:
	std::unique_ptr<Connection> conn_;
	std::thread reader_;
	void *stop_event_ = nullptr;
	std::mutex queue_mutex_;
	std::deque<json> queue_;
};

} // namespace hl::ipc
