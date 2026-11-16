#!/usr/bin/env python3
"""Generate a tiny, authored 3D Tiles 1.1 dataset for the native Gazebo adapter.

No external imagery, credentials or Python dependencies are required. Tile and
node transforms deliberately exercise ECEF, glTF Y-up, and a local origin at
Vicksburg. Authored geometry is released under CC0-1.0.
"""
import json
from pathlib import Path
import math
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]


def png():
    def chunk(kind, data):
        return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data))
    colors = [(108, 133, 91), (151, 158, 137), (151, 158, 137), (108, 133, 91)]
    data = b''.join(b'\0'+bytes(sum((list(c) for c in colors[row*2:row*2+2]), [])) for row in range(2))
    return (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 2, 0, 0, 0))
            +chunk(b'IDAT', zlib.compress(data))+chunk(b'IEND', b''))


def glb(path, x0, x1, y0, y1, detailed, edges=(True, True)):
    vertices, normals, texcoords, indices = [], [], [], []

    def quad(points, normal):
        base = len(vertices)
        # ENU to glTF Y-up: inverse of Cesium's Y-up to Z-up transform.
        vertices.extend((x, z, -y) for x, y, z in points)
        normals.extend([(normal[0], normal[2], -normal[1])]*4)
        texcoords.extend([(0, 0), (4, 0), (4, 4), (0, 4)])
        indices.extend([base, base+1, base+2, base, base+2, base+3])

    def box(x, y, sx, sy, sz):
        a, b, c, d = x-sx/2, x+sx/2, y-sy/2, y+sy/2
        quad([(a,c,sz), (b,c,sz), (b,d,sz), (a,d,sz)], (0,0,1))
        quad([(a,c,0), (b,c,0), (b,c,sz), (a,c,sz)], (0,-1,0))
        quad([(b,d,0), (a,d,0), (a,d,sz), (b,d,sz)], (0,1,0))
        quad([(a,d,0), (a,c,0), (a,c,sz), (a,d,sz)], (-1,0,0))
        quad([(b,c,0), (b,d,0), (b,d,sz), (b,c,sz)], (1,0,0))

    quad([(x0,y0,0), (x1,y0,0), (x1,y1,0), (x0,y1,0)], (0,0,1))
    if detailed:
        # Boundary walls and a landmark stay clear of the staging/drive lane.
        box((x0+x1)/2, y0 if edges[1] else y1, x1-x0, .4, 3)
        box(x0 if edges[0] else x1, (y0+y1)/2, .4, y1-y0, 3)
        box((x0+x1)/2, (y0+y1)/2, 6, 7, 5)
    blob = bytearray()
    views, accessors = [], []

    def add(data, component, kind, count, bounds=None):
        while len(blob)%4:
            blob.append(0)
        views.append(dict(buffer=0, byteOffset=len(blob), byteLength=len(data)))
        blob.extend(data)
        accessor = dict(bufferView=len(views)-1, componentType=component, type=kind, count=count)
        if bounds:
            accessor.update(min=bounds[0], max=bounds[1])
        accessors.append(accessor)
        return len(accessors)-1

    positions = add(b''.join(struct.pack('<3f', *v) for v in vertices), 5126, 'VEC3', len(vertices),
                    ([min(v[i] for v in vertices) for i in range(3)],
                     [max(v[i] for v in vertices) for i in range(3)]))
    normal = add(b''.join(struct.pack('<3f', *v) for v in normals), 5126, 'VEC3', len(normals))
    uv = add(b''.join(struct.pack('<2f', *v) for v in texcoords), 5126, 'VEC2', len(texcoords))
    index = add(struct.pack('<'+'I'*len(indices), *indices), 5125, 'SCALAR', len(indices))
    while len(blob)%4:
        blob.append(0)
    image = png()
    views.append(dict(buffer=0, byteOffset=len(blob), byteLength=len(image)))
    blob.extend(image)
    model = dict(asset=dict(version='2.0', copyright='CC0-1.0; Fleet Simulation Workspace authored fixture'),
                 buffers=[dict(byteLength=len(blob))], bufferViews=views, accessors=accessors,
                 images=[dict(bufferView=len(views)-1, mimeType='image/png')], textures=[dict(source=0)],
                 materials=[dict(pbrMetallicRoughness=dict(baseColorTexture=dict(index=0), metallicFactor=0, roughnessFactor=1))],
                 meshes=[dict(primitives=[dict(attributes=dict(POSITION=positions, NORMAL=normal, TEXCOORD_0=uv),
                                              indices=index, material=0)])],
                 nodes=[dict(children=[1]), dict(mesh=0)], scenes=[dict(nodes=[0])], scene=0)
    encoded = json.dumps(model, separators=(',', ':')).encode()
    encoded += b' '*(-len(encoded)%4)
    blob.extend(b'\0'*(-len(blob)%4))
    path.write_bytes(struct.pack('<III', 0x46546c67, 2, 12+8+len(encoded)+8+len(blob))
                     +struct.pack('<I4s', len(encoded), b'JSON')+encoded
                     +struct.pack('<I4s', len(blob), b'BIN\0')+blob)


def main():
    output = ROOT/'fixtures'
    output.mkdir(parents=True, exist_ok=True)
    latitude, longitude = map(math.radians, (32.3645, -90.8431))
    height = 100.
    a, e2 = 6378137., 6.6943799901413165e-3
    n = a/math.sqrt(1-e2*math.sin(latitude)**2)
    x = (n+height)*math.cos(latitude)*math.cos(longitude)
    y = (n+height)*math.cos(latitude)*math.sin(longitude)
    z = (n*(1-e2)+height)*math.sin(latitude)
    east = [-math.sin(longitude), math.cos(longitude), 0., 0.]
    north = [-math.sin(latitude)*math.cos(longitude), -math.sin(latitude)*math.sin(longitude), math.cos(latitude), 0.]
    up = [math.cos(latitude)*math.cos(longitude), math.cos(latitude)*math.sin(longitude), math.sin(latitude), 0.]
    transform = east+north+up+[x,y,z,1.]
    glb(output/'coarse.glb', -64, 64, -64, 64, False)
    children = []
    for number, (cx, cy) in enumerate([(-32,-32), (-32,32), (32,-32), (32,32)]):
        glb(output/f'tile_{number}.glb', -32, 32, -32, 32, True, (cx < 0, cy < 0))
        # Each child's own frame is translated relative to the root ENU frame.
        translated = [1,0,0,0, 0,1,0,0, 0,0,1,0, cx,cy,0,1]
        children.append(dict(transform=translated, boundingVolume=dict(box=[0,0,2.5, 32,0,0, 0,32,0, 0,0,2.5]),
                             geometricError=0, content=dict(uri=f'tile_{number}.glb')))
    tileset = dict(asset=dict(version='1.1'), geometricError=200,
                   root=dict(transform=transform, boundingVolume=dict(box=[0,0,2.5, 64,0,0, 0,64,0, 0,0,2.5]),
                             geometricError=32, refine='REPLACE', content=dict(uri='coarse.glb'), children=children))
    (output/'tileset.json').write_text(json.dumps(tileset, indent=2)+'\n')
    print(f'Generated authored local tileset: {output}')


if __name__ == '__main__':
    main()
