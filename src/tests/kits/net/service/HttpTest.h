/*
 * Copyright 2014 Haiku, inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef HTTP_TEST_H
#define HTTP_TEST_H

#include <map>
#include <string>

#include <Url.h>

#include <TestCase.h>
#include <TestSuite.h>

#include <cppunit/TestSuite.h>


typedef std::map<std::string, std::string> HttpHeaderMap;


class HttpTest: public BTestCase {
public:
										HttpTest();
	virtual								~HttpTest();

								void	GetTest();
								void	GetTestConnectionRefused();
								void	UploadTest();
								void	AuthBasicTest();
								void	AuthBasicTestNotAuthorized();
								void	AuthDigestTest();
								void	ProxyTest();

	static						void	AddTests(BTestSuite& suite);

private:
	template<class T> static	void	_AddCommonTests(BString prefix,
											CppUnit::TestSuite& suite);
};


class HttpsTest: public HttpTest {
public:
								HttpsTest();
};


#endif
