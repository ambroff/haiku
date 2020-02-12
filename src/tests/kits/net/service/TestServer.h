#ifndef TEST_SERVER_H
#define TEST_SERVER_H

#include <os/support/SupportDefs.h>


class TestServer {
public:
	TestServer();
	~TestServer();

	status_t Start();

private:
	pid_t fChildPid;
};


#endif // TEST_SERVER_H
