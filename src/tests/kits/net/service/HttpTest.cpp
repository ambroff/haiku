/*
 * Copyright 2010, Christophe Huriaux
 * Copyright 2014, Haiku, inc.
 * Distributed under the terms of the MIT licence
 */


#include "HttpTest.h"


#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>

#include <HttpRequest.h>
#include <NetworkKit.h>
#include <UrlProtocolListener.h>

#include <cppunit/TestCaller.h>


namespace {

class TestListener : public BUrlProtocolListener {
public:
	TestListener(const std::string& expectedResponseBody,
				 const HttpHeaderMap& expectedResponseHeaders)
		:
		fExpectedResponseBody(expectedResponseBody),
		fExpectedResponseHeaders(expectedResponseHeaders)
	{
	}

	virtual void DataReceived(
		BUrlRequest *caller,
		const char *data,
		off_t position,
		ssize_t size)
	{
		std::copy_n(
			data + position,
			size,
			std::back_inserter(fActualResponseBody));
	}

	virtual void HeadersReceived(
		BUrlRequest* caller,
		const BUrlResult& result)
	{
		const BHttpResult& http_result
			= dynamic_cast<const BHttpResult&>(result);
		const BHttpHeaders& headers = http_result.Headers();

		for (int32 i = 0; i < headers.CountHeaders(); ++i) {
			const BHttpHeader& header = headers.HeaderAt(i);
			fActualResponseHeaders[std::string(header.Name())]
				= std::string(header.Value());
		}
	}

	void Verify()
	{
		CPPUNIT_ASSERT_EQUAL(fExpectedResponseBody, fActualResponseBody);

		for (HttpHeaderMap::iterator iter = fActualResponseHeaders.begin();
			 iter != fActualResponseHeaders.end();
			 ++iter)
		{
			CPPUNIT_ASSERT_EQUAL_MESSAGE(
				"(header " + iter->first + ")",
				fExpectedResponseHeaders[iter->first],
				iter->second);
		}
		CPPUNIT_ASSERT_EQUAL(fExpectedResponseHeaders.size(), fActualResponseHeaders.size());
	}

private:
	std::string fExpectedResponseBody;
	std::string fActualResponseBody;

	HttpHeaderMap fExpectedResponseHeaders;
	HttpHeaderMap fActualResponseHeaders;
};


void SendAuthenticatedRequest(
	BUrlContext &context,
	BUrl &testUrl,
	const HttpHeaderMap &expectedResponseHeaders)
{
	std::string expectedResponseBody(
		"Path: /auth/basic/walter/secret\r\n"
		"\r\n"
		"Headers:\r\n"
		"--------\r\n"
		"Host: 192.168.1.17:9090\r\n"
		"Accept: */*\r\n"
		"Accept-Encoding: gzip\r\n"
		"Connection: close\r\n"
		"User-Agent: Services Kit (Haiku)\r\n"
		"Referer: http://192.168.1.17:9090/auth/basic/walter/secret\r\n"
		"Authorization: Basic d2FsdGVyOnNlY3JldA==\r\n");
	TestListener listener(expectedResponseBody, expectedResponseHeaders);

	BHttpRequest request(testUrl, false, "HTTP", &listener, &context);
	request.SetUserName("walter");
	request.SetPassword("secret");

	CPPUNIT_ASSERT(request.Run());

	while (request.IsRunning())
		snooze(10);

	CPPUNIT_ASSERT_EQUAL(B_OK, request.Status());

	const BHttpResult &result =
		dynamic_cast<const BHttpResult &>(request.Result());
	CPPUNIT_ASSERT_EQUAL(200, result.StatusCode());
	CPPUNIT_ASSERT_EQUAL(BString("OK"), result.StatusText());
	CPPUNIT_ASSERT_EQUAL(214, result.Length());

	listener.Verify();
}

}


HttpTest::HttpTest()
	:
	fBaseUrl("http://192.168.1.17:9090/")
{
}


HttpTest::~HttpTest()
{
}


void
HttpTest::GetTest()
{
	BUrl testUrl(fBaseUrl, "/");
	BUrlContext* context = new BUrlContext();
	context->AcquireReference();

	std::string expectedResponseBody(
		"Path: /\r\n"
		"\r\n"
		"Headers:\r\n"
		"--------\r\n"
		"Host: 192.168.1.17:9090\r\n"
		"Accept: */*\r\n"
		"Accept-Encoding: gzip\r\n"
		"Connection: close\r\n"
		"User-Agent: Services Kit (Haiku)\r\n");
	HttpHeaderMap expectedResponseHeaders;
	expectedResponseHeaders["Content-Encoding"] = "gzip";
	expectedResponseHeaders["Content-Length"] = "147";
	expectedResponseHeaders["Content-Type"] = "text/plain";
	expectedResponseHeaders["Date"] = "Sun, 09 Feb 2020 19:32:42 GMT";
	expectedResponseHeaders["Server"] = "Test HTTP Server for Haiku";
	TestListener listener(expectedResponseBody, expectedResponseHeaders);

	BHttpRequest request(testUrl, false, "HTTP", &listener, context);
	CPPUNIT_ASSERT(request.Run());
	while (request.IsRunning())
		snooze(10);

	CPPUNIT_ASSERT_EQUAL(B_OK, request.Status());

	const BHttpResult& result
		= dynamic_cast<const BHttpResult&>(request.Result());
	CPPUNIT_ASSERT_EQUAL(200, result.StatusCode());
	CPPUNIT_ASSERT_EQUAL(BString("OK"), result.StatusText());

	CPPUNIT_ASSERT_EQUAL(147, result.Length());

	listener.Verify();

	CPPUNIT_ASSERT(!context->GetCookieJar().GetIterator().HasNext());
		// This page should not set cookies

	context->ReleaseReference();
}


