#pragma once

// Control server for Dispatcher (github.com/Krafft-Media/Dispatcher): newline-delimited JSON over
// TCP on 127.0.0.1, one request per line, one response line per request. Started with
// --control-port; when CEMU_DISPATCHER_TOKEN is set, a connection must authenticate first.
namespace DispatcherControl
{
	constexpr int kProtocolVersion = 1;

	void Start(uint16 port);
	void Stop();
}
