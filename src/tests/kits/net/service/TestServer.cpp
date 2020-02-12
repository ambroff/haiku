#include "TestServer.h"

#include <sys/wait.h>
#include <unistd.h>
#include <posix/libgen.h>

#include <TestShell.h>
#include <AutoDeleter.h>


TestServer::TestServer()
	:
	fChildPid(-1)
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
}

status_t TestServer::Start()
{
	pid_t child = fork();
	if (child < 0)
		return B_ERROR;

	if (child > 0) {
		fChildPid = child;
		sleep(1);
		return B_OK;
	}

	char* testFileSource = strdup(__FILE__);
	MemoryDeleter _(testFileSource);

	std::string testSrcDir(dirname(testFileSource));
	std::string testServerScript = testSrcDir + "/" + "testserver.py";

	// TODO: Use this for generated TLS cert
	// std::string testGeneratedDir = BTestShell::GlobalTestDir();

	execl(
		"/bin/python3",
		"/bin/python3",
		"../src/tests/kits/net/service/testserver.py",
		"--port=9090",
		NULL);

	// If we reach this point we failed to load the Python image.
	fprintf(
		stderr,
		"Unable to spawn %s: %s\n",
		testServerScript.c_str(),
		strerror(errno));
	exit(1);
}