void
HttpTest::GetTestConnectionRefused()
{
}


void
HttpTest::ProxyTest()
{
	BUrl testUrl(fBaseUrl, "/user-agent");

	BUrlContext* c = new BUrlContext();
	c->AcquireReference();
	c->SetProxy("120.203.214.182", 83);

	BHttpRequest t(testUrl);
	t.SetContext(c);

	BUrlProtocolListener l;
	t.SetListener(&l);

	CPPUNIT_ASSERT(t.Run());

	while (t.IsRunning())
		snooze(10);

	CPPUNIT_ASSERT_EQUAL(B_OK, t.Status());

	const BHttpResult& r = dynamic_cast<const BHttpResult&>(t.Result());
	CPPUNIT_ASSERT_EQUAL(200, r.StatusCode());
	CPPUNIT_ASSERT_EQUAL(BString("OK"), r.StatusText());
	CPPUNIT_ASSERT_EQUAL(42, r.Length());
	CPPUNIT_ASSERT(!c->GetCookieJar().GetIterator().HasNext());

	c->ReleaseReference();
}


void
HttpTest::UploadTest()
{
	// The test server will echo the POST body back to us in the HTTP response,
	// so here we load it into memory so that we can compare to make sure that
	// the server received it.
	std::string fileContents;
	{
		std::ifstream inputStream("/system/data/licenses/MIT");
		CPPUNIT_ASSERT(inputStream.is_open());
		fileContents = std::string(
			std::istreambuf_iterator<char>(inputStream),
			std::istreambuf_iterator<char>());
		CPPUNIT_ASSERT(!fileContents.empty());

		// The server
	}

	std::string expectedResponseBody(
		"Path: /post\r\n"
		"\r\n"
		"Headers:\r\n"
		"--------\r\n"
		"Host: 192.168.1.17:9090\r\n"
		"Accept: */*\r\n"
		"Accept-Encoding: gzip\r\n"
		"Connection: close\r\n"
		"User-Agent: Services Kit (Haiku)\r\n"
		"Content-Type: multipart/form-data; boundary=<<BOUNDARY-ID>>\r\n"
		"Content-Length: 1409\r\n"
		"\r\n"
		"Request body:\r\n"
		"-------------\r\n"
		"--<<BOUNDARY-ID>>\r\n"
		"Content-Disposition: form-data; name=\"_uploadfile\";"
		" filename=\"MIT\"\r\n"
		"Content-Type: locale/x-vnd.Be.locale-catalog.default\r\n"
		"\r\n"
		+ fileContents
		+ "\r\n"
		"--<<BOUNDARY-ID>>\r\n"
		"Content-Disposition: form-data; name=\"hello\"\r\n"
		"\r\n"
		"world\r\n"
		"--<<BOUNDARY-ID>>--\r\n"
		"\r\n");
	HttpHeaderMap expectedResponseHeaders;
	expectedResponseHeaders["Content-Encoding"] = "gzip";
	expectedResponseHeaders["Content-Length"] = "925";
	expectedResponseHeaders["Content-Type"] = "text/plain";
	expectedResponseHeaders["Date"] = "Sun, 09 Feb 2020 19:32:42 GMT";
	expectedResponseHeaders["Server"] = "Test HTTP Server for Haiku";
	TestListener listener(expectedResponseBody, expectedResponseHeaders);

	BUrl testUrl(fBaseUrl, "/post");

	BUrlContext context;
	BHttpRequest request(testUrl, false, "HTTP", &listener, &context);

	BHttpForm form;
	form.AddString("hello", "world");
	CPPUNIT_ASSERT_EQUAL(
		B_OK,
		form.AddFile("_uploadfile", BPath("/system/data/licenses/MIT")));

	request.SetPostFields(form);

	CPPUNIT_ASSERT(request.Run());

	while (request.IsRunning())
		snooze(10);

	CPPUNIT_ASSERT_EQUAL(B_OK, request.Status());

	const BHttpResult &result =
		dynamic_cast<const BHttpResult &>(request.Result());
	CPPUNIT_ASSERT_EQUAL(200, result.StatusCode());
	CPPUNIT_ASSERT_EQUAL(BString("OK"), result.StatusText());
	CPPUNIT_ASSERT_EQUAL(925, result.Length());

	listener.Verify();
}


