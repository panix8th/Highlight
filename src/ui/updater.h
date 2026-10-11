#pragma once

/* Update check against the project's GitHub releases. Runs only from the window
 * (never the capture process) and only when "Check for updates" is on: one
 * HTTPS request to api.github.com, nothing else is sent. */

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace ui {

struct UpdateInfo {
	std::string version;   /* "0.1.4" */
	std::string page_url;  /* release page */
	std::string setup_url; /* installer download, may be empty */
};

class Updater {
public:
	~Updater();

	/* Starts a background check. `manual` makes the result reported even when up to date. */
	void check(bool manual);
	/* Downloads the installer to %TEMP% and starts it. The caller closes the window. */
	void install(const UpdateInfo &info);
	/* Hides an available update (the user skipped it). */
	void dismiss()
	{
		if (state_ == State::Available)
			state_ = State::Idle;
	}

	enum class State { Idle, Checking, UpToDate, Available, Failed, Downloading, Launched };
	State state() const { return state_; }
	bool manual() const { return manual_; }
	float progress() const { return progress_; }
	UpdateInfo info() const;
	std::string error() const;

	/* true when Highlight was installed by the setup program (not the portable zip) */
	static bool installed_copy();
	/* "0.1.10" > "0.1.9" */
	static bool newer(const std::string &a, const std::string &b);

private:
	void join();

	std::thread thread_;
	std::atomic<State> state_{State::Idle};
	std::atomic<bool> manual_{false};
	std::atomic<float> progress_{0};
	mutable std::mutex mutex_;
	UpdateInfo info_;
	std::string error_;
};

} // namespace ui
