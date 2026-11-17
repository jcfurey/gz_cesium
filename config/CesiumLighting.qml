import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
  width: 235
  CheckBox {
    text: "Indirect lighting"
    checked: _CesiumLighting.enabled
    onClicked: _CesiumLighting.enabled = checked
  }
  Label {
    text: "Updates with streamed terrain"
    font.pixelSize: 12
    opacity: 0.75
  }
}
