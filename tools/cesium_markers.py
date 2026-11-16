#!/usr/bin/env python3
"""Show the active Gazebo Cesium meshes and source credits in RViz."""
import argparse
from html.parser import HTMLParser
import json
from pathlib import Path
import textwrap

import rclpy
from rclpy.clock import Clock, ClockType
from rclpy.qos import DurabilityPolicy, QoSProfile
from std_msgs.msg import String
from visualization_msgs.msg import Marker, MarkerArray


class CreditText(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts = []

    def handle_data(self, data):
        self.parts.append(data)

    def handle_starttag(self, tag, attrs):
        if tag == 'img':
            values = dict(attrs)
            self.parts.append(values.get('alt') or values.get('title') or '')


def process_token(pid):
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        return fields[19] if fields[0] != 'Z' else None
    except (OSError, IndexError):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--status', type=Path, required=True)
    parser.add_argument('--frame', default='world')
    parser.add_argument('--watch-pid', type=int)
    args, ros_args = parser.parse_known_args()
    owner = process_token(args.watch_pid) if args.watch_pid else None
    if args.watch_pid and owner is None:
        raise SystemExit('The RViz process has already exited.')
    rclpy.init(args=ros_args)
    node = rclpy.create_node('cesium_rviz_meshes')
    qos = QoSProfile(depth=1, durability=DurabilityPolicy.TRANSIENT_LOCAL)
    publisher = node.create_publisher(MarkerArray, '/cesium/tiles', qos)
    credits = node.create_publisher(String, '/cesium/credits', qos)
    previous = set()
    previous_credits = None

    def update():
        nonlocal previous, previous_credits
        if args.watch_pid and process_token(args.watch_pid) != owner:
            rclpy.try_shutdown()
            return
        try:
            status = json.loads(args.status.read_text())
        except (OSError, ValueError):
            return
        markers, current = [], set()
        stamp = node.get_clock().now().to_msg()
        for tile in status.get('geometry', []):
            path = Path(tile['uri'])
            if not path.is_file():
                continue
            marker = Marker()
            marker.header.frame_id, marker.header.stamp = args.frame, stamp
            marker.ns, marker.id = 'cesium_tiles', tile['id']
            marker.type, marker.action = Marker.MESH_RESOURCE, Marker.ADD
            marker.pose.orientation.w = 1.
            marker.scale.x = marker.scale.y = marker.scale.z = 1.
            marker.color.r = marker.color.g = marker.color.b = marker.color.a = 1.
            marker.mesh_resource = path.resolve().as_uri()
            marker.mesh_use_embedded_materials = True
            markers.append(marker)
            current.add(marker.id)
        for retired in previous-current:
            marker = Marker()
            marker.header.frame_id, marker.header.stamp = args.frame, stamp
            marker.ns, marker.id, marker.action = 'cesium_tiles', retired, Marker.DELETE
            markers.append(marker)
        previous = current
        text = CreditText()
        for credit in status.get('credits', []):
            text.feed(credit)
            text.parts.append(' | ')
        attribution = ' '.join(' '.join(text.parts).split()).strip(' |')
        if status.get('source') == 'ion:2275207' and 'Google' not in attribution:
            attribution = 'Google Maps | '+attribution
        if attribution:
            label = Marker()
            label.header.frame_id, label.header.stamp = args.frame, stamp
            label.ns, label.id, label.type = 'cesium_credits', 0, Marker.TEXT_VIEW_FACING
            label.pose.orientation.w = 1.
            label.pose.position.x, label.pose.position.y, label.pose.position.z = 0., -12., 6.
            label.scale.z = .5
            label.color.r = label.color.g = label.color.b = label.color.a = 1.
            label.text = '\n'.join(textwrap.wrap(attribution, width=100))
            markers.append(label)
        publisher.publish(MarkerArray(markers=markers))
        if attribution != previous_credits:
            credits.publish(String(data=attribution))
            previous_credits = attribution

    node.create_timer(1., update, clock=Clock(clock_type=ClockType.STEADY_TIME))
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.try_shutdown()


if __name__ == '__main__':
    main()
