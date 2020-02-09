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
			// FIXME: Remove this
			if (iter->first == "Date" || iter->first == "Www-Authenticate") {
				continue;
			}
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
		"{\n"
		"  \"authenticated\": true, \n"
		"  \"user\": \"walter\"\n"
		"}\n");
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
	CPPUNIT_ASSERT_EQUAL(49, result.Length());

	listener.Verify();
}

}


HttpTest::HttpTest()
	: fBaseUrl("http://httpbin.org/")
{
}


HttpTest::~HttpTest()
{
}


void
HttpTest::GetTest()
{
	BUrl testUrl(fBaseUrl, "/user-agent");
	BUrlContext* context = new BUrlContext();
	context->AcquireReference();

	std::string expectedResponseBody(
		"{\n"
		"  \"user-agent\": \"Services Kit (Haiku)\"\n"
		"}\n");
	HttpHeaderMap expectedResponseHeaders;
	expectedResponseHeaders["Content-Type"] = "application/json";
	expectedResponseHeaders["Content-Length"] = "43";
	expectedResponseHeaders["Access-Control-Allow-Origin"] = "*";
	expectedResponseHeaders["Access-Control-Allow-Credentials"] = "true";
	expectedResponseHeaders["Server"] = "Werkzeug/1.0.0 Python/3.7.6";
	expectedResponseHeaders["Date"] = ""; // FIXME
	TestListener listener(expectedResponseBody, expectedResponseHeaders);

	BHttpRequest request(testUrl, false, "HTTP", &listener, context);
	CPPUNIT_ASSERT(request.Run());
	while (request.IsRunning())
		snooze(10);

	CPPUNIT_ASSERT_EQUAL(B_OK, request.Status());

	const BHttpResult& r = dynamic_cast<const BHttpResult&>(request.Result());
	CPPUNIT_ASSERT_EQUAL(200, r.StatusCode());
	CPPUNIT_ASSERT_EQUAL(BString("OK"), r.StatusText());

	CPPUNIT_ASSERT_EQUAL(43, r.Length());

	listener.Verify();

	CPPUNIT_ASSERT(!context->GetCookieJar().GetIterator().HasNext());
		// This page should not set cookies

	context->ReleaseReference();
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

printf("%s\n", r.StatusText().String());

	CPPUNIT_ASSERT_EQUAL(200, r.StatusCode());
	CPPUNIT_ASSERT_EQUAL(BString("OK"), r.StatusText());
	CPPUNIT_ASSERT_EQUAL(42, r.Length());
		// Fixed size as we know the response format.
	CPPUNIT_ASSERT(!c->GetCookieJar().GetIterator().HasNext());
		// This page should not set cookies

	c->ReleaseReference();
}


class PortTestListener: public BUrlProtocolListener
{
public:
	virtual			~PortTestListener() {};

			void	DataReceived(BUrlRequest*, const char* data, off_t,
						ssize_t size)
			{
				fResult.Append(data, size);
			}

	BString fResult;
};


void
HttpTest::PortTest()
{
	BUrl testUrl("http://portquiz.net:4242");
	BHttpRequest t(testUrl);

	// portquiz returns more easily parseable results when UA is Wget...
	t.SetUserAgent("Wget/1.15 (haiku testsuite)");

	PortTestListener listener;
	t.SetListener(&listener);

	CPPUNIT_ASSERT(t.Run());

	while (t.IsRunning())
		snooze(10);

	CPPUNIT_ASSERT_EQUAL(B_OK, t.Status());

	const BHttpResult& r = dynamic_cast<const BHttpResult&>(t.Result());
	CPPUNIT_ASSERT_EQUAL(200, r.StatusCode());

	CPPUNIT_ASSERT(listener.fResult.StartsWith("Port 4242 test successful!"));
}


void
HttpTest::UploadTest()
{
	BUrl testUrl(fBaseUrl, "/post");
	BUrlContext c;
	BHttpRequest t(testUrl);

	t.SetContext(&c);

	BHttpForm f;
	f.AddString("hello", "world");
	CPPUNIT_ASSERT(f.AddFile("_uploadfile", BPath("/system/data/licenses/MIT"))
		== B_OK);

	t.SetPostFields(f);

	CPPUNIT_ASSERT(t.Run());

	while (t.IsRunning())
		snooze(10);

	CPPUNIT_ASSERT_EQUAL(B_OK, t.Status());

	const BHttpResult& r = dynamic_cast<const BHttpResult&>(t.Result());
	CPPUNIT_ASSERT_EQUAL(200, r.StatusCode());
	CPPUNIT_ASSERT_EQUAL(BString("OK"), r.StatusText());
	CPPUNIT_ASSERT_EQUAL(466, r.Length());
		// Fixed size as we know the response format.
}


void
HttpTest::AuthBasicTest()
{
	BUrlContext context;
	
	BUrl testUrl(fBaseUrl, "/basic-auth/walter/secret");

	HttpHeaderMap expectedResponseHeaders;
	expectedResponseHeaders["Access-Control-Allow-Credentials"] = "true";
	expectedResponseHeaders["Access-Control-Allow-Origin"] = "*";
	expectedResponseHeaders["Content-Length"] = "49";
	expectedResponseHeaders["Content-Type"] = "application/json";
	expectedResponseHeaders["Date"] = "";
	expectedResponseHeaders["Server"] = "Werkzeug/1.0.0 Python/3.7.6";
	expectedResponseHeaders["Www-Authenticate"] = "Basic realm=\"Fake Realm\"";

	SendAuthenticatedRequest(context, testUrl, expectedResponseHeaders);

	CPPUNIT_ASSERT(!context.GetCookieJar().GetIterator().HasNext());
		// This page should not set cookies
}


void
HttpTest::AuthDigestTest()
{
	BUrlContext context;

	BUrl testUrl(fBaseUrl, "/digest-auth/auth/walter/secret");

	HttpHeaderMap expectedResponseHeaders;
	expectedResponseHeaders["Access-Control-Allow-Credentials"] = "true";
	expectedResponseHeaders["Access-Control-Allow-Origin"] = "*";
	expectedResponseHeaders["Content-Length"] = "49";
	expectedResponseHeaders["Content-Type"] = "application/json";
	expectedResponseHeaders["Date"] = "";
	expectedResponseHeaders["Server"] = "Werkzeug/1.0.0 Python/3.7.6";
	expectedResponseHeaders["Set-Cookie"] = "stale_after=never; Path=/";
	expectedResponseHeaders["Www-Authenticate"]
		= "Digest realm=\"me@kennethreitz.com\", "
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
	name << "UploadTest";
	suite.addTest(new CppUnit::TestCaller<T>(name.String(), &T::UploadTest));

	name = prefix;
	name << "AuthBasicTest";
	suite.addTest(new CppUnit::TestCaller<T>(name.String(), &T::AuthBasicTest));

	name = prefix;
	name << "AuthDigestTest";
	suite.addTest(new CppUnit::TestCaller<T>(name.String(), &T::AuthDigestTest));
}


/* static */ void
HttpTest::AddTests(BTestSuite& parent)
{
	{
		CppUnit::TestSuite& suite = *new CppUnit::TestSuite("HttpTest");

		// HTTP + HTTPs
		_AddCommonTests<HttpTest>("HttpTest::", suite);

		// HTTP-only
		suite.addTest(new CppUnit::TestCaller<HttpTest>(
			"HttpTest::PortTest", &HttpTest::PortTest));

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
