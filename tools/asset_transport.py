"""Content-addressed, compressed transport for the simulator's active assets.

Only resources referenced by the selected status/description files are exposed.
Source endpoints, credentials and arbitrary filesystem paths are never served.
"""
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor
import gzip
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import io
import mimetypes
import os
from pathlib import Path
import re
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import xml.etree.ElementTree as ET

# Assimp's Collada reader expects unprefixed element names, even when a
# default XML namespace is present. Preserve that spelling when rewriting.
ET.register_namespace('', 'http://www.collada.org/2005/11/COLLADASchema')

ASSET_ID = re.compile(r'^[0-9a-f]{64}\.(?:dae|png|jpg|jpeg|glb|gltf|stl|obj|mtl)$')
MAX_ASSET = 64*1024*1024


def digest(data):
    return hashlib.sha256(data).hexdigest()


def local_path(value, parent=None):
    parsed = urllib.parse.urlparse(value)
    if parsed.scheme == 'file':
        if parsed.netloc not in ('', 'localhost'):
            raise ValueError('Only local file resources can be exported')
        return Path(urllib.request.url2pathname(parsed.path)).resolve()
    if parsed.scheme:
        raise ValueError('The asset service does not proxy source URLs')
    return ((parent or Path.cwd())/value).resolve()


def redact(value):
    value = re.sub(r'([?&](?:access_token|key|token)=)[^&\s"\'<>]+', r'\1<redacted>', value, flags=re.I)
    return re.sub(r'eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+', '<redacted-token>', value)


def process_token(pid):
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        return fields[19] if fields[0] != 'Z' else None
    except (OSError, IndexError):
        return None


class AssetRegistry:
    def __init__(self, status=None, session=None, cache_bytes=64*1024*1024, texture_size=0, jpeg_quality=90):
        self.status = Path(status) if status else None
        self.session = Path(session) if session else None
        self.cache_bytes = cache_bytes
        self.texture_size, self.jpeg_quality = texture_size, jpeg_quality
        self.assets = OrderedDict()
        self.files = {}
        self.active = set()
        self.lock = threading.RLock()
        self.counters = dict(http_requests=0, asset_requests=0, bytes_sent=0, evictions=0)

    def register(self, path, seen):
        path = Path(path).resolve()
        stat = path.stat()
        key = str(path), stat.st_ino, stat.st_mtime_ns, stat.st_size
        cached = self.files.get(key)
        if cached in self.assets and all(name in self.assets for name in self.assets[cached]['dependencies']):
            seen.add(cached)
            seen.update(self.assets[cached]['dependencies'])
            self.assets.move_to_end(cached)
            return cached
        if stat.st_size > MAX_ASSET:
            raise ValueError('An asset exceeds the 64 MiB transfer limit')
        data = path.read_bytes()
        extension = path.suffix.lower()
        if extension in {'.png', '.jpg', '.jpeg'} and self.texture_size:
            from PIL import Image
            with Image.open(io.BytesIO(data)) as image:
                image.thumbnail((self.texture_size, self.texture_size), Image.Resampling.LANCZOS)
                opaque = 'A' not in image.getbands() or image.getchannel('A').getextrema() == (255, 255)
                output = io.BytesIO()
                if opaque:
                    image.convert('RGB').save(output, format='JPEG', quality=self.jpeg_quality, subsampling=0)
                    extension = '.jpg'
                else:
                    image.save(output, format='PNG')
                    extension = '.png'
                data = output.getvalue()
        dependencies = []
        if extension == '.dae':
            root = ET.fromstring(data)
            for image in root.findall('.//{*}library_images/{*}image/{*}init_from'):
                if not image.text:
                    continue
                resource = self.register(local_path(image.text, path.parent), seen)
                image.text = resource
                dependencies.append(resource)
            data = ET.tostring(root, encoding='utf-8', xml_declaration=True)
        name = digest(data)+extension
        if not ASSET_ID.fullmatch(name):
            raise ValueError('Unsupported exported mesh or texture format')
        self.assets[name] = dict(data=gzip.compress(data, compresslevel=3, mtime=0), size=len(data),
                                 media_type=mimetypes.guess_type('asset'+extension)[0] or 'application/octet-stream',
                                 dependencies=dependencies)
        self.files[key] = name
        seen.add(name)
        return name

    def snapshot(self):
        with self.lock:
            status = json.loads(self.status.read_text()) if self.status else dict(schema=1, ready=True, geometry=[], credits=[])
            seen = set()
            geometry = []
            for tile in status.get('geometry', []):
                try:
                    asset = self.register(local_path(tile['uri']), seen)
                except FileNotFoundError:
                    continue  # A native retirement raced this status read.
                item = {key: value for key, value in tile.items() if key not in {'uri', 'visuals'}}
                item['uri'] = '/assets/'+asset
                geometry.append(item)
            status['geometry'] = geometry
            for field in ['error', 'source']:
                if field in status: status[field] = redact(status[field])
            descriptions = {}
            if self.session:
                contract = json.loads((self.session/'contract.json').read_text())
                # Whitelist interface information rather than serializing runtime
                # paths, environment snapshots or private driver configuration.
                status['contract'] = {key: contract[key] for key in
                    ['schema', 'backend', 'domain_id', 'shared_frame', 'use_sim_time'] if key in contract}
                status['contract']['transport'] = 'network'
                status['contract']['robots'] = []
                for robot in contract.get('robots', []):
                    name = robot['name']
                    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name):
                        raise ValueError('Invalid robot name in session contract')
                    status['contract']['robots'].append({key: robot[key] for key in
                        ['name', 'namespace', 'topics', 'frames', 'robot_description', 'origin', 'tf_owners'] if key in robot})
                    description = self.session/name/'robot.urdf'
                    if not description.is_file():
                        continue
                    root = ET.fromstring(description.read_text())
                    for element in root.iter():
                        if element.tag not in {'mesh', 'texture'} or not element.get('filename'):
                            continue
                        asset = self.register(local_path(element.get('filename'), description.parent), seen)
                        element.set('filename', '/assets/'+asset)
                    descriptions[name] = ET.tostring(root, encoding='unicode')
            status['robot_descriptions'] = descriptions
            status['asset_transport'] = dict(texture_max_pixels=self.texture_size, jpeg_quality=self.jpeg_quality,
                                             geometry='original local ENU triangles')
            self.active = seen
            size = sum(len(asset['data']) for asset in self.assets.values())
            for key in list(self.assets):
                if size <= self.cache_bytes: break
                if key in seen: continue
                size -= len(self.assets.pop(key)['data'])
                self.counters['evictions'] += 1
            self.files = {key: asset for key, asset in self.files.items() if asset in self.assets}
            status['assets'] = {name: {key: value for key, value in self.assets[name].items() if key != 'data'}
                                for name in sorted(seen)}
            return json.dumps(status, sort_keys=True, separators=(',', ':')).encode()

    def metrics(self):
        with self.lock:
            return dict(self.counters, cache_bytes=sum(len(a['data']) for a in self.assets.values()),
                        cache_target_bytes=self.cache_bytes, cached_assets=len(self.assets), active_assets=len(self.active))


class AssetHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, registry):
        self.registry = registry
        super().__init__(address, AssetHandler)


class AssetHandler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        registry = self.server.registry
        with registry.lock:
            registry.counters['http_requests'] += 1
        try:
            if self.path == '/v1/status':
                data = registry.snapshot()
                etag = '"'+digest(data)+'"'
                if self.headers.get('If-None-Match') == etag:
                    self.send_response(304)
                    self.send_header('ETag', etag)
                    self.end_headers()
                    return
                return self.respond(data, 'application/json', etag=etag)
            if self.path == '/v1/metrics':
                return self.respond(json.dumps(registry.metrics()).encode(), 'application/json')
            if self.path.startswith('/assets/') and ASSET_ID.fullmatch(self.path[8:]):
                with registry.lock:
                    asset = registry.assets.get(self.path[8:])
                    if asset:
                        registry.assets.move_to_end(self.path[8:])
                        registry.counters['asset_requests'] += 1
                if asset:
                    return self.respond(asset['data'], asset['media_type'], compressed=True,
                                        etag='"'+self.path[8:].split('.')[0]+'"', immutable=True)
            self.send_error(404, 'Unknown resource')
        except (OSError, ValueError, KeyError, ET.ParseError):
            self.send_error(503, 'Simulation assets are not available yet')

    def respond(self, data, content_type, compressed=False, etag=None, immutable=False):
        wants_gzip = 'gzip' in self.headers.get('Accept-Encoding', '')
        if compressed and not wants_gzip:
            data = gzip.decompress(data)
        elif not compressed and wants_gzip:
            data = gzip.compress(data, compresslevel=3, mtime=0)
        self.send_response(200)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'private, max-age=86400, immutable' if immutable else 'no-cache')
        if etag: self.send_header('ETag', etag)
        if wants_gzip: self.send_header('Content-Encoding', 'gzip')
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            return
        with self.server.registry.lock:
            self.server.registry.counters['bytes_sent'] += len(data)


