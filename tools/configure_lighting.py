#!/usr/bin/env python3
"""Apply matching streaming-aware Ogre2 GI settings to Cesium world templates."""
import argparse
from pathlib import Path
import xml.etree.ElementTree as ET


def text(parent, name, value):
    child = parent.find(name)
    if child is None:
        child = ET.SubElement(parent,name)
    child.text = str(value)
    return child


def configure(tree, enabled=True, resolution=(128,128,64), bounces=1):
    world = tree.find('world')
    if world is None:
        raise ValueError('Expected one SDF world')
    scene = world.find('scene')
    if scene is None:
        scene = ET.SubElement(world,'scene')
    text(scene,'ambient','.18 .20 .24 1')
    text(scene,'background','.65 .75 .9 1')
    text(scene,'shadows','true')
    name = 'gz_cesium::LightingSystem'
    server = world.find(f"plugin[@name='{name}']")
    if server is None:
        server = ET.SubElement(world,'plugin',filename='gz_cesium_lighting_system',name=name)
    gui = world.find('gui')
    if gui is None:
        gui = ET.SubElement(world,'gui',fullscreen='false')
        view = ET.SubElement(gui,'plugin',filename='MinimalScene',name='3D View')
        text(view,'engine','ogre2')
        text(view,'camera_pose','0 -45 30 0 .6 1.57079632679')
        text(view,'ambient_light','.18 .20 .24 1')
        text(view,'background_color','.65 .75 .9 1')
        for filename,label in [('GzSceneManager','Scene Manager'),
                               ('InteractiveViewControl','View controls'),
                               ('WorldControl','World controls'),('WorldStats','World statistics')]:
            ET.SubElement(gui,'plugin',filename=filename,name=label)
    viewer = gui.find("plugin[@filename='CesiumLighting']")
    if viewer is None:
        viewer = ET.SubElement(gui,'plugin',filename='CesiumLighting',name='Lighting')
    for plugin in [server,viewer]:
        for key,value in [('enabled',str(enabled).lower()),('resolution',' '.join(map(str,resolution))),
                          ('bounce_count',bounces),('rebuild_interval',2.),('settle_time',.5)]:
            text(plugin,key,value)
    return tree


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('worlds',type=Path,nargs='+')
    parser.add_argument('--off',action='store_true')
    parser.add_argument('--resolution',type=int,nargs=3,default=[128,128,64])
    parser.add_argument('--bounces',type=int,default=1)
    args = parser.parse_args()
    if any(n not in [16,32,64,128,256] for n in args.resolution) or not 0<=args.bounces<=4:
        parser.error('Resolution uses powers of two from 16 to 256; bounces range from 0 to 4')
    for path in args.worlds:
        tree = configure(ET.parse(path),not args.off,args.resolution,args.bounces)
        ET.indent(tree,space='  ')
        tree.write(path,encoding='utf-8',xml_declaration=True)
        print('Configured lighting:',path)


if __name__=='__main__': main()
