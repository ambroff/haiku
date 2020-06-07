/*
 * Copyright 2020 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *   Kyle Ambroff-Kao, kyle@ambroffkao.com
 */
#include "SecureSocketTest.h"

#include <sstream>
#include <string>
#include <vector>

#include <OS.h>
#include <os/net/SecureSocket.h>

#include <cppunit/TestAssert.h>
#include <cppunit/TestCaller.h>
#include <cppunit/TestSuite.h>


namespace {
int32 stop;


template <typename T>
std::string to_string(T value)
{
	std::ostringstream s;
	s << value;
	return s.str();
}


int32 send_signal_repeatedly(void*)
{
	while (atomic_get(&stop) != 1) {
		//raise(SIGWINCH);
	}

	return 0;
}


void exec(const std::vector<std::string>& args)
{
	const char** argv = new const char*[args.size() + 1];

	for (size_t i = 0; i < args.size(); ++i) {
		argv[i] = args[i].c_str();
	}
	argv[args.size()] = NULL;

	execv(args[0].c_str(), const_cast<char* const*>(argv));
	delete[] argv;
}


class ChildProcess {
public:
	ChildProcess()
		:
		fChildPid(-1)
	{
	}

	~ChildProcess()
	{
		if (fChildPid != -1) {
			::kill(fChildPid, SIGTERM);

			pid_t result = -1;
			while (result != fChildPid) {
				result = ::waitpid(fChildPid, NULL, 0);
			}
		}
	}

	status_t Start(const std::vector<std::string>& args)
	{
		if (fChildPid != -1) {
			return B_ALREADY_RUNNING;
		}

		pid_t child = ::fork();
		if (child < 0)
			return B_ERROR;

		if (child > 0) {
			fChildPid = child;
			return B_OK;
		}

		// This is the child process. We can exec image provided in args.
		exec(args);

		// If we reach this point we failed to load the Python image.
		std::ostringstream ostr;

		for (std::vector<std::string>::const_iterator iter = args.begin();
			 iter != args.end();
			 ++iter) {
			ostr << " " << *iter;
		}

		fprintf(
				stderr,
				"Unable to spawn `%s': %s\n",
				ostr.str().c_str(),
				strerror(errno));
		exit(1);
	}

private:
	pid_t				fChildPid;
};



class TestTLSServer {
public:
	TestTLSServer()
		:
		fServerPort(9093), // TODO: Choose an unused port automatically
		fChildProcess(NULL)
	{
	}

	~TestTLSServer()
	{
		if (fChildProcess != NULL) {
			delete fChildProcess;
			fChildProcess = NULL;
		}
	}
	
	status_t Start()
	{	
		// TODO: Generate these temporary paths
		const std::string keyPath("/tmp/securesockettest-key.pem");
		const std::string certPath("/tmp/securesockettest-cert.pem");
		std::string cmd(
			"openssl req -x509 -nodes -subj /CN=127.0.0.1 -newkey rsa:4096 "
			"-days 1 -keyout " + keyPath + " -out " + certPath);
		int certGenerationResult = system(cmd.c_str());
		if (certGenerationResult != 0)
			return B_ERROR;


		std::vector<std::string> serverArgs;
		serverArgs.push_back("openssl");
		serverArgs.push_back("s_server");
		serverArgs.push_back("-accept");
		serverArgs.push_back(to_string(fServerPort));
		serverArgs.push_back("-key");
		serverArgs.push_back(keyPath);
		serverArgs.push_back("-cert");
		serverArgs.push_back(certPath);
		
		fChildProcess = new ChildProcess;
		return fChildProcess->Start(serverArgs);
	}

	uint16 Port() const
	{
		return fServerPort;
	}

private:
	uint16 fServerPort;
	ChildProcess* fChildProcess;
};

}


void SecureSocketTest::InterruptedSyscallTest()
{
	// Start a TLS server
	TestTLSServer server;
	CPPUNIT_ASSERT_EQUAL(B_OK, server.Start());
	
	// Simulate constant resizing of the terminal by sending SIGWINCH to this
	// process over and over again.
	atomic_set(&stop, 0);
	thread_id signalSenderThread = spawn_thread(send_signal_repeatedly, "",
		B_NORMAL_PRIORITY, NULL);
	resume_thread(signalSenderThread);

	snooze(1000000);

	// Connect to the server
	BSecureSocket clientSocket;
	{
		BNetworkAddress serverAddress("127.0.0.1", server.Port());
		CPPUNIT_ASSERT_EQUAL(B_OK, serverAddress.InitCheck());

		status_t connectResult = clientSocket.Connect(serverAddress);
		CPPUNIT_ASSERT_EQUAL(B_OK, connectResult);
	}

	// Write a line of data
	{
	}

	// Read back the same line, which the server should have echoed back to us.
	{
	}

	// Tests are complete, stop signal sender thread.
	atomic_set(&stop, 1);
	status_t threadStatus;
	wait_for_thread(signalSenderThread, &threadStatus);
	CPPUNIT_ASSERT_EQUAL(threadStatus, B_OK);
}


void SecureSocketTest::AddTests(BTestSuite &parent) {
	CppUnit::TestSuite &suite = *new CppUnit::TestSuite("SecureSocketTest");

	suite.addTest(new CppUnit::TestCaller<SecureSocketTest>(
		"SecureSocketTest::InterruptedSyscallTest",
		&SecureSocketTest::InterruptedSyscallTest));

	parent.addTest("SecureSocketTest", &suite);
}
