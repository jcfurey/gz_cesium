"""Exercise the actual HTTP service using a separate viewer cache."""
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from asset_transport import AssetCache, AssetHandler, AssetHTTPServer, AssetRegistry, digest


class AssetTransportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        source = self.root/'server'
        source.mkdir()
        (source/'texture.png').write_bytes(b'authored texture bytes'*1200)
        (source/'robot.glb').write_bytes(b'authored robot bytes'*1600)
        (source/'private.txt').write_text('This file must never be served.')
        (source/'mesh.dae').write_text('''<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema">
          <library_images><image id="image"><init_from>texture.png</init_from></image></library_images>
          </COLLADA>''')
        self.status_path = source/'status.json'
        self.status = dict(schema=1, ready=True, progress=100., source='ion:2275207', credits=['Authored fixture'],
                           geometry=[dict(id=7, uri=str(source/'mesh.dae'), visuals=[dict(uri='/private/path')])])
        self.status_path.write_text(json.dumps(self.status))
        session = source/'session'
        (session/'rover').mkdir(parents=True)
        (session/'contract.json').write_text(json.dumps(dict(schema=2, backend='gazebo', domain_id=201,
            shared_frame='world', use_sim_time=True, robots=[dict(name='rover', namespace='rover',
            frames=dict(base='rover/base_link'), topics=dict(robot_description='/rover/robot_description'))])))
        (session/'rover/robot.urdf').write_text('<robot name="rover"><link name="base_link"><visual><geometry>'
            '<mesh filename="'+(source/'robot.glb').as_uri()+'"/></geometry></visual></link></robot>')
        self.registry = AssetRegistry(self.status_path, session)
        self.server = None
        self.start_server()
        self.cache = AssetCache(self.endpoint, self.root/'viewer', timeout=2.)

    def start_server(self, port=0):
        self.server = AssetHTTPServer(('127.0.0.1', port), self.registry)
        self.endpoint = 'http://127.0.0.1:'+str(self.server.server_port)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': .01}, daemon=True)
        self.thread.start()

    def stop_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)

    def tearDown(self):
        self.stop_server()
        self.temporary.cleanup()

    def test_separate_viewer_gets_local_hash_verified_meshes_and_urdf_without_source_paths(self):
        manifest = self.cache.manifest()
        self.assertNotIn(str(self.root/'server'), json.dumps(manifest))
        self.assertNotIn('visuals', manifest['geometry'][0])
        localized = self.cache.localize(manifest)
        mesh = Path(localized['geometry'][0]['uri'])
        self.assertEqual(mesh.parent, self.root/'viewer')
        root = ET.parse(mesh)
        self.assertNotIn(b'<ns0:', mesh.read_bytes())
        image = root.find('.//{*}library_images/{*}image/{*}init_from').text
        self.assertTrue((mesh.parent/image).is_file())
        for path in (self.root/'viewer').iterdir():
            self.assertEqual(digest(path.read_bytes()), path.name.split('.')[0])
        urdf = ET.fromstring(localized['robot_descriptions']['rover'])
        self.assertIn((self.root/'viewer').as_uri(), urdf.find('.//mesh').get('filename'))
        self.assertEqual(localized['contract']['transport'], 'network')

    def test_warm_manifest_and_changed_status_do_not_download_assets_again(self):
        self.cache.localize(self.cache.manifest())
        count = self.registry.metrics()['asset_requests']
        self.assertIsNone(self.cache.manifest())
        self.status['progress'] = 99.
        self.status_path.write_text(json.dumps(self.status))
        self.cache.localize(self.cache.manifest())
        self.assertEqual(self.registry.metrics()['asset_requests'], count)
        self.assertGreater(self.cache.counters['cache_hits'], 0)

    def test_failed_transfer_is_retried_even_when_manifest_has_not_changed(self):
        manifest = self.cache.manifest()
        respond = AssetHandler.respond
        def corrupt(handler, data, content_type, **kwargs):
            if handler.path.startswith('/assets/'):
                return respond(handler, b'corrupt transfer', content_type)
            return respond(handler, data, content_type, **kwargs)
        with patch.object(AssetHandler, 'respond', corrupt), self.assertRaises(ValueError):
            self.cache.localize(manifest)
        self.assertIsNone(self.cache.etag)
        self.assertFalse(list((self.root/'viewer').glob('*.tmp')))
        self.assertIsNotNone(self.cache.manifest())
        self.cache.localize(self.cache.manifest())
        self.assertTrue(list((self.root/'viewer').glob('*.dae')))

    def test_reconnection_reuses_the_viewer_cache(self):
        self.cache.localize(self.cache.manifest())
        port = self.server.server_port
        self.stop_server()
        with self.assertRaises(OSError): self.cache.manifest()
        self.start_server(port)
        self.status['progress'] = 98.
        self.status_path.write_text(json.dumps(self.status))
        count = self.registry.metrics()['asset_requests']
        self.cache.localize(self.cache.manifest())
        self.assertEqual(self.registry.metrics()['asset_requests'], count)

    def test_service_does_not_expose_arbitrary_files_or_paths(self):
        self.cache.localize(self.cache.manifest())
        for path in ['/private.txt', '/assets/../private.txt', '/assets/'+('0'*64)+'.txt']:
            with self.subTest(path=path), self.assertRaises(urllib.error.HTTPError) as failure:
                urllib.request.urlopen(self.endpoint+path)
            self.assertEqual(failure.exception.code, 404)
            failure.exception.close()

    def test_eviction_preserves_active_assets_and_unrelated_files(self):
        manifest = self.cache.manifest()
        self.cache.cache_bytes = 1
        self.cache.localize(manifest)
        self.assertGreater(self.cache.counters['cache_bytes'], 1)
        unrelated = self.root/'viewer/original.txt'
        unrelated.write_text('keep')
        self.cache.prune(set())
        self.assertEqual(self.cache.counters['cache_bytes'], 0)
        self.assertEqual(unrelated.read_text(), 'keep')

    def test_viewer_texture_lod_preserves_source_files_and_transparency(self):
        from PIL import Image
        texture = self.root/'server/texture.png'
        Image.new('RGB', (128,256), (20,150,70)).save(texture)
        source = texture.read_bytes()
        self.server.registry = AssetRegistry(self.status_path, self.registry.session, texture_size=64)
        manifest = self.cache.manifest()
        localized = self.cache.localize(manifest)
        self.assertTrue(any(name.endswith('.jpg') for name in manifest['assets']))
        image_path = next((self.root/'viewer').glob('*.jpg'))
        with Image.open(image_path) as image: self.assertEqual(image.size, (32,64))
        self.assertEqual(texture.read_bytes(), source)
        Image.new('RGBA', (128,256), (20,150,70,120)).save(texture)
        self.server.registry = AssetRegistry(self.status_path, self.registry.session, texture_size=64)
        manifest = self.cache.manifest()
        self.cache.localize(manifest)
        images = [name for name in manifest['assets'] if name.endswith('.png')]
        self.assertTrue(images)
        with Image.open(self.root/'viewer'/images[0]) as image:
            self.assertEqual(image.getchannel('A').getextrema(), (120,120))


if __name__ == '__main__':
    unittest.main()
