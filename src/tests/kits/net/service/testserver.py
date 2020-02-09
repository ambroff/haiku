"""
HTTP(S) server used for integration testing of ServicesKit.

This service receives HTTP requests and just echos them back in the response.
"""

import optparse
import os
import sys
import http.server
import socket


class RequestHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        """
        Any GET request just gets echoed back to the sender. If the path ends with a numeric component like "/404" or
        "/500", then that value will be set as the status code in the response.

        Note that this isn't meant to replicate expected functionality exactly. Rather than implementing all of these
        status codes as expected per RFC, such as having an empty response body for 201 response, only the functionality
        that is required to handle requests from HttpTests is implemented.
        """
        status_code = 200
        path_parts = os.path.split(self.path)
        try:
            status_code = int(path_parts[-1])
        except ValueError:
            pass

        self.send_response(status_code)
        self.send_header('Content-Type', 'text/plain')
        self.wfile.write('Path: {}\r\n\r\n'.format(self.path).encode('utf-8'))
        self.wfile.write(b'Headers\r\n')
        self.wfile.write(b'-------\r\n')
        for header in self.headers:
            for header_value in self.headers.get_all(header):
                self.wfile.write('{}: {}\r\n'.format(header, header_value).encode('utf-8'))


def main():
    options = parse_args(sys.argv)

    bind_addr = ('127.0.0.1', options.port)

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
