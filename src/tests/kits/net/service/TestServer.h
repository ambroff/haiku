#ifndef TEST_SERVER_H
#define TEST_SERVER_H

#include <os/support/SupportDefs.h>
#include <os/support/Url.h>


class TestServer {
public:
	TestServer();
	~TestServer();

	status_t	Start();
	BUrl		BaseUrl()	const;

private:
	pid_t		fChildPid;
	int			fSocketFd;
	uint16_t	fServerPort;
};


#endif // TEST_SERVER_H
