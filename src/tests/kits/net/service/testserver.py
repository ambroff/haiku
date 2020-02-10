"""
HTTP(S) server used for integration testing of ServicesKit.

This service receives HTTP requests and just echos them back in the response.

TODO: Handle Accept-Encoding.
"""

import optparse
import os
import sys
import http.server
import socket
import io
import re
import base64


MULTIPART_FORM_BOUNDARY_RE = re.compile(r'^multipart/form-data; boundary=(----------------------------\d+)$')
AUTH_PATH_RE = re.compile(
    r'^/auth/(?P<strategy>(basic|digest))/(?P<username>[a-z0-9]+)/(?P<password>[a-z0-9]+)',
    re.IGNORECASE)


def extract_desired_status_code_from_path(path, default=200):
    status_code = default
    path_parts = os.path.split(path)
    try:
        status_code = int(path_parts[-1])
    except ValueError:
        pass
    return status_code


class RequestHandler(http.server.BaseHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super(RequestHandler, self).__init__(*args, **kwargs)
        self.extra_headers = []

    def do_GET(self, write_response=True):
        """
        Any GET request just gets echoed back to the sender. If the path ends with a numeric component like "/404" or
        "/500", then that value will be set as the status code in the response.

        Note that this isn't meant to replicate expected functionality exactly. Rather than implementing all of these
        status codes as expected per RFC, such as having an empty response body for 201 response, only the functionality
        that is required to handle requests from HttpTests is implemented.
        """
        if self._authorize():
            return

        response_body = self._build_response_body()

        self.send_response(extract_desired_status_code_from_path(self.path, 200))
        self.send_header('Content-Type', 'text/plain')
        self.send_header('Content-Length', len(response_body))
        self.end_headers()

        if write_response:
            self.wfile.write(response_body.encode('utf-8'))

    def do_HEAD(self):
        self.do_GET(False)

    def do_POST(self):
        if self._authorize():
            return

        response_body = self._build_response_body()
        self.send_response(extract_desired_status_code_from_path(self.path, 200))
        self.send_header('Content-Type', 'text/plain')
        self.send_header('Content-Length', len(response_body))
        self.end_headers()
        self.wfile.write(response_body.encode('utf-8'))

    def do_DELETE(self):
        self._not_supported()

    def do_PATCH(self):
        self._not_supported()

    def do_OPTIONS(self):
        self._not_supported()

    def send_response(self, code, message=None):
        self.log_request(code)
        self.send_response_only(code, message)
        self.send_header('Server', 'Test HTTP Server for Haiku')
        self.send_header('Date', 'Sun, 09 Feb 2020 19:32:42 GMT')

    def _build_response_body(self):
        # The post-body may be multi-part/form-data, in which case the client will have generated some
        # random identifier to identify the boundary. If that's the case, we'll replace it here in order to allow
        # the test client to validate the response data without needing to predict the boundary identifier.
        boundary_id_value = None

        output_stream = io.StringIO()
        output_stream.write('Path: {}\r\n\r\n'.format(self.path))
        output_stream.write('Headers:\r\n')
        output_stream.write('--------\r\n')
        for header in self.headers:
            for header_value in self.headers.get_all(header):
                if header == 'Content-Type':
                    match = MULTIPART_FORM_BOUNDARY_RE.match(self.headers.get('Content-Type', 'text/plain'))
                    if match is not None:
                        boundary_id_value = match.group(1)
                        header_value = header_value.replace(boundary_id_value, '<<BOUNDARY-ID>>')
                output_stream.write('{}: {}\r\n'.format(header, header_value))

        content_length = int(self.headers.get('Content-Length', 0))
        if content_length > 0:
            output_stream.write('\r\n')
            output_stream.write('Request body:\r\n')
            output_stream.write('-------------\r\n')

            body_bytes = self.rfile.read(content_length).decode('utf-8')
            if boundary_id_value:
                body_bytes = body_bytes.replace(boundary_id_value, '<<BOUNDARY-ID>>')

            output_stream.write(body_bytes)
            output_stream.write('\r\n')
        return output_stream.getvalue()

    def _not_supported(self):
        self.send_response(405, '{} not supported'.format(self.command))
        self.end_headers()
        self.wfile.write('{} not supported\r\n'.format(self.command).encode('utf-8'))

    def _authorize(self):
        """
        Authorizes the request. If True is returned that means that the request was not authorized and the 4xx response
        has been send to the client.
        """
        match = AUTH_PATH_RE.match(self.path)
        if match is None:
            return False

        strategy = match.group('strategy')
        expected_username = match.group('username')
        expected_password = match.group('password')

        if strategy == 'basic':
            authorization = self.headers.get('Authorization', None)
            if authorization is None:
                self.send_response(401, 'Not authorized')
                self.end_headers()
                return True

            decoded = base64.decodebytes(authorization)
            username, password = decoded.split(':')

            if username != expected_username or password != expected_password:
                self.send_response(401, 'Not authorized')
                self.end_headers()
                return True

            self.extra_headers.append(('Www-Authenticate', 'Basic realm="Fake realm"'))
        elif strategy == 'digest':
            pass
        else:
            raise NotImplementedError('Unimplemented authorization strategy ' + strategy)

        return False


def main():
    options = parse_args(sys.argv)

    bind_addr = (options.bind_addr, options.port)

    if options.server_socket_fd:
        server = http.server.HTTPServer(bind_addr, RequestHandler, bind_and_activate=False)
        server.socket = socket.fromfd(options.server_socket_fd, socket.AF_INET, socket.SOCK_STREAM)
    else:
        # A socket hasn't been open for us already, so we'll just use
        # a random port here.
        server = http.server.HTTPServer(bind_addr, RequestHandler)

    try:
        print('Test server listening on port', server.server_port, file=sys.stderr)
        server.serve_forever(0.01)
    except KeyboardInterrupt:
        server.server_close()


def parse_args(argv):
    parser = optparse.OptionParser(usage='Usage: %prog [OPTIONS]', description=__doc__)
    parser.add_option('--bind-addr', default='127.0.0.1', dest='bind_addr', help='By default only bind to loopback')
    parser.add_option(
        '--use-tls',
        dest='use_tls',
        default=False,
        action='store_true',
        help='If set, a self-signed TLS certificate, key and CA will be generated for testing purposes.')
    parser.add_option('--port', dest='port', default=0, type='int', help='If not specified a random port will be used.')
    parser.add_option(
        "--fd",
        dest='server_socket_fd',
        default=None,
        help='A socket FD to use for accept() instead of binding a new one.')
    options, args = parser.parse_args(argv)
    if len(args) > 1:
        parser.error('Unexpected arguments: {}'.format(', '.join(args[1:])))
    return options


if __name__ == '__main__':
    main()
