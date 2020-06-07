/*
 * Copyright 2020 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *   Kyle Ambroff-Kao, kyle@ambroffkao.com
 */
#ifndef SECURE_SOCKET_TESTS_H
#define SECURE_SOCKET_TESTS_H

#include <TestCase.h>
#include <TestSuite.h>


class SecureSocketTest : public BTestCase {
public:
			void	InterruptedSyscallTest();

	static	void	AddTests(BTestSuite& suite);
};

#endif // SECURE_SOCKET_TESTS_H
