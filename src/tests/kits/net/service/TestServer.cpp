#include "TestServer.h"

#include <netinet/in.h>
#include <posix/libgen.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sstream>

#include <TestShell.h>
#include <AutoDeleter.h>

namespace {

template <typename T>
std::string to_string(T value)
{
	std::ostringstream s;
	s << value;
	return s.str();
}

}


TestServer::TestServer()
	:
	fChildPid(-1),
	fSocketFd(-1),
	fServerPort(0)
{
}


TestServer::~TestServer()
{
	if (fChildPid != -1) {
		kill(fChildPid, SIGTERM);

		pid_t result = -1;
		while (result != fChildPid) {
			result = waitpid(fChildPid, NULL, 0);
		}
	}

	if (fSocketFd != -1) {
		::close(fSocketFd);
		fSocketFd = -1;
	}
}


// The job of this method is to spawn a child process that will later be killed
// by the destructor. The steps are roughly:
//
// 1. Choose a random TCP port by binding to the loopback interface.
// 2. Spawn a child Python process to run testserver.py.
// 3. Return immediately allowing the tests to be performed by the caller of
//    TestServer::Start(). We don't have to wait for the child process to start
//    up because the socket has already been created. The tests will block
//    until accept() is called in the child.
status_t TestServer::Start()
{
	// Bind to a random unused TCP port.
	{
		int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (socket_fd == -1) {
			fprintf(
				stderr,
				"ERROR: Unable to create socket: %s\n",
				strerror(errno));
			return B_ERROR;
		}

		fSocketFd = socket_fd;

		struct sockaddr_in server_address;
		server_address.sin_family = AF_INET;
		server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		int bind_result = ::bind(
			socket_fd,
			reinterpret_cast<struct sockaddr*>(&server_address),
			sizeof(server_address));
		if (bind_result == -1) {
			fprintf(
				stderr,
				"ERROR: Unable to bind to loopback interface: %s\n",
				strerror(errno));
			return B_ERROR;
		}

		socklen_t server_address_length = sizeof(server_address);
		getsockname(
			socket_fd,
			reinterpret_cast<struct sockaddr*>(&server_address),
			&server_address_length);

		fServerPort = server_address.sin_port;
	}

	fprintf(stderr, "Binding to port %d\n", fServerPort);

	pid_t child = fork();
	if (child < 0)
		return B_ERROR;

	if (child > 0) {
		// The child process has started. It may take a short amount of time
		// before the child process is ready to call accept(), but that's OK.
		//
		// Since the socket has already been created above, the tests will not
		// get ECONNREFUSED and will block until the child process calls
		// accept(). So we don't have to busy loop here waiting for a
		// connection to the child.
		sleep(1);
		fChildPid = child;
		return B_OK;
	}

	// This is the child process. We can exec the server process.
	char* testFileSource = strdup(__FILE__);
	MemoryDeleter _(testFileSource);

	std::string testSrcDir(dirname(testFileSource));
	std::string testServerScript = testSrcDir + "/" + "testserver.py";

	// TODO: Use this for generated TLS cert
	// std::string testGeneratedDir = BTestShell::GlobalTestDir();

	std::string socket_fd_string = to_string(fSocketFd);
	std::string server_port_string = to_string(fServerPort);

	execl(
		"/bin/python3",
		"/bin/python3",
		testServerScript.c_str(),
		"--port", server_port_string.c_str(),
		"--fd", socket_fd_string.c_str(),
		NULL);

	// If we reach this point we failed to load the Python image.
	fprintf(
		stderr,
		"Unable to spawn %s: %s\n",
		testServerScript.c_str(),
		strerror(errno));
	exit(1);
}


uint16_t TestServer::Port() const
{
	return fServerPort;
}


BUrl TestServer::BaseUrl() const
{
	std::string baseUrl = "http://127.0.0.1:" + to_string(fServerPort) + "/";
	return BUrl(baseUrl.c_str());
}
