import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 520
    height: 560
    visible: true
    title: "turret-ua  " + phone.localUri

    readonly property var stateColors: ({
        "Calling": "#b07d00", "Ringing": "#b07d00", "Connected": "#1a7f37",
        "OnHold": "#6e40c9", "Transferring": "#0969da", "Terminated": "#8c959f"
    })

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Label { text: "Line: " + phone.localUri; font.bold: true }

        RowLayout {
            Layout.fillWidth: true
            TextField {
                id: dest
                objectName: "destField"
                Layout.fillWidth: true
                placeholderText: "sip:bob@127.0.0.1:5070"
            }
            Button {
                text: "Dial"
                enabled: dest.text.length > 0
                onClicked: phone.dial(dest.text)
            }
        }

        GridLayout {
            columns: 3
            Repeater {
                model: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"]
                Button {
                    text: modelData
                    Layout.preferredWidth: 56
                    onClicked: dest.insert(dest.cursorPosition, modelData)
                }
            }
        }

        RowLayout {
            Label { text: "Transfer to:" }
            TextField {
                id: xferTarget
                Layout.fillWidth: true
                placeholderText: "sip:carol@127.0.0.1:5090"
            }
        }

        Label { text: phone.activeCalls + " active call(s)" }

        ListView {
            id: calls
            objectName: "callList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: phone
            delegate: Rectangle {
                width: calls.width
                height: 76
                radius: 6
                border.color: "#d0d7de"
                color: callState === "Terminated" ? "#f6f8fa" : "white"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4
                    RowLayout {
                        Label { text: (incoming ? "↙ " : "↗ ") + peer; Layout.fillWidth: true; elide: Text.ElideRight }
                        Label {
                            text: callState + (remoteHold ? " (held by peer)" : "")
                            color: win.stateColors[callState] || "black"
                            font.bold: true
                        }
                    }
                    RowLayout {
                        visible: callState !== "Terminated"
                        Button { text: "Answer"; visible: incoming && callState === "Ringing"; onClicked: phone.answer(callId) }
                        Button { text: "Hold"; visible: callState === "Connected"; onClicked: phone.hold(callId) }
                        Button { text: "Resume"; visible: callState === "OnHold"; onClicked: phone.resume(callId) }
                        Button {
                            text: "Transfer"
                            visible: callState === "Connected" || callState === "OnHold"
                            enabled: xferTarget.text.length > 0
                            onClicked: phone.transfer(callId, xferTarget.text)
                        }
                        Button { text: "Hang up"; onClicked: phone.hangup(callId) }
                    }
                    Label { visible: callState === "Terminated"; text: reason; color: "#57606a" }
                }
            }
        }
    }
}
