#!/usr/bin/env python3
"""Read-only, loopback-only PCjs debugger host. Python 3 standard library only."""
import argparse
import hashlib
import json
import mimetypes
import os
from pathlib import Path
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, unquote
from urllib.request import build_opener, HTTPRedirectHandler, Request
from urllib.error import HTTPError, URLError
from typing import cast

HERE = Path(__file__).resolve().parent
DISK = HERE.parent / 'out' / 'disk.img'
CACHE = HERE / '.cache'
# Exact paths only: no user-supplied URL, remote query, prefix wildcard or redirect.
ASSETS = frozenset({
    '/assets/js/xslt-polyfill.min.js',
    '/machines/pcx86/releases/2.25/pcx86.js',
    '/machines/pcx86/xsl/components.xsl',
    '/machines/pcx86/xsl/components.css',
    '/machines/pcx86/ibm/5170/rom/bios/1985-11-15/ATBIOS-REV3.json5',
    '/machines/pcx86/ibm/video/vga/1986-10-27/IBM-VGA.json5',
    '/pcjs-hdd.json',
})
STATIC = {'/': 'index.html', '/debug.js': 'debug.js',
          '/debug.css': 'debug.css', '/machine.xml': 'machine.xml'}
LOCK = threading.Lock()
ERRORS = {}


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def asset(path):
    if path not in ASSETS:
        raise ValueError('Remote path is not allowlisted')
    target = CACHE / path.lstrip('/')
    with LOCK:
        if target.is_file():
            return target.read_bytes()
        try:
            # PCjs's disk resources live on a dedicated fixed pcjs.org subdomain.
            url = ('https://harddisks.pcjs.org/pcx86/20mb/PCDOS330-WIN310-VGA.json'
                   if path == '/pcjs-hdd.json' else 'https://www.pcjs.org' + path)
            req = Request(url,
                          headers={'User-Agent': 'win31c-local-debug/1.0'})
            with build_opener(NoRedirect).open(req, timeout=90) as response:
                data = response.read(100 * 1024 * 1024 + 1)
                if len(data) > 100 * 1024 * 1024:
                    raise ValueError('Remote asset exceeds 100 MiB')
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = target.with_name(target.name + '.tmp')
            temporary.write_bytes(data)
            os.replace(temporary, target)
            ERRORS.pop(path, None)
            return data
        except Exception as exc:
            ERRORS[path] = str(exc)
            raise


def disk_status():
    try:
        data = DISK.read_bytes()
        return {'exists': True, 'bytes': len(data),
                'sha256': hashlib.sha256(data).hexdigest(),
                'mtime_ns': DISK.stat().st_mtime_ns}
    except FileNotFoundError:
        return {'exists': False, 'bytes': 0, 'sha256': None}


class Handler(BaseHTTPRequestHandler):
    def reply(self, status, data, content_type='application/json'):
        if isinstance(data, dict):
            data = json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        # PCjs parses its JavaScript-style XML parameters and JSON5 ROMs with eval.
        self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self' 'unsafe-eval'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self'; object-src 'none'; frame-ancestors 'none'")
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(data)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        # Reject DNS rebinding and non-local Host headers as well as traversal.
        host = self.headers.get('Host', '')
        port = cast(ThreadingHTTPServer, self.server).server_port
        if host not in (f'localhost:{port}', f'127.0.0.1:{port}'):
            return self.reply(403, {'error': 'Local Host header required'})
        parsed = urlsplit(self.path)
        path = unquote(parsed.path)
        if parsed.scheme or parsed.netloc or '\\' in path or '%' in path or any(
                segment in ('.', '..') for segment in path.split('/')):
            return self.reply(400, {'error': 'Invalid path'})
        try:
            if path == '/api/health':
                return self.reply(200, {'service': 'win31c-pcjs-debug', 'ok': True,
                                        'repo': str(HERE.parent)})
            if path == '/api/status':
                with LOCK:
                    errors = dict(ERRORS)
                return self.reply(200, {'service': 'win31c-pcjs-debug',
                                        'disk': disk_status(), 'asset_errors': errors})
            if path == '/api/disk':
                return self.reply(200, DISK.read_bytes(), 'application/octet-stream')
            if path in STATIC:
                data = (HERE / STATIC[path]).read_bytes()
            elif path in ASSETS:
                data = asset(path)
            else:
                return self.reply(404, {'error': 'Route not found or asset not allowlisted'})
            content_type = mimetypes.guess_type(path if path != '/' else 'index.html')[0]
            if path.endswith(('.json', '.json5')):
                content_type = 'application/json'
            if path.endswith('.xsl'):
                content_type = 'application/xml'
            return self.reply(200, data, content_type or 'application/octet-stream')
        except FileNotFoundError:
            self.reply(404, {'error': 'out/disk.img is missing; build the application first'})
        except (HTTPError, URLError, TimeoutError, ValueError, OSError) as exc:
            self.reply(502, {'error': f'Unable to serve {path}: {exc}'})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8310)
    parser.add_argument('--prefetch', action='store_true', help='Fetch fixed PCjs assets then exit')
    args = parser.parse_args()
    if args.prefetch:
        for path in sorted(ASSETS):
            print(f'{path}: {len(asset(path)):,} bytes', flush=True)
        return
    try:
        server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    except OSError as exc:
        parser.exit(1, f'Cannot bind localhost:{args.port}: {exc}\n')
    print(f'win31c PCjs debug: http://localhost:{args.port}/\nCtrl-C stops this server.', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
