#include "ipc.h"
#include "log.h"

#include <windows.h>

namespace hl::ipc {

std::wstring pipe_name()
{
	DWORD session = 0;
	ProcessIdToSessionId(GetCurrentProcessId(), &session);
	return L"\\\\.\\pipe\\highlight-core-" + std::to_wstring(session);
}

/* ------------------------------------------------------------------ */
/* Connection                                                         */

Connection::Connection(void *pipe) : pipe_(pipe)
{
	read_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

Connection::~Connection()
{
	close();
	if (thread.joinable()) {
		/* The last reference can be dropped by the connection's own reader thread. */
		if (thread.get_id() == std::this_thread::get_id())
			thread.detach();
		else
			thread.join();
	}
	CloseHandle(read_event_);
	CloseHandle(write_event_);
}

void Connection::close()
{
	bool was = alive_.exchange(false);
	if (was && pipe_ && pipe_ != INVALID_HANDLE_VALUE) {
		CancelIoEx(pipe_, nullptr);
		CloseHandle(pipe_);
		pipe_ = nullptr;
	}
}

bool Connection::send(const json &msg)
{
	if (!alive_)
		return false;
	std::string line = msg.dump() + "\n";

	std::lock_guard lock(write_mutex_);
	if (!alive_ || !pipe_)
		return false;

	OVERLAPPED ov = {};
	ov.hEvent = write_event_;
	ResetEvent(write_event_);
	DWORD written = 0;
	BOOL ok = WriteFile(pipe_, line.data(), (DWORD)line.size(), &written, &ov);
	if (!ok && GetLastError() == ERROR_IO_PENDING) {
		if (WaitForSingleObject(write_event_, 2000) != WAIT_OBJECT_0) {
			CancelIoEx(pipe_, &ov);
			GetOverlappedResult(pipe_, &ov, &written, TRUE);
			return false;
		}
		ok = GetOverlappedResult(pipe_, &ov, &written, FALSE);
	}
	return ok && written == line.size();
}

bool Connection::read_line(std::string &line, void *stop_event)
{
	for (;;) {
		size_t nl = buffer_.find('\n');
		if (nl != std::string::npos) {
			line = buffer_.substr(0, nl);
			buffer_.erase(0, nl + 1);
			return true;
		}
		if (!alive_ || !pipe_)
			return false;

		char chunk[4096];
		OVERLAPPED ov = {};
		ov.hEvent = read_event_;
		ResetEvent(read_event_);
		DWORD got = 0;
		BOOL ok = ReadFile(pipe_, chunk, sizeof(chunk), &got, &ov);
		if (!ok && GetLastError() == ERROR_IO_PENDING) {
			HANDLE handles[2] = {read_event_, stop_event};
			DWORD n = stop_event ? 2 : 1;
			DWORD w = WaitForMultipleObjects(n, handles, FALSE, INFINITE);
			if (w != WAIT_OBJECT_0) {
				CancelIoEx(pipe_, &ov);
				GetOverlappedResult(pipe_, &ov, &got, TRUE);
				return false;
			}
			ok = GetOverlappedResult(pipe_, &ov, &got, FALSE);
		}
		if (!ok || got == 0) {
			alive_ = false;
			return false;
		}
		buffer_.append(chunk, got);
		if (buffer_.size() > 1024 * 1024) { /* a single message never gets this big */
			alive_ = false;
			return false;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Server                                                             */

Server::~Server()
{
	stop();
}

bool Server::start(Handler handler)
{
	if (running_)
		return true;
	handler_ = std::move(handler);
	stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	running_ = true;
	accept_thread_ = std::thread(&Server::accept_loop, this);
	return true;
}

void Server::stop()
{
	if (!running_)
		return;
	running_ = false;
	SetEvent(stop_event_);
	if (accept_thread_.joinable())
		accept_thread_.join();

	std::vector<std::shared_ptr<Connection>> clients;
	{
		std::lock_guard lock(mutex_);
		clients.swap(clients_);
	}
	for (auto &c : clients) {
		c->close();
		if (c->thread.joinable())
			c->thread.join();
	}
	CloseHandle(stop_event_);
	stop_event_ = nullptr;
}

void Server::broadcast(const json &msg)
{
	std::vector<std::shared_ptr<Connection>> clients;
	{
		std::lock_guard lock(mutex_);
		clients = clients_;
	}
	for (auto &c : clients)
		if (c->alive())
			c->send(msg);
}

size_t Server::client_count()
{
	std::lock_guard lock(mutex_);
	size_t n = 0;
	for (auto &c : clients_)
		if (c->alive())
			n++;
	return n;
}

void Server::accept_loop()
{
	std::wstring name = pipe_name();
	HANDLE connect_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

	while (running_) {
		HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
					       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
						       PIPE_REJECT_REMOTE_CLIENTS,
					       PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);
		if (pipe == INVALID_HANDLE_VALUE) {
			HL_ERROR("CreateNamedPipe failed: %lu", GetLastError());
			if (WaitForSingleObject(stop_event_, 1000) == WAIT_OBJECT_0)
				break;
			continue;
		}

		OVERLAPPED ov = {};
		ov.hEvent = connect_event;
		ResetEvent(connect_event);
		BOOL ok = ConnectNamedPipe(pipe, &ov);
		DWORD err = GetLastError();
		bool connected = false;
		if (!ok && err == ERROR_IO_PENDING) {
			HANDLE handles[2] = {connect_event, stop_event_};
			DWORD w = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
			if (w == WAIT_OBJECT_0) {
				DWORD dummy;
				connected = GetOverlappedResult(pipe, &ov, &dummy, FALSE) != 0;
			} else {
				CancelIoEx(pipe, &ov);
			}
		} else if (!ok && err == ERROR_PIPE_CONNECTED) {
			connected = true;
		}

		if (!connected) {
			CloseHandle(pipe);
			continue;
		}

		auto conn = std::make_shared<Connection>(pipe);
		{
			std::lock_guard lock(mutex_);
			/* drop dead clients */
			for (auto it = clients_.begin(); it != clients_.end();) {
				if (!(*it)->alive()) {
					if ((*it)->thread.joinable())
						(*it)->thread.detach();
					it = clients_.erase(it);
				} else {
					++it;
				}
			}
			clients_.push_back(conn);
		}

		std::weak_ptr<Connection> weak = conn;
		conn->thread = std::thread([this, weak]() {
			auto c = weak.lock();
			if (!c)
				return;
			std::string line;
			while (running_ && c->read_line(line, stop_event_)) {
				if (line.empty())
					continue;
				try {
					json msg = json::parse(line);
					if (handler_)
						handler_(msg, *c);
				} catch (const std::exception &e) {
					HL_WARN("Bad IPC message: %s", e.what());
				}
			}
			c->close();
		});
	}

	CloseHandle(connect_event);
}

/* ------------------------------------------------------------------ */
/* Client                                                             */

Client::~Client()
{
	close();
}

bool Client::connect(int timeout_ms)
{
	close();
	std::wstring name = pipe_name();
	ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout_ms;

	HANDLE pipe = INVALID_HANDLE_VALUE;
	for (;;) {
		pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
				   FILE_FLAG_OVERLAPPED, nullptr);
		if (pipe != INVALID_HANDLE_VALUE)
			break;
		DWORD err = GetLastError();
		if (GetTickCount64() >= deadline)
			return false;
		if (err == ERROR_PIPE_BUSY)
			WaitNamedPipeW(name.c_str(), 200);
		else
			Sleep(100);
	}

	conn_ = std::make_unique<Connection>(pipe);
	stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	reader_ = std::thread([this]() {
		std::string line;
		while (conn_->read_line(line, stop_event_)) {
			if (line.empty())
				continue;
			try {
				json msg = json::parse(line);
				std::lock_guard lock(queue_mutex_);
				queue_.push_back(std::move(msg));
				if (queue_.size() > 1000)
					queue_.pop_front();
			} catch (...) {
			}
		}
	});
	return true;
}

bool Client::send(const json &msg)
{
	return conn_ && conn_->send(msg);
}

bool Client::poll(json &out)
{
	std::lock_guard lock(queue_mutex_);
	if (queue_.empty())
		return false;
	out = std::move(queue_.front());
	queue_.pop_front();
	return true;
}

void Client::close()
{
	if (stop_event_)
		SetEvent(stop_event_);
	if (conn_)
		conn_->close();
	if (reader_.joinable())
		reader_.join();
	conn_.reset();
	if (stop_event_) {
		CloseHandle(stop_event_);
		stop_event_ = nullptr;
	}
	std::lock_guard lock(queue_mutex_);
	queue_.clear();
}

} // namespace hl::ipc