class AssetCache:
    def __init__(self, endpoint, directory, cache_bytes=256*1024*1024, timeout=20.):
        url = urllib.parse.urlsplit(endpoint)
        if url.scheme not in {'http', 'https'} or not url.netloc or url.username or url.password or url.query or url.fragment:
            raise ValueError('Asset endpoint must be an HTTP(S) origin without credentials or query parameters')
        self.endpoint = endpoint.rstrip('/')
        self.directory = Path(directory).expanduser().resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.cache_bytes, self.timeout = cache_bytes, timeout
        self.etag = None
        self.pending_etag = None
        self.verified = {}
        self.lock = threading.Lock()
        self.counters = dict(manifest_requests=0, asset_requests=0, bytes_received=0, cache_hits=0, evictions=0)

    def request(self, path, etag=None):
        request = urllib.request.Request(self.endpoint+path, headers={'Accept-Encoding': 'gzip'})
        if etag: request.add_header('If-None-Match', etag)
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                data = response.read(MAX_ASSET+1)
                if len(data)>MAX_ASSET: raise ValueError('Compressed asset exceeds the transfer limit')
                with self.lock: self.counters['bytes_received'] += len(data)
                if response.headers.get('Content-Encoding') == 'gzip':
                    # Bounded decompression also rejects a corrupt gzip stream.
                    import zlib
                    decoder = zlib.decompressobj(16+zlib.MAX_WBITS)
                    data = decoder.decompress(data, MAX_ASSET+1)
                    if len(data)>MAX_ASSET or not decoder.eof:
                        raise ValueError('Asset exceeds the decoded transfer limit or is truncated')
                return data, response.headers.get('ETag')
        except urllib.error.HTTPError as error:
            code = error.code
            error.close()
            if code == 304: return None, etag
            raise RuntimeError('Asset service returned HTTP '+str(code)) from None

    def manifest(self):
        self.counters['manifest_requests'] += 1
        data, etag = self.request('/v1/status', self.etag)
        if data is None: return None
        value = json.loads(data)
        self.pending_etag = etag
        return value

    def fetch(self, name, metadata):
        if not ASSET_ID.fullmatch(name): raise ValueError('Invalid content-addressed asset name')
        path = self.directory/name
        if path.is_file():
            stat = path.stat()
            signature = stat.st_mtime_ns, stat.st_size
            if self.verified.get(name) == signature or (stat.st_size <= MAX_ASSET and digest(path.read_bytes()) == name.split('.')[0]):
                os.utime(path, None)
                stat = path.stat()
                self.verified[name] = stat.st_mtime_ns, stat.st_size
                with self.lock: self.counters['cache_hits'] += 1
                return path
        with self.lock: self.counters['asset_requests'] += 1
        data, _ = self.request('/assets/'+name)
        if data is None or len(data) != metadata['size'] or digest(data) != name.split('.')[0]:
            raise ValueError('Asset checksum or size does not match the manifest')
        temporary = self.directory/(name+'.'+uuid.uuid4().hex+'.tmp')
        try:
            temporary.write_bytes(data)
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)
        stat = path.stat()
        self.verified[name] = stat.st_mtime_ns, stat.st_size
        return path

    def localize(self, manifest):
        assets = manifest.get('assets', {})
        if len(assets)>10000: raise ValueError('Too many active assets in manifest')
        for name in assets:
            if not ASSET_ID.fullmatch(name): raise ValueError('Invalid asset manifest')
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(lambda name: self.fetch(name, assets[name]), assets))
        result = json.loads(json.dumps(manifest))
        for tile in result.get('geometry', []):
            name = tile['uri'].removeprefix('/assets/')
            if name not in assets: raise ValueError('Mesh missing from manifest')
            tile['uri'] = str(self.directory/name)
        for name, description in result.get('robot_descriptions', {}).items():
            root = ET.fromstring(description)
            for element in root.iter():
                value = element.get('filename', '')
                if value.startswith('/assets/'):
                    asset = value[8:]
                    if asset not in assets: raise ValueError('Robot mesh missing from manifest')
                    element.set('filename', (self.directory/asset).as_uri())
            result['robot_descriptions'][name] = ET.tostring(root, encoding='unicode')
        self.prune(set(assets))
        self.etag = self.pending_etag
        return result

    def prune(self, active):
        files = [p for p in self.directory.iterdir() if p.is_file() and ASSET_ID.fullmatch(p.name)]
        size = sum(p.stat().st_size for p in files)
        for path in sorted(files, key=lambda p: p.stat().st_mtime_ns):
            if size <= self.cache_bytes: break
            if path.name in active: continue
            size -= path.stat().st_size
            path.unlink()
            self.verified.pop(path.name, None)
            self.counters['evictions'] += 1
        self.counters['cache_bytes'] = size
        self.counters['cache_target_bytes'] = self.cache_bytes
        self.counters['active_assets'] = len(active)
