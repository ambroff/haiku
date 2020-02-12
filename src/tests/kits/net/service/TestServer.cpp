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
		return B_OK;
	}

	char* testFileSource = strdup(__FILE__);
	MemoryDeleter _(testFileSource);

	std::string testSrcDir(dirname(testFileSource));
	std::string testServerScript = testSrcDir + "/" + "testserver.py";

	std::string testGeneratedDir = BTestShell::GlobalTestDir();
}
