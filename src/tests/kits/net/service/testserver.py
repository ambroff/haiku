"""
HTTP(S) server used for integration testing of ServicesKit.
"""

import optparse
import sys
import http.server


class RequestHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_


def main():
    options = parse_args(sys.argv)

    if options.server_socket_fd:
        server = http.server.HTTPServer((), bind_and_activate=False)
        print('Test server listening on server socket fd', options.server_socket_fd, file=sys.stderr)
    else:
        # A socket hasn't been open for us already, so we'll just use
        # a random port here.
        server = http.server.HTTPServer(('127.0.0.1', 0), RequestHandler)
        print('Test server listening on port', server.server_port, file=sys.stderr)

    try:
        server.serve_forever(0.01)
    except KeyboardInterrupt:
        server.server_close()


def parse_args(argv):
    parser = optparse.OptionParser(usage='Usage: %prog [OPTIONS]', description=__doc__)
    parser.add_option('--use-tls', dest='use_tls', default=False, action='store_true')
    parser.add_option("--fd", dest='server_socket_fd', default=None)
    options, args = parser.parse_args(argv)
    if len(args) > 1:
        parser.error('Unexpected arguments: {}'.format(', '.join(args[1:])))
    return options


if __name__ == '__main__':
    main()