void
HttpTest::AuthBasicTest()
{
	BUrlContext context;
	
	BUrl testUrl(fBaseUrl, "/auth/basic/walter/secret");

	HttpHeaderMap expectedResponseHeaders;
	expectedResponseHeaders["Content-Encoding"] = "gzip";
	expectedResponseHeaders["Content-Length"] = "214";
	expectedResponseHeaders["Content-Type"] = "text/plain";
	expectedResponseHeaders["Date"] = "Sun, 09 Feb 2020 19:32:42 GMT";
	expectedResponseHeaders["Server"] = "Test HTTP Server for Haiku";
	expectedResponseHeaders["Www-Authenticate"] = "Basic realm=\"Fake Realm\"";

	SendAuthenticatedRequest(context, testUrl, expectedResponseHeaders);

	CPPUNIT_ASSERT(!context.GetCookieJar().GetIterator().HasNext());
		// This page should not set cookies
}


void
HttpTest::AuthBasicTestNotAuthorized()
{
}


void
HttpTest::AuthDigestTest()
{
	BUrlContext context;

	BUrl testUrl(fBaseUrl, "/auth/digest/walter/secret");

	HttpHeaderMap expectedResponseHeaders;
	expectedResponseHeaders["Access-Control-Allow-Credentials"] = "true";
	expectedResponseHeaders["Access-Control-Allow-Origin"] = "*";
	expectedResponseHeaders["Content-Length"] = "49";
	expectedResponseHeaders["Content-Type"] = "application/json";
	expectedResponseHeaders["Date"] = "Sun, 09 Feb 2020 19:32:42 GMT";
	expectedResponseHeaders["Server"] = "Test HTTP Server for Haiku";
	expectedResponseHeaders["Set-Cookie"] = "stale_after=never; Path=/";
	expectedResponseHeaders["Www-Authenticate"]
		= "Digest realm=\"user@shredder\", "
		"nonce=\"54f03096e39fc96b80fc41f6dac4e489\", "
		"qop=\"auth\", "
		"opaque=\"ef3dfdd63cd2bba0af0f3a2c7806f40b\", "
		"algorithm=MD5, "
		"stale=FALSE";

	SendAuthenticatedRequest(context, testUrl, expectedResponseHeaders);

	std::map<BString, BString> cookies;
	BNetworkCookieJar::Iterator iter
		= context.GetCookieJar().GetIterator();
	while (iter.HasNext()) {
		const BNetworkCookie* cookie = iter.Next();
		cookies[cookie->Name()] = cookie->Value();
	}
	CPPUNIT_ASSERT_EQUAL(2, cookies.size());
	CPPUNIT_ASSERT_EQUAL(BString("fake_value"), cookies["fake"]);
	CPPUNIT_ASSERT_EQUAL(BString("never"), cookies["stale_after"]);
}


/* static */ template<class T> void
HttpTest::_AddCommonTests(BString prefix, CppUnit::TestSuite& suite)
{
	BString name;

	name = prefix;
	name << "GetTest";
	suite.addTest(new CppUnit::TestCaller<T>(name.String(), &T::GetTest));

	name = prefix;
	name << "GetTestConnectionRefused";
	suite.addTest(new CppUnit::TestCaller<T>(
		name.String(),
		&T::GetTestConnectionRefused));

	name = prefix;
	name << "UploadTest";
	suite.addTest(new CppUnit::TestCaller<T>(name.String(), &T::UploadTest));

	name = prefix;
	name << "AuthBasicTest";
	suite.addTest(new CppUnit::TestCaller<T>(name.String(), &T::AuthBasicTest));

	// name = prefix;
	// name << "AuthDigestTest";
	// suite.addTest(new CppUnit::TestCaller<T>(name.String(), &T::AuthDigestTest));
}


/* static */ void
HttpTest::AddTests(BTestSuite& parent)
{
	{
		CppUnit::TestSuite& suite = *new CppUnit::TestSuite("HttpTest");

		// HTTP + HTTPs
		_AddCommonTests<HttpTest>("HttpTest::", suite);

		// TODO: reaches out to some mysterious IP 120.203.214.182 which does
		// not respond anymore?
		//suite.addTest(new CppUnit::TestCaller<HttpTest>("HttpTest::ProxyTest",
		//	&HttpTest::ProxyTest));

		parent.addTest("HttpTest", &suite);
	}

	{
		CppUnit::TestSuite& suite = *new CppUnit::TestSuite("HttpsTest");

		// HTTP + HTTPs
		_AddCommonTests<HttpsTest>("HttpsTest::", suite);

		parent.addTest("HttpsTest", &suite);
	}
}


// # pragma mark - HTTPS


HttpsTest::HttpsTest()
	: HttpTest()
{
	fBaseUrl.SetProtocol("https");
}
